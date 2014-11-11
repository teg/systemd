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

#include "util.h"
#include "macro.h"
#include "refcnt.h"
#include "path-util.h"
#include "strxcpyx.h"
#include "fileio.h"
#include "hashmap.h"
#include "set.h"

#include "sd-device.h"

#include "device-util.h"

struct sd_device {
        RefCount n_ref;

        sd_device *parent;
        OrderedHashmap *properties;
        Set *tags;

        char *syspath;
        const char *devpath;
        char *sysname;
        const char *sysnum;

        char *devtype;
        int ifindex;
        char *devnode;
        dev_t devnum;

        char *subsystem;
        char *driver;

        char *id_filename;

        bool is_initialized;
        uint64_t usec_initialized;

        bool uevent_loaded;
        bool db_loaded;
        bool parent_set;
        bool subsystem_set;
        bool driver_set;

        bool tags_uptodate;
};

static int device_new(sd_device **ret) {
        _cleanup_device_unref_ sd_device *device = NULL;

        assert(ret);

        device = new0(sd_device, 1);
        if (!device)
                return -ENOMEM;

        device->n_ref = REFCNT_INIT;
        device->tags_uptodate = true;

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
                sd_device_unref(device->parent);
                free(device->syspath);
                free(device->sysname);
                free(device->devtype);
                free(device->devnode);
                free(device->subsystem);
                free(device->driver);
                free(device->id_filename);

                ordered_hashmap_free_free_free(device->properties);

                set_free_free(device->tags);

                free(device);
        }

        return NULL;
}

static int device_add_property(sd_device *device, const char *_key, const char *_value) {
        assert(device);
        assert(_key);

        if (_value) {
                _cleanup_free_ char *key = NULL, *value = NULL, *old_key = NULL, *old_value = NULL;
                int r;

                r = ordered_hashmap_ensure_allocated(&device->properties, &string_hash_ops);
                if (r < 0)
                        return r;

                key = strdup(_key);
                if (!key)
                        return -ENOMEM;

                value = strdup(_value);
                if (!value)
                        return -ENOMEM;

                old_value = ordered_hashmap_get2(device->properties, key, (void**) &old_key);

                r = ordered_hashmap_replace(device->properties, key, value);
                if (r < 0)
                        return r;

                key = NULL;
                value = NULL;
        } else {
                _cleanup_free_ char *key = NULL;
                _cleanup_free_ char *value = NULL;

                value = ordered_hashmap_remove2(device->properties, _key, (void**) &key);
        }

        return 0;
}

