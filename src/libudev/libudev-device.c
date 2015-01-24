/***
  This file is part of systemd.

  Copyright 2008-2012 Kay Sievers <kay@vrfy.org>
  Copyright 2015 Tom Gundersen <teg@jklm.no>

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

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <ctype.h>
#include <net/if.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/sockios.h>

#include "sd-device.h"

#include "libudev.h"
#include "libudev-private.h"

static int udev_device_set_devnode(struct udev_device *udev_device, const char *devnode);

/**
 * SECTION:libudev-device
 * @short_description: kernel sys devices
 *
 * Representation of kernel sys devices. Devices are uniquely identified
 * by their syspath, every device has exactly one path in the kernel sys
 * filesystem. Devices usually belong to a kernel subsystem, and have
 * a unique name inside that subsystem.
 */

/**
 * udev_device:
 *
 * Opaque object representing one kernel sys device.
 */
struct udev_device {
        struct udev *udev;

        /* real device object */
        sd_device *device;

        /* legacy */
        int refcount;

        struct udev_device *parent;
        bool parent_set;

        char *action;
        char *devpath_old;
        unsigned long long int seqnum;
        usec_t usec_initialized;
        char **envp;
        struct udev_list event_properties;
        bool envp_uptodate;
};

/**
 * udev_device_get_seqnum:
 * @udev_device: udev device
 *
 * This is only valid if the device was received through a monitor. Devices read from
 * sys do not have a sequence number.
 *
 * Returns: the kernel event sequence number, or 0 if there is no sequence number available.
 **/
_public_ unsigned long long int udev_device_get_seqnum(struct udev_device *udev_device)
{
        if (udev_device == NULL)
                return 0;
        return udev_device->seqnum;
}

static int udev_device_set_seqnum(struct udev_device *udev_device, unsigned long long int seqnum)
{
        char num[32];

        udev_device->seqnum = seqnum;
        snprintf(num, sizeof(num), "%llu", seqnum);
        udev_device_add_property(udev_device, "SEQNUM", num);
        return 0;
}

int udev_device_get_ifindex(struct udev_device *udev_device)
{
        int r, ifindex;

        if (udev_device == NULL)
                return -EINVAL;

        r = sd_device_get_ifindex(udev_device->device, &ifindex);
        if (r < 0)
                return r;

        return ifindex;
}

/**
 * udev_device_get_devnum:
 * @udev_device: udev device
 *
 * Get the device major/minor number.
 *
 * Returns: the dev_t number.
 **/
_public_ dev_t udev_device_get_devnum(struct udev_device *udev_device)
{
        dev_t devnum;
        int r;

        if (udev_device == NULL)
                return makedev(0, 0);

        r = sd_device_get_devnum(udev_device->device, &devnum);
        if (r < 0) {
                errno = -r;
                return makedev(0, 0);
        }

        return devnum;
}

const char *udev_device_get_devpath_old(struct udev_device *udev_device)
{
        return udev_device->devpath_old;
}

static int udev_device_set_devpath_old(struct udev_device *udev_device, const char *devpath_old)
{
        const char *pos;

        free(udev_device->devpath_old);
        udev_device->devpath_old = strdup(devpath_old);
        if (udev_device->devpath_old == NULL)
                return -ENOMEM;
        udev_device_add_property(udev_device, "DEVPATH_OLD", udev_device->devpath_old);

        pos = strrchr(udev_device->devpath_old, '/');
        if (pos == NULL)
                return -EINVAL;
        return 0;
}

/**
 * udev_device_get_driver:
 * @udev_device: udev device
 *
 * Get the kernel driver name.
 *
 * Returns: the driver name string, or #NULL if there is no driver attached.
 **/
_public_ const char *udev_device_get_driver(struct udev_device *udev_device)
{
        const char *driver;
        int r;

        if (udev_device == NULL)
                return NULL;

        r = sd_device_get_driver(udev_device->device, &driver);
        if (r < 0) {
                errno = -r;
                return NULL;
        }

        return driver;
}

/**
 * udev_device_get_devtype:
 * @udev_device: udev device
 *
 * Retrieve the devtype string of the udev device.
 *
 * Returns: the devtype name of the udev device, or #NULL if it can not be determined
 **/
_public_ const char *udev_device_get_devtype(struct udev_device *udev_device)
{
        const char *devtype;
        int r;

        if (udev_device == NULL)
                return NULL;

        r = sd_device_get_devtype(udev_device->device, &devtype);
        if (r < 0) {
                errno = -r;
                return NULL;
        }

        return devtype;
}

