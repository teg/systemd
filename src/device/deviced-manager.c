/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyrigh 2014 Tom Gundersen <teg@jklm.no>

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

#include "device-util.h"
#include "deviced-manager.h"

static void device_handler(DeviceMonitor *monitor, DeviceMonitorEvent *event, void *userdata) {
        Manager *manager = userdata;
        int r;

        assert(event);
        assert(manager);

        r = manager_send_device(manager, event);
        if (r < 0)
                log_warning_errno(r, "could not send device event: %m");
}

int manager_new(Manager **ret, int fd) {
        _cleanup_(manager_freep) Manager *m = NULL;
        int r;

        assert(ret);
        assert(fd >= 0);

        m = new0(Manager, 1);
        if (!m)
                return -ENOMEM;

        r = sd_event_default(&m->event);
        if (r < 0)
                return r;

        sd_event_add_signal(m->event, NULL, SIGTERM, NULL,  NULL);
        sd_event_add_signal(m->event, NULL, SIGINT, NULL, NULL);

        sd_event_set_watchdog(m->event, true);

        r = device_monitor_new_from_netlink(&m->monitor, fd, m->event, 0);
        if (r < 0)
                return r;

        r = device_monitor_set_callback(m->monitor, device_handler, m);
        if (r < 0)
                return r;

        *ret = m;
        m = NULL;

        return 0;
}

static int manager_connect_bus(Manager *m) {
        int r;

        assert(m);

        r = sd_bus_default_system(&m->bus);
        if (r < 0)
                return log_error_errno(r, "failed to connect to bus: %m");

        r = sd_bus_add_object_vtable(m->bus, NULL,
                                     "/org/freedesktop/device1",
                                     "org.freedesktop.device1.Manager",
                                     manager_vtable, NULL);
        if (r < 0)
                return log_error_errno(r, "failed to set vtable: %m");

        r = sd_bus_request_name(m->bus, "org.freedesktop.device1", 0);
        if (r < 0)
                return log_error_errno(r, "failed to request name: %m");

        r = sd_bus_attach_event(m->bus, m->event, SD_EVENT_PRIORITY_NORMAL);
        if (r < 0)
                return log_error_errno(r, "failed to attach event to bus: %m");

        return 0;
}

int manager_start(Manager *m) {
        int r;

        assert(m);

        r = manager_connect_bus(m);
        if (r < 0)
                return r;

        r = device_monitor_start(m->monitor);
        if (r < 0)
                return r;

        return 0;
}

Manager *manager_free(Manager *m) {
        if (!m)
                return NULL;

        sd_event_unref(m->event);
        device_monitor_unref(m->monitor);

        free(m);

        return NULL;
}
