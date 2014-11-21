/***
  This file is part of systemd.

  Copyright 2008-2012 Kay Sievers <kay@vrfy.org>
  Copyright 2014 Tom Gundersen <teg@jklm.no>

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <ctype.h>
#include <sys/types.h>
#include <net/if.h>
#include <unistd.h>

#include "util.h"
#include "macro.h"
#include "refcnt.h"
#include "path-util.h"
#include "strxcpyx.h"
#include "fileio.h"
#include "prioq.h"
#include "strv.h"

#include "sd-device.h"

#include "device-util.h"
#include "device-internal.h"

struct sd_device_enumerator {
        RefCount n_ref;

        Prioq *devices;
        bool match_modified;
};

_public_ int sd_device_enumerator_new(sd_device_enumerator **ret) {
        _cleanup_device_enumerator_unref_ sd_device_enumerator *enumerator = NULL;

        assert(ret);

        enumerator = new0(sd_device_enumerator, 1);
        if (!enumerator)
                return -ENOMEM;

        enumerator->n_ref = REFCNT_INIT;

        *ret = enumerator;
        enumerator = NULL;

        return 0;
}

_public_ sd_device_enumerator *sd_device_enumerator_ref(sd_device_enumerator *enumerator) {
        assert_return(enumerator, NULL);

        assert_se(REFCNT_INC(enumerator->n_ref) >= 2);

        return enumerator;
}

_public_ sd_device_enumerator *sd_device_enumerator_unref(sd_device_enumerator *enumerator) {
        if (enumerator && REFCNT_DEC(enumerator->n_ref) <= 0) {
                sd_device *device;

                while ((device = prioq_pop(enumerator->devices)))
                        sd_device_unref(device);

                prioq_free(enumerator->devices);

                free(enumerator);
        }

        return NULL;
}

static int device_compare(const void *_a, const void *_b) {
        const sd_device *a = _a, *b = _b;
        const char *devpath_a, *devpath_b, *sound_a;
        bool delay_a = false, delay_b = false;

        assert_se(sd_device_get_devpath(a, &devpath_a) >= 0);
        assert_se(sd_device_get_devpath(b, &devpath_b) >= 0);

        sound_a = strstr(devpath_a, "/sound/card");
        if (sound_a) {
                /* For sound cards the control device must be enumerated last to
                 * make sure it's the final device node that gets ACLs applied.
                 * Applications rely on this fact and use ACL changes on the
                 * control node as an indicator that the ACL change of the
                 * entire sound card completed. The kernel makes this guarantee
                 * when creating those devices, and hence we should too when
                 * enumerating them. */
                sound_a += strlen("/sound/card");
                sound_a = strchr(sound_a, '/');

                if (sound_a) {
                        unsigned prefix_len;

                        prefix_len = sound_a - devpath_a;

                        if (strncmp(devpath_a, devpath_b, prefix_len) == 0) {
                                const char *sound_b;

                                sound_b = devpath_b + prefix_len;

                                if (startswith(sound_a, "/controlC") &&
                                    !startswith(sound_b, "/contolC"))
                                        return 1;

                                if (!startswith(sound_a, "/controlC") &&
                                    startswith(sound_b, "/controlC"))
                                        return -1;
                        }
                }
        }

        /* md and dm devices are enumerated after all other devices */
        if (strstr(devpath_a, "/block/md") || strstr(devpath_a, "/block/dm-"))
                delay_a = true;

        if (strstr(devpath_b, "/block/md") || strstr(devpath_b, "/block/dm-"))
                delay_b = true;

        if (delay_a && !delay_b)
                return 1;

        if (!delay_a && delay_b)
                return -1;

        return strcmp(devpath_a, devpath_b);
}

static int enumerator_scan_dir_and_add_devices(sd_device_enumerator *enumerator, const char *basedir, const char *subdir1, const char *subdir2) {
        _cleanup_closedir_ DIR *dir = NULL;
        char *path;
        struct dirent *dent;
        int r;

        assert(enumerator);
        assert(basedir);
        assert(subdir1);

        if (subdir2)
                path = strappenda("/sys/", basedir, "/", subdir1, "/", subdir2);
        else
                path = strappenda("/sys/", basedir, "/", subdir1);

        log_debug("    device-enumerator: scanning %s", path);

        dir = opendir(path);
        if (!dir)
                return -errno;

        FOREACH_DIRENT(dent, dir, return -errno) {
                _cleanup_device_unref_ sd_device *device = NULL;
                char *syspath;

                syspath = strappenda(path, "/", dent->d_name);

                r = sd_device_new_from_syspath(&device, syspath);
                if (r < 0)
                        continue;

                r = prioq_ensure_allocated(&enumerator->devices, device_compare);
                if (r < 0)
                        return r;

                r = prioq_put(enumerator->devices, device, NULL);
                if (r < 0)
                        return r;

                        log_debug("      device-enumerator: added %s", syspath);

                device = NULL;
        }

        return 0;
}

static int enumerator_scan_dir(sd_device_enumerator *enumerator, const char *basedir, const char *subdir) {
        _cleanup_closedir_ DIR *dir = NULL;
        char *path;
        struct dirent *dent;
        int r;

        path = strappenda("/sys/", basedir);

        dir = opendir(path);
        if (!dir)
                return -errno;

        log_debug("  device-enumerator: scanning %s", path);

        FOREACH_DIRENT(dent, dir, return -errno) {
                r = enumerator_scan_dir_and_add_devices(enumerator, basedir, dent->d_name, subdir);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int enumerator_scan_dirs_all(sd_device_enumerator *enumerator) {
        int r;

        log_debug("device-enumerator: scan all dirs");

        if (access("/sys/subsystem", F_OK) >= 0) {
                /* we have /subsystem/, forget all the old stuff */
                r = enumerator_scan_dir(enumerator, "subsystem", "devices");
                if (r < 0) {
                        log_debug("device-enumerator: failed to scan /sys/subsystem: %s", strerror(-r));
                        return r;
                }
        } else {
                r = enumerator_scan_dir(enumerator, "bus", "devices");
                if (r < 0) {
                        log_debug("device-enumerator: failed to scan /sys/bus: %s", strerror(-r));
                        return r;
                }

                r = enumerator_scan_dir(enumerator, "class", NULL);
                if (r < 0) {
                        log_debug("device-enumerator: failed to scan /sys/class: %s", strerror(-r));
                        return r;
                }
        }

        return 0;
}

_public_ sd_device *sd_device_enumerator_get_device_first(sd_device_enumerator *enumerator) {
        sd_device *device;
        int r;

        assert_return(enumerator, NULL);

        while ((device = prioq_pop(enumerator->devices)))
                sd_device_unref(device);

        r = enumerator_scan_dirs_all(enumerator);
        if (r < 0)
                return NULL;

        enumerator->match_modified = false;

        return prioq_peek(enumerator->devices);
}

_public_ sd_device *sd_device_enumerator_get_device_next(sd_device_enumerator *enumerator) {
        assert_return(enumerator, NULL);

        if (enumerator->match_modified)
                return NULL;

        sd_device_unref(prioq_pop(enumerator->devices));

        return prioq_peek(enumerator->devices);
}