/**
 * udev_device_get_subsystem:
 * @udev_device: udev device
 *
 * Retrieve the subsystem string of the udev device. The string does not
 * contain any "/".
 *
 * Returns: the subsystem name of the udev device, or #NULL if it can not be determined
 **/
_public_ const char *udev_device_get_subsystem(struct udev_device *udev_device)
{
        const char *subsystem;
        int r;

        if (udev_device == NULL)
                return NULL;

        r = sd_device_get_subsystem(udev_device->device, &subsystem);
        if (r < 0) {
                errno = -r;
                return NULL;
        }

        return subsystem;
}

mode_t udev_device_get_devnode_mode(struct udev_device *udev_device)
{
        mode_t mode;
        int r;

        if (udev_device == NULL) {
                errno = EINVAL;
                return 0;
        }

        r = device_get_devnode_mode(udev_device->device, &mode);
        if (r < 0) {
                errno = -r;
                return 0;
        }

        return mode;
}

uid_t udev_device_get_devnode_uid(struct udev_device *udev_device)
{
        uid_t uid;
        int r;

        if (udev_device == NULL) {
                errno = EINVAL;
                return 0;
        }

        r = device_get_devnode_uid(udev_device->device, &uid);
        if (r < 0) {
                errno = -r;
                return 0;
        }

        return uid;
}

gid_t udev_device_get_devnode_gid(struct udev_device *udev_device)
{
        gid_t gid;
        int r;

        if (udev_device == NULL) {
                errno = EINVAL;
                return 0;
        }

        r = device_get_devnode_gid(udev_device->device, &gid);
        if (r < 0) {
                errno = -r;
                return 0;
        }

        return gid;
}

struct udev_list_entry *udev_device_add_event_property(struct udev_device *udev_device, const char *key, const char *value)
{
        udev_device->envp_uptodate = false;
        if (value == NULL) {
                struct udev_list_entry *list_entry;

                list_entry = udev_device_get_event_properties_entry(udev_device);
                list_entry = udev_list_entry_get_by_name(list_entry, key);
                if (list_entry != NULL)
                        udev_list_entry_delete(list_entry);
                return NULL;
        }
        return udev_list_entry_add(&udev_device->event_properties, key, value);
}

/**
 * udev_device_get_property_value:
 * @udev_device: udev device
 * @key: property name
 *
 * Get the value of a given property.
 *
 * Returns: the property string, or #NULL if there is no such property.
 **/
_public_ const char *udev_device_get_property_value(struct udev_device *udev_device, const char *key)
{
        const char *value = NULL;

        if (udev_device == NULL || key == NULL) {
                errno = EINVAL;
                return NULL;
        }

        r = sd_device_get_property_value(udev_device, key, &value);
        if (r == -ENOENT) {
                if (streq(key, "OLD_DEVPATH"))
                        value = udev_device->old_devpath;
                else if (streq(key, "ACTION"))
                        value = udev_device->action;
                else if (streq(key, "SEQNUM"))
                        value = udev_device->seqnum_str;
                else if (streq(key, "USEC_INITIALIZED"))
                        value = udev_device->usec_initialized_str;

                if (value == NULL)
                        errno = ENOENT;

                return value;
        } else if (r < 0) {
                errno = -r;
                return NULL;
        }

        return value;
}

struct udev_device *udev_device_new(struct udev *udev)
{
        struct udev_device *udev_device;

        if (udev == NULL) {
                errno = EINVAL;
                return NULL;
        }

        udev_device = new0(struct udev_device, 1);
        if (udev_device == NULL) {
                errno = ENOMEM;
                return NULL;
        }
        udev_device->refcount = 1;
        udev_device->udev = udev;
        udev_list_init(udev, &udev_device->event_properties, true);

        return udev_device;
}

/**
 * udev_device_new_from_syspath:
 * @udev: udev library context
 * @syspath: sys device path including sys directory
 *
 * Create new udev device, and fill in information from the sys
 * device and the udev database entry. The syspath is the absolute
 * path to the device, including the sys mount point.
 *
 * The initial refcount is 1, and needs to be decremented to
 * release the resources of the udev device.
 *
 * Returns: a new udev device, or #NULL, if it does not exist
 **/
