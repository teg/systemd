/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

#pragma once

/***
  This file is part of systemd.

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

#include "util.h"
#include "sd-device.h"
#include "sd-event.h"

typedef struct DeviceMonitor DeviceMonitor;

typedef enum DeviceAction {
        DEVICE_ACTION_ADD,
        DEVICE_ACTION_REMOVE,
        DEVICE_ACTION_CHANGE,
        DEVICE_ACTION_MOVE,
        DEVICE_ACTION_ONLINE,
        DEVICE_ACTION_OFFLINE,
        _DEVICE_ACTION_MAX,
        _DEVICE_ACTION_INVALID = -1,
} DeviceAction;

typedef void (*device_monitor_cb_t)(DeviceMonitor *monitor, sd_device *device, uint64_t seqnum, DeviceAction action, const char *devpath_old, void *userdata);

DeviceMonitor *device_monitor_ref(DeviceMonitor *monitor);
DeviceMonitor *device_monitor_unref(DeviceMonitor *monitor);

int device_monitor_new_from_netlink(DeviceMonitor **ret, int fd, sd_event *event, int priority);
int device_monitor_set_receive_buffer_size(DeviceMonitor *monitor, int size);
int device_monitor_set_callback(DeviceMonitor *monitor, device_monitor_cb_t cb, void *userdata);

int device_monitor_start(DeviceMonitor *monitor);
int device_monitor_stop(DeviceMonitor *monitor);

DEFINE_TRIVIAL_CLEANUP_FUNC(DeviceMonitor*, device_monitor_unref);
#define _cleanup_device_monitor_unref_ _cleanup_(device_monitor_unrefp)