static int device_add_tag(sd_device *device, const char *tag) {
        int r;

        assert(device);
        assert(tag);

        r = set_ensure_allocated(&device->tags, &string_hash_ops);
        if (r < 0)
                return r;

        r = set_put_strdup(device->tags, tag);
        if (r < 0)
                return r;

        device->tags_uptodate = false;

        return 0;
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

        r = device_add_property(device, "DEVPATH", devpath);
        if (r < 0)
                return r;

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

_public_ int sd_device_get_syspath(sd_device *device, const char **ret) {
        assert_return(device, -EINVAL);
        assert_return(ret, -EINVAL);

        assert(path_startswith(device->syspath, "/sys/"));

        *ret = device->syspath;

        return 0;
}

static int device_set_ifindex(sd_device *device, const char *ifindex_str) {
        int ifindex, r;

        assert(device);
        assert(ifindex_str);

        r = safe_atoi(ifindex_str, &ifindex);
        if (r < 0)
                return r;

        if (ifindex <= 0)
                return -EINVAL;

        r = device_add_property(device, "IFINDEX", ifindex_str);
        if (r < 0)
                return r;

        device->ifindex = ifindex;

        return 0;
}

static int device_set_devnode(sd_device *device, const char *_devnode) {
        _cleanup_free_ char *devnode = NULL;
        int r;

        assert(device);
        assert(_devnode);

        if (_devnode[0] != '/') {
                r = asprintf(&devnode, "/dev/%s", _devnode);
                if (r < 0)
                        return -ENOMEM;
        } else {
                devnode = strdup(_devnode);
                if (!devnode)
                        return -ENOMEM;
        }

        r = device_add_property(device, "DEVNAME", devnode);
        if (r < 0)
                return r;

        free(device->devnode);
        device->devnode = devnode;
        devnode = NULL;

        return 0;
}

static int device_set_devtype(sd_device *device, const char *_devtype) {
        _cleanup_free_ char *devtype = NULL;
        int r;

        assert(device);
        assert(_devtype);

        devtype = strdup(_devtype);
        if (!devtype)
                return -ENOMEM;

        r = device_add_property(device, "DEVTYPE", devtype);
        if (r < 0)
                return r;

        free(device->devtype);
        device->devtype = devtype;
        devtype = NULL;

        return 0;
}

static int device_set_devnum(sd_device *device, const char *major, const char *minor) {
        unsigned maj = 0, min = 0;
        int r;

        assert(device);
        assert(major);

        r = safe_atou(major, &maj);
        if (r < 0)
                return r;
        if (!maj)
                return 0;

        if (minor) {
                r = safe_atou(minor, &min);
                if (r < 0)
                        return r;
        }

        r = device_add_property(device, "MAJOR", major);
        if (r < 0)
                return r;

        if (minor) {
                r = device_add_property(device, "MINOR", minor);
                if (r < 0)
                        return r;
        }

        device->devnum = makedev(maj, min);

        return 0;
}

static int handle_uevent_line(sd_device *device, const char *key, const char *value, const char **major, const char **minor) {
        int r;

        assert(device);
        assert(key);
        assert(value);

        if (streq(key, "MAJOR"))
                *major = value;
        else if (streq(key, "MINOR"))
                *minor = value;
        else if (streq(key, "DEVTYPE")) {
                r = device_set_devtype(device, value);
                if (r < 0)
                        return r;
        } else if (streq(key, "IFINDEX")) {
                r = device_set_ifindex(device, value);
                if (r < 0)
                        return r;
        } else if (streq(key, "DEVNAME")) {
                r = device_set_devnode(device, value);
                if (r < 0)
                        return r;
        } else {
                r = device_add_property(device, key, value);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int device_read_uevent_file(sd_device *device) {
        _cleanup_free_ char *uevent = NULL;
        const char *syspath, *key, *value, *major = NULL, *minor = NULL;
        char *path;
        size_t uevent_len;
        unsigned i;
        int r;

        enum {
                PRE_KEY,
                KEY,
                PRE_VALUE,
                VALUE,
                INVALID_LINE,
        } state = PRE_KEY;

        assert(device);

        if (device->uevent_loaded)
                return 0;

        r = sd_device_get_syspath(device, &syspath);
        if (r < 0)
                return r;

        path = strappenda(syspath, "/uevent");

        r = read_full_file(path, &uevent, &uevent_len);
        if (r < 0) {
                log_debug("sd-device: failed to read uevent file '%s': %s", path, strerror(-r));
                return r;
        }

        for (i = 0; i < uevent_len; i++) {
                switch (state) {
                case PRE_KEY:
                        if (!strchr(NEWLINE, uevent[i])) {
                                key = &uevent[i];

                                state = KEY;
                        }

                        break;
                case KEY:
                        if (uevent[i] == '=') {
                                uevent[i] = '\0';

                                state = PRE_VALUE;
                        } else if (strchr(NEWLINE, uevent[i])) {
                                uevent[i] = '\0';
                                log_debug("sd-device: ignoring invalid uevent line '%s'", key);

                                state = PRE_KEY;
                        }

                        break;
                case PRE_VALUE:
                        value = &uevent[i];

                        state = VALUE;

                        break;
                case VALUE:
                        if (strchr(NEWLINE, uevent[i])) {
                                uevent[i] = '\0';

                                r = handle_uevent_line(device, key, value, &major, &minor);
                                if (r < 0)
                                        log_debug("sd-device: failed to handle uevent entry '%s=%s': %s", key, value, strerror(-r));

                                state = PRE_KEY;
                        }

                        break;
                default:
                        assert_not_reached("invalid state when parsing uevent file");
                }
        }

        if (major) {
                r = device_set_devnum(device, major, minor);
                if (r < 0)
                        log_debug("sd-device: could not set 'MAJOR=%s' or 'MINOR=%s' from '%s': %s", major, minor, path, strerror(-r));
        }

        device->uevent_loaded = true;

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

_public_ int sd_device_new_from_devnum(sd_device **ret, char type, dev_t devnum) {
        char *syspath;
        char id[DECIMAL_STR_MAX(unsigned) * 2 + 1];

        assert_return(ret, -EINVAL);
        assert_return(type == 'b' || type == 'c', -EINVAL);

        /* use /sys/dev/{block,char}/<maj>:<min> link */
        snprintf(id, sizeof(id), "%u:%u", major(devnum), minor(devnum));

        syspath = strappenda("/sys/dev/", (type == 'b' ? "block" : "char"), "/", id);

        return sd_device_new_from_syspath(ret, syspath);
}

_public_ int sd_device_new_from_subsystem_sysname(sd_device **ret, const char *subsystem, const char *sysname) {
        char *syspath;

        assert_return(ret, -EINVAL);
        assert_return(subsystem, -EINVAL);
        assert_return(sysname, -EINVAL);

        if (streq(subsystem, "subsystem")) {
                syspath = strappenda("/sys/subsystem/", sysname);
                if (access(syspath, F_OK) >= 0)
                        return sd_device_new_from_syspath(ret, syspath);

                syspath = strappenda("/sys/bus/", sysname);
                if (access(syspath, F_OK) >= 0)
                        return sd_device_new_from_syspath(ret, syspath);

                syspath = strappenda("/sys/class/", sysname);
                if (access(syspath, F_OK) >= 0)
                        return sd_device_new_from_syspath(ret, syspath);
        } else  if (streq(subsystem, "module")) {
                syspath = strappenda("/sys/module/", sysname);
                if (access(syspath, F_OK) >= 0)
                        return sd_device_new_from_syspath(ret, syspath);
        } else if (streq(subsystem, "drivers")) {
                char subsys[PATH_MAX];
                char *driver;

                strscpy(subsys, sizeof(subsys), sysname);
                driver = strchr(subsys, ':');
                if (driver) {
                        driver[0] = '\0';
                        driver++;

                        syspath = strappenda("/sys/subsystem/", subsys, "/drivers/", driver);
                        if (access(syspath, F_OK) >= 0)
                                return sd_device_new_from_syspath(ret, syspath);

                        syspath = strappenda("/sys/bus/", subsys, "/drivers/", driver);
                        if (access(syspath, F_OK) >= 0)
                                return sd_device_new_from_syspath(ret, syspath);
                } else
                        return -EINVAL;
        } else {
                syspath = strappenda("/sys/subsystem/", subsystem, "/devices/", sysname);
                if (access(syspath, F_OK) >= 0)
                        return sd_device_new_from_syspath(ret, syspath);

                syspath = strappenda("/sys/bus/", subsystem, "/devices/", sysname);
                if (access(syspath, F_OK) >= 0)
                        return sd_device_new_from_syspath(ret, syspath);

                syspath = strappenda("/sys/class/", subsystem, "/", sysname);
                if (access(syspath, F_OK) >= 0)
                        return sd_device_new_from_syspath(ret, syspath);
        }

        return -ENOENT;
}

static int device_get_ifindex(sd_device *device, int *ifindex) {
        int r;

        assert(device);
        assert(ifindex);

        r = device_read_uevent_file(device);
        if (r < 0)
                return r;

        *ifindex = device->ifindex;

        return 0;
}

_public_ int sd_device_new_from_device_id(sd_device **ret, const char *id) {
        int r;

        assert_return(ret, -EINVAL);
        assert_return(id, -EINVAL);

        switch (id[0]) {
        case 'b':
        case 'c':
        {
                char type;
                int maj, min;

                r = sscanf(id, "%c%i:%i", &type, &maj, &min);
                if (r != 3)
                        return -EINVAL;

                return sd_device_new_from_devnum(ret, type, makedev(maj, min));
        }
        case 'n':
        {
                _cleanup_device_unref_ sd_device *device = NULL;
                _cleanup_close_ int sk = -1;
                struct ifreq ifr = {};
                int ifindex;

                r = safe_atoi(&id[1], &ifr.ifr_ifindex);
                if (r < 0)
                        return r;
                else if (ifr.ifr_ifindex <= 0)
                        return -EINVAL;

                sk = socket(PF_INET, SOCK_DGRAM, 0);
                if (sk < 0)
                        return -errno;

                r = ioctl(sk, SIOCGIFNAME, &ifr);
                if (r < 0)
                        return -errno;

                r = sd_device_new_from_subsystem_sysname(&device, "net", ifr.ifr_name);
                if (r < 0)
                        return r;

                r = device_get_ifindex(device, &ifindex);
                if (r < 0)
                        return r;

                /* this si racey, so we might end up with the wrong device */
                if (ifr.ifr_ifindex != ifindex)
                        return -ENODEV;

                *ret = device;
                device = NULL;

                return 0;
        }
        case '+':
        {
                char subsys[PATH_MAX];
                char *sysname;

                (void)strscpy(subsys, sizeof(subsys), id + 1);
                sysname = strchr(subsys, ':');
                if (!sysname)
                        return -EINVAL;

                sysname[0] = '\0';
                sysname ++;

                return sd_device_new_from_subsystem_sysname(ret, subsys, sysname);
        }
        default:
                return -EINVAL;
        }
}

static int device_new_from_child(sd_device **ret, sd_device *child) {
        _cleanup_free_ char *path = NULL;
        const char *subdir, *syspath;
        int r;

        assert(ret);
        assert(child);

        r = sd_device_get_syspath(child, &syspath);
        if (r < 0)
                return r;

        path = strdup(syspath);
        if (!path)
                return -ENOMEM;
        subdir = path + strlen("/sys");

        for (;;) {
                char *pos;

                pos = strrchr(subdir, '/');
                if (!pos || pos < subdir + 2)
                        break;

                *pos = '\0';

                r = sd_device_new_from_syspath(ret, path);
                if (r < 0)
                        continue;

                return 0;
        }

        return -ENOENT;
}

_public_ int sd_device_get_parent(sd_device *child, sd_device **ret) {

        assert_return(ret, -EINVAL);
        assert_return(child, -EINVAL);

        if (child->parent_set) {
                child->parent_set = true;

                (void)device_new_from_child(&child->parent, child);
        }

        if (!child->parent)
                return -ENOENT;

        *ret = child->parent;

        return 0;
}

static int device_set_subsystem(sd_device *device, const char *_subsystem) {
        _cleanup_free_ char *subsystem = NULL;
        int r;

        assert(device);
        assert(_subsystem);

        subsystem = strdup(_subsystem);
        if (!subsystem)
                return -ENOMEM;

        r = device_add_property(device, "SUBSYSTEM", subsystem);
        if (r < 0)
                return r;

        free(device->subsystem);
        device->subsystem = subsystem;
        subsystem = NULL;

        device->subsystem_set = true;

        return 0;
}

_public_ int sd_device_get_subsystem(sd_device *device, const char **ret) {
        assert_return(ret, -EINVAL);
        assert_return(device, -EINVAL);

        if (!device->subsystem_set) {
                _cleanup_free_ char *subsystem = NULL;
                const char *syspath;
                char *path;
                int r;

                /* read 'subsystem' link */
                r = sd_device_get_syspath(device, &syspath);
                if (r < 0)
                        return r;

                path = strappenda(syspath, "/subsystem");
                r = readlink_value(path, &subsystem);
                if (r >= 0)
                        r = device_set_subsystem(device, subsystem);
                /* use implicit names */
                else if (path_startswith(device->devpath, "/module/"))
                        r = device_set_subsystem(device, "module");
                else if (strstr(device->devpath, "/drivers/"))
                        r = device_set_subsystem(device, "drivers");
                else if (path_startswith(device->devpath, "/subsystem/") ||
                         path_startswith(device->devpath, "/class/") ||
                         path_startswith(device->devpath, "/buss/"))
                        r = device_set_subsystem(device, "subsystem");
                if (r < 0)
                        return r;

                device->subsystem_set = true;
        }

        *ret = device->subsystem;

        return 0;
}

_public_ int sd_device_get_devtype(sd_device *device, const char **devtype) {
        int r;

        assert(devtype);
        assert(device);

        r = device_read_uevent_file(device);
        if (r < 0)
                return r;

        *devtype = device->devtype;

        return 0;
}

_public_ int sd_device_get_parent_with_subsystem_devtype(sd_device *child, const char *subsystem, const char *devtype, sd_device **ret) {
        sd_device *parent = NULL;
        int r;

        assert_return(child, -EINVAL);
        assert_return(subsystem, -EINVAL);

        r = sd_device_get_parent(child, &parent);
        while (r >= 0) {
                const char *parent_subsystem;
                const char *parent_devtype;

                (void)sd_device_get_subsystem(parent, &parent_subsystem);
                if (streq_ptr(parent_subsystem, subsystem)) {
                        if (!devtype)
                                break;

                        (void)sd_device_get_devtype(parent, &parent_devtype);
                        if (streq_ptr(parent_devtype, devtype))
                                break;
                }
                r = sd_device_get_parent(parent, &parent);
        }

        if (r < 0)
                return r;

        *ret = parent;

        return 0;
}

_public_ int sd_device_get_devnum(sd_device *device, dev_t *devnum) {
        int r;

        assert_return(device, -EINVAL);
        assert_return(devnum, -EINVAL);

        r = device_read_uevent_file(device);
        if (r < 0)
                return r;

        *devnum = device->devnum;

        return 0;
}

static int device_set_driver(sd_device *device, const char *_driver) {
        char *driver;
        int r;

        assert(device);
        assert(_driver);

        driver = strdup(_driver);
        if (!driver)
                return -ENOMEM;

        r = device_add_property(device, "DRIVER", driver);
        if (r < 0)
                return r;

        free(device->driver);
        device->driver = driver;

        device->driver_set = true;

        return 0;
}

_public_ int sd_device_get_driver(sd_device *device, const char **ret) {
        assert_return(device, -EINVAL);
        assert_return(ret, -EINVAL);

        if (!device->driver_set) {
                _cleanup_free_ char *driver = NULL;
                const char *syspath;
                char *path;
                int r;

                r = sd_device_get_syspath(device, &syspath);
                if (r < 0)
                        return r;

                path = strappenda(syspath, "/driver");
                r = readlink_value(path, &driver);
                if (r >= 0) {
                        r = device_set_driver(device, driver);
                        if (r < 0)
                                return r;
                }
        }

        *ret = device->driver;

        return 0;
}

static int device_get_id_filename(sd_device *device, const char **ret) {
        assert(device);
        assert(ret);

        if (!device->id_filename) {
                _cleanup_free_ char *id = NULL;
                const char *subsystem;
                dev_t devnum;
                int ifindex, r;

                r = sd_device_get_subsystem(device, &subsystem);
                if (r < 0)
                        return r;

                r = sd_device_get_devnum(device, &devnum);
                if (r < 0)
                        return r;

                r = device_get_ifindex(device, &ifindex);
                if (r < 0)
                        return r;

                if (major(devnum) > 0) {
                        /* use dev_t -- b259:131072, c254:0 */
                        r = asprintf(&id, "%c%u:%u",
                                     streq(subsystem, "block") ? 'b' : 'c',
                                     major(devnum), minor(devnum));
                        if (r < 0)
                                return -errno;
                } else if (ifindex > 0) {
                        /* use netdev ifindex -- n3 */
                        r = asprintf(&id, "n%u", ifindex);
                        if (r < 0)
                                return -errno;
                } else {
                        /* use $subsys:$sysname -- pci:0000:00:1f.2
                         * sysname() has '!' translated, get it from devpath
                         */
                        const char *sysname;

                        sysname = basename(device->devpath);
                        if (!sysname)
                                return -EINVAL;

                        r = asprintf(&id, "+%s:%s", subsystem, sysname);
                        if (r < 0)
                                return -errno;
                }

                device->id_filename = id;
                id = NULL;
        }

        *ret = device->id_filename;

        return 0;
}

static int device_add_property_from_string(sd_device *device, const char *str) {
        _cleanup_free_ char *key = NULL;
        char *value;

        assert(device);
        assert(str);

        key = strdup(str);
        if (!key)
                return -ENOMEM;

        value = strchr(key, '=');
        if (!value)
                return -EINVAL;

        *value = '\0';

        if (isempty(++value))
                value = NULL;

        return device_add_property(device, key, value);
}

static int device_set_usec_initialized(sd_device *device, const char *initialized) {
        uint64_t usec_initialized;
        int r;

        assert(device);
        assert(initialized);

        r = safe_atou64(initialized, &usec_initialized);
        if (r < 0)
                return r;

        r = device_add_property(device, "USEC_INITIALIZED", initialized);
        if (r < 0)
                return r;

        device->usec_initialized = usec_initialized;

        return 0;
}

static int handle_db_line(sd_device *device, char key, const char *value) {
        int r;

        assert(device);
        assert(value);

        switch (key) {
        case 'E':
                r = device_add_property_from_string(device, value);
                if (r < 0)
                        return r;

                break;
        case 'G':
                r = device_add_tag(device, value);
                if (r < 0)
                        return r;

                break;
        case 'I':
                r = device_set_usec_initialized(device, value);
                if (r < 0)
                        return r;

                break;
        case 'L':
        case 'W':
                /* TODO */
                break;
        default:
                log_debug("device db: unknown key '%c'", key);
        }

        return 0;
}

static int device_read_db(sd_device *device) {
        _cleanup_free_ char *db = NULL;
        char *path;
        const char *id, *value;
        char key;
        size_t db_len;
        unsigned i;
        int r;

        enum {
                PRE_KEY,
                KEY,
                PRE_VALUE,
                VALUE,
                INVALID_LINE,
        } state = PRE_KEY;

        if (device->db_loaded)
                return 0;

        r = device_get_id_filename(device, &id);
        if (r < 0)
                return r;

        path = strappenda("/run/udev/data/", id);

        r = read_full_file(path, &db, &db_len);
        if (r < 0) {
                if (r == -ENOENT)
                        return 0;
                else {
                        log_debug("sd-device: failed to read db '%s': %s", path, strerror(-r));
                        return r;
                }
        }

        /* devices with a database entry are initialized */
        device->is_initialized = true;

        for (i = 0; i < db_len; i++) {
                switch (state) {
                case PRE_KEY:
                        if (!strchr(NEWLINE, db[i])) {
                                key = db[i];

                                state = KEY;
                        }

                        break;
                case KEY:
                        if (db[i] != ':') {
                                log_debug("sd-device: ignoring invalid db entry with key '%c'", key);

                                state = INVALID_LINE;
                        } else {
                                db[i] = '\0';

                                state = PRE_VALUE;
                        }

                        break;
                case PRE_VALUE:
                        value = &db[i];

                        state = VALUE;

                        break;
                case INVALID_LINE:
                        if (strchr(NEWLINE, db[i]))
                                state = PRE_KEY;

                        break;
                case VALUE:
                        if (strchr(NEWLINE, db[i])) {
                                db[i] = '\0';
                                r = handle_db_line(device, key, value);
                                if (r < 0)
                                        log_debug("sd-device: failed to handle db entry '%c:%s': %s", key, value, strerror(-r));

                                state = PRE_KEY;
                        }

                        break;
                default:
                        assert_not_reached("invalid state when parsing db");
                }
        }

        device->db_loaded = true;

        return 0;
}