_public_ struct udev_device *udev_device_new_from_syspath(struct udev *udev, const char *syspath)
{
        struct udev_device *udev_device;
        int r;

        if (udev == NULL) {
                errno = EINVAL;
                return NULL;
        }

        if (syspath == NULL) {
                errno = EINVAL;
                return NULL;
        }

        udev_device = udev_device_new(udev);
        if (udev_device == NULL) {
                errno = ENOMEM;
                return NULL;
        }

        r = sd_device_new_from_syspath(&udev_device->device, syspath);
        if (r < 0) {
                errno = -r;
                udev_device_unref(udev_device);
                return NULL;
        }

        return udev_device;
}

/**
 * udev_device_new_from_devnum:
 * @udev: udev library context
 * @type: char or block device
 * @devnum: device major/minor number
 *
 * Create new udev device, and fill in information from the sys
 * device and the udev database entry. The device is looked-up
 * by its major/minor number and type. Character and block device
 * numbers are not unique across the two types.
 *
 * The initial refcount is 1, and needs to be decremented to
 * release the resources of the udev device.
 *
 * Returns: a new udev device, or #NULL, if it does not exist
 **/
_public_ struct udev_device *udev_device_new_from_devnum(struct udev *udev, char type, dev_t devnum)
{
        struct udev_device *udev_device;
        int r;

        if (udev == NULL) {
                errno = EINVAL;
                return NULL;
        }

        udev_device = udev_device_new(udev);
        if (udev_device == NULL) {
                errno = ENOMEM;
                return NULL;
        }

        r = sd_device_new_from_devnum(&udev_device->device, type, devnum);
        if (r < 0) {
                errno = -r;
                udev_device_unref(udev_device);
                return NULL;
        }

        return udev_device;
}

/**
 * udev_device_new_from_device_id:
 * @udev: udev library context
 * @id: text string identifying a kernel device
 *
 * Create new udev device, and fill in information from the sys
 * device and the udev database entry. The device is looked-up
 * by a special string:
 *   b8:2          - block device major:minor
 *   c128:1        - char device major:minor
 *   n3            - network device ifindex
 *   +sound:card29 - kernel driver core subsystem:device name
 *
 * The initial refcount is 1, and needs to be decremented to
 * release the resources of the udev device.
 *
 * Returns: a new udev device, or #NULL, if it does not exist
 **/
_public_ struct udev_device *udev_device_new_from_device_id(struct udev *udev, const char *id)
{
        struct udev_device *udev_device;
        int r;

        if (udev == NULL) {
                errno = EINVAL;
                return NULL;
        }

        udev_device = udev_device_new(udev);
        if (udev_device == NULL) {
                errno = ENOMEM;
                return NULL;
        }

        r = sd_device_new_from_device_id(&udev_device->device, id);
        if (r < 0) {
                errno = -r;
                udev_device_unref(udev_device);
                return NULL;
        }

        return udev_device;
}

/**
 * udev_device_new_from_subsystem_sysname:
 * @udev: udev library context
 * @subsystem: the subsystem of the device
 * @sysname: the name of the device
 *
 * Create new udev device, and fill in information from the sys device
 * and the udev database entry. The device is looked up by the subsystem
 * and name string of the device, like "mem" / "zero", or "block" / "sda".
 *
 * The initial refcount is 1, and needs to be decremented to
 * release the resources of the udev device.
 *
 * Returns: a new udev device, or #NULL, if it does not exist
 **/
_public_ struct udev_device *udev_device_new_from_subsystem_sysname(struct udev *udev, const char *subsystem, const char *sysname)
{
        struct udev_device *udev_device;
        int r;

        if (udev == NULL) {
                errno = EINVAL;
                return NULL;
        }

        udev_device = udev_device_new(udev);
        if (udev_device == NULL) {
                errno = ENOMEM;
                return NULL;
        }

        r = sd_device_new_from_subsystem_sysname(&udev_device->device, subsystem, sysname);
        if (r < 0) {
                errno = -r;
                udev_device_unref(udev_device);
                return NULL;
        }

        return udev_device;
}

/**
 * udev_device_new_from_environment
 * @udev: udev library context
 *
 * Create new udev device, and fill in information from the
 * current process environment. This only works reliable if
 * the process is called from a udev rule. It is usually used
 * for tools executed from IMPORT= rules.
 *
 * The initial refcount is 1, and needs to be decremented to
 * release the resources of the udev device.
 *
 * Returns: a new udev device, or #NULL, if it does not exist
 **/
