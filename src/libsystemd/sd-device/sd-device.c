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

#include "util.h"
#include "refcnt.h"
#include "path-util.h"
#include "strxcpyx.h"

#include "sd-device.h"

#include "device-util.h"

struct sd_device {
        RefCount n_ref;

        char *syspath;
        const char *devpath;
        char *sysname;
        const char *sysnum;
};

static int device_new(sd_device **ret) {
        _cleanup_device_unref_ sd_device *device = NULL;

        assert(ret);

        device = new0(sd_device, 1);
        if (!device)
                return -ENOMEM;

        device->n_ref = REFCNT_INIT;

        *ret = device;
        device = NULL;

        return 0;
}

_public_ sd_device *sd_device_ref(sd_device *device) {
        if (device)
                assert_se(REFCNT_INC(device->n_ref) >= 2);

        return device;
}

_public_ sd_device *sd_device_unref(sd_device *device) {
        if (device && REFCNT_DEC(device->n_ref) <= 0) {
                free(device->syspath);
                free(device->sysname);
                free(device);
        }

        return NULL;
}

static int device_set_syspath(sd_device *device, const char *_syspath) {
        _cleanup_free_ char *syspath = NULL, *sysname = NULL;
        const char *devpath, *sysnum;
        const char *pos;
        size_t len = 0;
        int r;

        assert(device);
        assert(_syspath);

        /* must be a subdirectory of /sys */
        if (!path_startswith(_syspath, "/sys/"))
                return -EINVAL;

        r = readlink_and_canonicalize(_syspath, &syspath);
        if (r < 0)
                return r;

        devpath = syspath + strlen("/sys");

        if (path_startswith(devpath,  "/devices/")) {
                char *path;

                /* all 'devices' require an 'uevent' file */
                path = strappenda(syspath, "/uevent");
                r = access(path, F_OK);
                if (r < 0)
                        return -errno;
        } else
                /* everything else just just needs to be a directory */
                if (!is_dir(syspath, false))
                        return -EINVAL;

        pos = strrchr(syspath, '/');
        if (!pos)
                return -EINVAL;
        pos ++;

        /* devpath is not a root directory */
        if (*pos == '\0' || pos <= devpath)
                return -EINVAL;

        sysname = strdup(pos);
        if (!sysname)
                return -ENOMEM;

        /* some devices have '!' in their name, change that to '/' */
        while (sysname[len] != '\0') {
                if (sysname[len] == '!')
                        sysname[len] = '/';

                len ++;
        }

        /* trailing number */
        while (len > 0 && isdigit(sysname[--len]))
                sysnum = &sysname[len];

        if (len == 0)
                sysnum = NULL;

        free(device->syspath);
        device->syspath = syspath;
        syspath = NULL;

        free(device->sysname);
        device->sysname = sysname;
        sysname = NULL;

        device->devpath = devpath;
        device->sysnum = sysnum;

        return 0;
}

_public_ int sd_device_new_from_syspath(sd_device **ret, const char *syspath) {
        _cleanup_device_unref_ sd_device *device = NULL;
        int r;

        assert_return(ret, -EINVAL);
        assert_return(syspath, -EINVAL);

        r = device_new(&device);
        if (r < 0)
                return r;

        r = device_set_syspath(device, syspath);
        if (r < 0)
                return r;

        *ret = device;
        device = NULL;

        return 0;
}
