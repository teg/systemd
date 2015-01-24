/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

#ifndef foosddevicehfoo
#define foosddevicehfoo

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

#include <sys/types.h>
#include <stdint.h>

#include "_sd-common.h"

_SD_BEGIN_DECLARATIONS;

typedef struct sd_device sd_device;

sd_device *sd_device_ref(sd_device *device);
sd_device *sd_device_unref(sd_device *device);

int sd_device_new_from_syspath(sd_device **ret, const char *syspath);
int sd_device_new_from_devnum(sd_device **ret, char type, dev_t devnum);
int sd_device_new_from_subsystem_sysname(sd_device **ret, const char *subsystem, const char *sysname);
int sd_device_new_from_device_id(sd_device **ret, const char *id);

int sd_device_get_syspath(sd_device *device, const char **ret);
int sd_device_get_parent(sd_device *child, sd_device **ret);
int sd_device_get_subsystem(sd_device *device, const char **ret);
int sd_device_get_devtype(sd_device *device, const char **ret);
int sd_device_get_parent_with_subsystem_devtype(sd_device *child, const char *subsystem, const char *devtype, sd_device **ret);
int sd_device_get_devnum(sd_device *device, dev_t *devnum);
int sd_device_get_driver(sd_device *device, const char **ret);

_SD_END_DECLARATIONS;

#endif