_public_ struct udev_device *udev_device_new_from_environment(struct udev *udev)
{

// TODO!
        int i;
        struct udev_device *udev_device;

        udev_device = udev_device_new(udev);
        if (udev_device == NULL)
                return NULL;
/* TODO
        r = sd_device_new_from_strv();
        if (r < 0) {
                errno = -r;
                udev_device_unref(udev_device);
                return NULL;
        }
*/

        udev_device_set_info_loaded(udev_device);

        for (i = 0; environ[i] != NULL; i++)
                udev_device_add_property_from_string_parse(udev_device, environ[i]);

        if (udev_device_add_property_from_string_parse_finish(udev_device) < 0) {
                log_debug("missing values, invalid device");
                udev_device_unref(udev_device);
                udev_device = NULL;
        }

        return udev_device;
}

static struct udev_device *device_new_from_parent(struct udev_device *child)
{
        struct udev_device *parent;
        int r;

        if (udev == NULL) {
                errno = EINVAL;
                return NULL;
        }

        parent = udev_device_new(udev);
        if (parent == NULL) {
                errno = ENOMEM;
                return NULL;
        }

        r = sd_device_get_parent(child, &parent->device);
        if (r < 0) {
                errno = -r;
                udev_device_unref(udev_device);
                return NULL;
        }

        return parent;
}

/**
 * udev_device_get_parent:
 * @udev_device: the device to start searching from
 *
 * Find the next parent device, and fill in information from the sys
 * device and the udev database entry.
 *
 * Returned device is not referenced. It is attached to the child
 * device, and will be cleaned up when the child device is cleaned up.
 *
 * It is not necessarily just the upper level directory, empty or not
 * recognized sys directories are ignored.
 *
 * It can be called as many times as needed, without caring about
 * references.
 *
 * Returns: a new udev device, or #NULL, if it no parent exist.
 **/
_public_ struct udev_device *udev_device_get_parent(struct udev_device *udev_device)
{
        if (udev_device == NULL) {
                errno = EINVAL;
                return NULL;
        }
        if (!udev_device->parent_set) {
                udev_device->parent_set = true;
                udev_device->parent_device = device_new_from_parent(udev_device);
        }
        // errno will differ here in case parent_device == NULL
        return udev_device->parent_device;
}

/**
 * udev_device_get_parent_with_subsystem_devtype:
 * @udev_device: udev device to start searching from
 * @subsystem: the subsystem of the device
 * @devtype: the type (DEVTYPE) of the device
 *
 * Find the next parent device, with a matching subsystem and devtype
 * value, and fill in information from the sys device and the udev
 * database entry.
 *
 * If devtype is #NULL, only subsystem is checked, and any devtype will
 * match.
 *
 * Returned device is not referenced. It is attached to the child
 * device, and will be cleaned up when the child device is cleaned up.
 *
 * It can be called as many times as needed, without caring about
 * references.
 *
 * Returns: a new udev device, or #NULL if no matching parent exists.
 **/
_public_ struct udev_device *udev_device_get_parent_with_subsystem_devtype(struct udev_device *udev_device, const char *subsystem, const char *devtype)
{
        sd_device *parent;

        if (udev_device == NULL) {
                errno = EINVAL;
                return NULL;
        }

        /* this relies on the fact that finding the subdevice of a parent or the
           parent of a subdevice commute */

        /* first find the correct sd_device */
        r = sd_device_get_parent_with_subsystem_devtype(udev_device->device, subsystem, devtype, &parent);
        if (r < 0) {
                errno = -r;
                return NULL;
        }

        /* then walk the chain of udev_device parents until the correspanding
           one is found */
        while ((udev_device = udev_device_get_parent(udev_device))) {
                if (udev_device->device == parent)
                        return udev_parent;
        }

        errno = ENOENT;
        return NULL;
}

/**
 * udev_device_get_udev:
 * @udev_device: udev device
 *
 * Retrieve the udev library context the device was created with.
 *
 * Returns: the udev library context
 **/
_public_ struct udev *udev_device_get_udev(struct udev_device *udev_device)
{
        if (udev_device == NULL) {
                errno = EINVAL;
                return NULL;
        }

        return udev_device->udev;
}

/**
 * udev_device_ref:
 * @udev_device: udev device
 *
 * Take a reference of a udev device.
 *
 * Returns: the passed udev device
 **/
_public_ struct udev_device *udev_device_ref(struct udev_device *udev_device)
{
        if (udev_device == NULL)
                return NULL;
        udev_device->refcount++;
        return udev_device;
}

/**
 * udev_device_unref:
 * @udev_device: udev device
 *
 * Drop a reference of a udev device. If the refcount reaches zero,
 * the resources of the device will be released.
 *
 * Returns: #NULL
 **/
