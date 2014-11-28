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

#include "sd-event.h"
#include "sd-bus.h"
#include "device-monitor.h"

typedef struct Manager Manager;

struct Manager {
        sd_event *event;
        sd_bus *bus;
        DeviceMonitor *monitor;
};

int manager_new(Manager **ret, int fd);
Manager* manager_free(Manager *m);

int manager_start(Manager *m);

extern const sd_bus_vtable manager_vtable[];

int manager_send_device(Manager *manager, DeviceMonitorEvent *event);

DEFINE_TRIVIAL_CLEANUP_FUNC(Manager*, manager_free);