_public_ struct udev_device *udev_device_unref(struct udev_device *udev_device)
{
        if (udev_device == NULL)
                return NULL;
        udev_device->refcount--;
        if (udev_device->refcount > 0)
                return NULL;
        sd_device_unref(udev_device->device);
        if (udev_device->parent_device != NULL)
                udev_device_unref(udev_device->parent_device);
        udev_list_cleanup(&udev_device->event_properties);
        free(udev_device->action);
        free(udev_device->devpath_old);
        free(udev_device->envp);
        free(udev_device);
        return NULL;
}

/**
 * udev_device_get_devpath:
 * @udev_device: udev device
 *
 * Retrieve the kernel devpath value of the udev device. The path
 * does not contain the sys mount point, and starts with a '/'.
 *
 * Returns: the devpath of the udev device
 **/
_public_ const char *udev_device_get_devpath(struct udev_device *udev_device)
{
        const char *devpath;

        if (udev_device == NULL) {
                errno = EINVAL;
                return NULL;
        }

        r = sd_device_get_devpath(udev_device->device, &devpath);
        if (r < 0) {
                errno = -r;
                return NULL;
        }

        return devpath;
}

/**
 * udev_device_get_syspath:
 * @udev_device: udev device
 *
 * Retrieve the sys path of the udev device. The path is an
 * absolute path and starts with the sys mount point.
 *
 * Returns: the sys path of the udev device
 **/
_public_ const char *udev_device_get_syspath(struct udev_device *udev_device)
{
        const char *syspath;

        if (udev_device == NULL) {
                errno = EINVAL;
                return NULL;
        }

        r = sd_device_get_syspath(udev_device->device, &syspath);
        if (r < 0) {
                errno = -r;
                return NULL;
        }

        return syspath;
}

/**
 * udev_device_get_sysname:
 * @udev_device: udev device
 *
 * Get the kernel device name in /sys.
 *
 * Returns: the name string of the device device
 **/
_public_ const char *udev_device_get_sysname(struct udev_device *udev_device)
{
        const char *sysname;

        if (udev_device == NULL) {
                errno = EINVAL;
                return NULL;
        }

        r = sd_device_get_sysname(udev_device->device, &sysname);
        if (r < 0) {
                errno = -r;
                return NULL;
        }

        return sysname;
}

/**
 * udev_device_get_sysnum:
 * @udev_device: udev device
 *
 * Get the instance number of the device.
 *
 * Returns: the trailing number string of the device name
 **/
_public_ const char *udev_device_get_sysnum(struct udev_device *udev_device)
{
        const char *sysnum;

        if (udev_device == NULL) {
                errno = EINVAL;
                return NULL;
        }

        r = sd_device_get_sysnum(udev_device->device, &sysnum);
        if (r < 0) {
                errno = -r;
                return NULL;
        }

        return sysnum;
}

/**
 * udev_device_get_devnode:
 * @udev_device: udev device
 *
 * Retrieve the device node file name belonging to the udev device.
 * The path is an absolute path, and starts with the device directory.
 *
 * Returns: the device node file name of the udev device, or #NULL if no device node exists
 **/
_public_ const char *udev_device_get_devnode(struct udev_device *udev_device)
{
        const char *devnode;

        if (udev_device == NULL) {
                errno = EINVAL;
                return NULL;
        }

        r = sd_device_get_devnode(udev_device->device, &devnode);
        if (r < 0) {
                errno = -r;
                return NULL;
        }

        return devnode;
}

/**
 * udev_device_get_devlinks_list_entry:
 * @udev_device: udev device
 *
 * Retrieve the list of device links pointing to the device file of
 * the udev device. The next list entry can be retrieved with
 * udev_list_entry_get_next(), which returns #NULL if no more entries exist.
 * The devlink path can be retrieved from the list entry by
 * udev_list_entry_get_name(). The path is an absolute path, and starts with
 * the device directory.
 *
 * Returns: the first entry of the device node link list
 **/
_public_ struct udev_list_entry *udev_device_get_devlinks_list_entry(struct udev_device *udev_device)
{
        if (udev_device == NULL) {
                errno = EINVAL;
                return NULL;
        }

        //TODO

        return udev_list_get_entry(&udev_device->devlinks_list);
}

void udev_device_cleanup_devlinks_list(struct udev_device *udev_device)
{
        udev_device->devlinks_uptodate = false;
        udev_list_cleanup(&udev_device->devlinks_list);
}

/**
 * udev_device_get_event_properties_entry:
 * @udev_device: udev device
 *
 * Retrieve the list of key/value device properties of the udev
 * device. The next list entry can be retrieved with udev_list_entry_get_next(),
 * which returns #NULL if no more entries exist. The property name
 * can be retrieved from the list entry by udev_list_entry_get_name(),
 * the property value by udev_list_entry_get_value().
 *
 * Returns: the first entry of the property list
 **/
_public_ struct udev_list_entry *udev_device_get_properties_list_entry(struct udev_device *udev_device)
{
        if (udev_device == NULL) {
                errno = EINVAL;
                return NULL;
        }

        //TODO

        return udev_list_get_entry(&udev_device->event_properties);
}

/**
 * udev_device_get_action:
 * @udev_device: udev device
 *
 * This is only valid if the device was received through a monitor. Devices read from
 * sys do not have an action string. Usual actions are: add, remove, change, online,
 * offline.
 *
 * Returns: the kernel action value, or #NULL if there is no action value available.
 **/
_public_ const char *udev_device_get_action(struct udev_device *udev_device)
{
        if (udev_device == NULL) {
                errno = EINVAL;
                return NULL;
        }

        return udev_device->action;
}

/**
 * udev_device_get_usec_since_initialized:
 * @udev_device: udev device
 *
 * Return the number of microseconds passed since udev set up the
 * device for the first time.
 *
 * This is only implemented for devices with need to store properties
 * in the udev database. All other devices return 0 here.
 *
 * Returns: the number of microseconds since the device was first seen.
 **/
_public_ unsigned long long int udev_device_get_usec_since_initialized(struct udev_device *udev_device)
{
        usec_t now_ts;

        //TODO: error handling

        if (udev_device == NULL)
                return 0;
        if (!udev_device->info_loaded)
                udev_device_read_db(udev_device, NULL);
        if (udev_device->usec_initialized == 0)
                return 0;
        now_ts = now(CLOCK_MONOTONIC);
        if (now_ts == 0)
                return 0;
        return now_ts - udev_device->usec_initialized;
}

usec_t udev_device_get_usec_initialized(struct udev_device *udev_device)
{
        return udev_device->usec_initialized;
}

void udev_device_set_usec_initialized(struct udev_device *udev_device, usec_t usec_initialized)
{
        char num[32];

        udev_device->usec_initialized = usec_initialized;
        snprintf(num, sizeof(num), USEC_FMT, usec_initialized);
        udev_device_add_property(udev_device, "USEC_INITIALIZED", num);
}

/**
 * udev_device_get_sysattr_value:
 * @udev_device: udev device
 * @sysattr: attribute name
 *
 * The retrieved value is cached in the device. Repeated calls will return the same
 * value and not open the attribute again.
 *
 * Returns: the content of a sys attribute file, or #NULL if there is no sys attribute value.
 **/
_public_ const char *udev_device_get_sysattr_value(struct udev_device *udev_device, const char *sysattr)
{
        const char *value;
        int r;

        if (udev_device == NULL || sysattr == NULL) {
                errno = EINVAL;
                return NULL;
        }

        r = sd_device_get_sysattr_value(udev_device->device, sysattr, &value);
        if (r < 0) {
                errno = -r;
                return NULL;
        }

        return value;
}

/**
 * udev_device_set_sysattr_value:
 * @udev_device: udev device
 * @sysattr: attribute name
 * @value: new value to be set
 *
 * Update the contents of the sys attribute and the cached value of the device.
 *
 * Returns: Negative error code on failure or 0 on success.
 **/
_public_ int udev_device_set_sysattr_value(struct udev_device *udev_device, const char *sysattr, char *value)
{
        if (udev_device == NULL)
                return -EINVAL;

        if (!value)
                value = "";

        r = sd_device_set_sysattr_value(udev_device->device, sysattr, value);
        if (r < 0)
                return r;

        return 0;
}

/**
 * udev_device_get_sysattr_list_entry:
 * @udev_device: udev device
 *
 * Retrieve the list of available sysattrs, with value being empty;
 * This just return all available sysfs attributes for a particular
 * device without reading their values.
 *
 * Returns: the first entry of the property list
 **/
_public_ struct udev_list_entry *udev_device_get_sysattr_list_entry(struct udev_device *udev_device)
{
        //TODO
        if (!udev_device->sysattr_list_read) {
                int ret;
                ret = udev_device_sysattr_list_read(udev_device);
                if (0 > ret)
                        return NULL;
        }

        return udev_list_get_entry(&udev_device->sysattr_list);
}

static int udev_device_set_devnode(struct udev_device *udev_device, const char *devnode)
{
        free(udev_device->devnode);
        if (devnode[0] != '/') {
                if (asprintf(&udev_device->devnode, "/dev/%s", devnode) < 0)
                        udev_device->devnode = NULL;
        } else {
                udev_device->devnode = strdup(devnode);
        }
        if (udev_device->devnode == NULL)
                return -ENOMEM;
        udev_device_add_property(udev_device, "DEVNAME", udev_device->devnode);
        return 0;
}

int udev_device_add_devlink(struct udev_device *udev_device, const char *devlink)
{
        struct udev_list_entry *list_entry;

        udev_device->devlinks_uptodate = false;
        list_entry = udev_list_entry_add(&udev_device->devlinks_list, devlink, NULL);
        if (list_entry == NULL)
                return -ENOMEM;
        return 0;
}

const char *udev_device_get_id_filename(struct udev_device *udev_device)
{
        if (udev_device->id_filename == NULL) {
                if (udev_device_get_subsystem(udev_device) == NULL)
                        return NULL;

                if (major(udev_device_get_devnum(udev_device)) > 0) {
                        /* use dev_t -- b259:131072, c254:0 */
                        if (asprintf(&udev_device->id_filename, "%c%u:%u",
                                     streq(udev_device_get_subsystem(udev_device), "block") ? 'b' : 'c',
                                     major(udev_device_get_devnum(udev_device)),
                                     minor(udev_device_get_devnum(udev_device))) < 0)
                                udev_device->id_filename = NULL;
                } else if (udev_device_get_ifindex(udev_device) > 0) {
                        /* use netdev ifindex -- n3 */
                        if (asprintf(&udev_device->id_filename, "n%i", udev_device_get_ifindex(udev_device)) < 0)
                                udev_device->id_filename = NULL;
                } else {
                        /*
                         * use $subsys:$syname -- pci:0000:00:1f.2
                         * sysname() has '!' translated, get it from devpath
                         */
                        const char *sysname;
                        sysname = strrchr(udev_device->devpath, '/');
                        if (sysname == NULL)
                                return NULL;
                        sysname = &sysname[1];
                        if (asprintf(&udev_device->id_filename, "+%s:%s", udev_device_get_subsystem(udev_device), sysname) < 0)
                                udev_device->id_filename = NULL;
                }
        }
        return udev_device->id_filename;
}

/**
 * udev_device_get_is_initialized:
 * @udev_device: udev device
 *
 * Check if udev has already handled the device and has set up
 * device node permissions and context, or has renamed a network
 * device.
 *
 * This is only implemented for devices with a device node
 * or network interfaces. All other devices return 1 here.
 *
 * Returns: 1 if the device is set up. 0 otherwise.
 **/
_public_ int udev_device_get_is_initialized(struct udev_device *udev_device)
{
        if (!udev_device->info_loaded)
                udev_device_read_db(udev_device, NULL);
        return udev_device->is_initialized;
}

void udev_device_set_is_initialized(struct udev_device *udev_device)
{
        udev_device->is_initialized = true;
}

static bool is_valid_tag(const char *tag)
{
        return !strchr(tag, ':') && !strchr(tag, ' ');
}

int udev_device_add_tag(struct udev_device *udev_device, const char *tag)
{
        if (!is_valid_tag(tag))
                return -EINVAL;
        udev_device->tags_uptodate = false;
        if (udev_list_entry_add(&udev_device->tags_list, tag, NULL) != NULL)
                return 0;
        return -ENOMEM;
}

void udev_device_remove_tag(struct udev_device *udev_device, const char *tag)
{
        struct udev_list_entry *e;

        if (!is_valid_tag(tag))
                return;
        e = udev_list_get_entry(&udev_device->tags_list);
        e = udev_list_entry_get_by_name(e, tag);
        if (e) {
                udev_device->tags_uptodate = false;
                udev_list_entry_delete(e);
        }
}

void udev_device_cleanup_tags_list(struct udev_device *udev_device)
{
        udev_device->tags_uptodate = false;
        udev_list_cleanup(&udev_device->tags_list);
}

/**
 * udev_device_get_tags_list_entry:
 * @udev_device: udev device
 *
 * Retrieve the list of tags attached to the udev device. The next
 * list entry can be retrieved with udev_list_entry_get_next(),
 * which returns #NULL if no more entries exist. The tag string
 * can be retrieved from the list entry by udev_list_entry_get_name().
 *
 * Returns: the first entry of the tag list
 **/
_public_ struct udev_list_entry *udev_device_get_tags_list_entry(struct udev_device *udev_device)
{
        if (udev_device == NULL)
                return NULL;
        if (!udev_device->info_loaded)
                udev_device_read_db(udev_device, NULL);
        return udev_list_get_entry(&udev_device->tags_list);
}

/**
 * udev_device_has_tag:
 * @udev_device: udev device
 * @tag: tag name
 *
 * Check if a given device has a certain tag associated.
 *
 * Returns: 1 if the tag is found. 0 otherwise.
 **/
_public_ int udev_device_has_tag(struct udev_device *udev_device, const char *tag)
{
        struct udev_list_entry *list_entry;

        if (udev_device == NULL)
                return false;
        if (!udev_device->info_loaded)
                udev_device_read_db(udev_device, NULL);
        list_entry = udev_device_get_tags_list_entry(udev_device);
        if (udev_list_entry_get_by_name(list_entry, tag) != NULL)
                return true;
        return false;
}

#define ENVP_SIZE                        128
#define MONITOR_BUF_SIZE                4096
static int update_envp_monitor_buf(struct udev_device *udev_device)
{
        struct udev_list_entry *list_entry;
        char *s;
        size_t l;
        unsigned int i;

        /* monitor buffer of property strings */
        free(udev_device->monitor_buf);
        udev_device->monitor_buf_len = 0;
        udev_device->monitor_buf = malloc(MONITOR_BUF_SIZE);
        if (udev_device->monitor_buf == NULL)
                return -ENOMEM;

        /* envp array, strings will point into monitor buffer */
        if (udev_device->envp == NULL)
                udev_device->envp = malloc(sizeof(char *) * ENVP_SIZE);
        if (udev_device->envp == NULL)
                return -ENOMEM;

        i = 0;
        s = udev_device->monitor_buf;
        l = MONITOR_BUF_SIZE;
        udev_list_entry_foreach(list_entry, udev_device_get_event_properties_entry(udev_device)) {
                const char *key;

                key = udev_list_entry_get_name(list_entry);
                /* skip private variables */
                if (key[0] == '.')
                        continue;

                /* add string to envp array */
                udev_device->envp[i++] = s;
                if (i+1 >= ENVP_SIZE)
                        return -EINVAL;

                /* add property string to monitor buffer */
                l = strpcpyl(&s, l, key, "=", udev_list_entry_get_value(list_entry), NULL);
                if (l == 0)
                        return -EINVAL;
                /* advance past the trailing '\0' that strpcpyl() guarantees */
                s++;
                l--;
        }
        udev_device->envp[i] = NULL;
        udev_device->monitor_buf_len = s - udev_device->monitor_buf;
        udev_device->envp_uptodate = true;
        return 0;
}

char **udev_device_get_properties_envp(struct udev_device *udev_device)
{
        if (!udev_device->envp_uptodate)
                if (update_envp_monitor_buf(udev_device) != 0)
                        return NULL;
        return udev_device->envp;
}

ssize_t udev_device_get_properties_monitor_buf(struct udev_device *udev_device, const char **buf)
{
        if (!udev_device->envp_uptodate)
                if (update_envp_monitor_buf(udev_device) != 0)
                        return -EINVAL;
        *buf = udev_device->monitor_buf;
        return udev_device->monitor_buf_len;
}

int udev_device_set_action(struct udev_device *udev_device, const char *action)
{
        free(udev_device->action);
        udev_device->action = strdup(action);
        if (udev_device->action == NULL)
                return -ENOMEM;
        udev_device_add_property(udev_device, "ACTION", udev_device->action);
        return 0;
}

int udev_device_get_devlink_priority(struct udev_device *udev_device)
{
        if (!udev_device->info_loaded)
                udev_device_read_db(udev_device, NULL);
        return udev_device->devlink_priority;
}

int udev_device_set_devlink_priority(struct udev_device *udev_device, int prio)
{
         udev_device->devlink_priority = prio;
        return 0;
}

int udev_device_get_watch_handle(struct udev_device *udev_device)
{
        if (!udev_device->info_loaded)
                udev_device_read_db(udev_device, NULL);
        return udev_device->watch_handle;
}

int udev_device_set_watch_handle(struct udev_device *udev_device, int handle)
{
        udev_device->watch_handle = handle;
        return 0;
}

bool udev_device_get_db_persist(struct udev_device *udev_device)
{
        return udev_device->db_persist;
}

void udev_device_set_db_persist(struct udev_device *udev_device)
{
        udev_device->db_persist = true;
}
