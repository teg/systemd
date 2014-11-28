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

#include "bus-util.h"

#include "device-util.h"

#include "deviced-manager.h"

const sd_bus_vtable manager_vtable[] = {
        SD_BUS_VTABLE_START(0),

        SD_BUS_SIGNAL("AddDevice", "ta{ss}", 0),
        SD_BUS_SIGNAL("ChangeDevice", "ta{ss}", 0),
        SD_BUS_SIGNAL("RemoveDevice", "ta{ss}", 0),
        SD_BUS_SIGNAL("OnlineDevice", "ta{ss}", 0),
        SD_BUS_SIGNAL("OfflineDevice", "ta{ss}", 0),
        SD_BUS_SIGNAL("MoveDevice", "tsa{ss}", 0),

        SD_BUS_VTABLE_END
};

int manager_send_device(Manager *manager, DeviceMonitorEvent *event) {
        _cleanup_bus_message_unref_ sd_bus_message *m = NULL;
        const char *method, *key, *value;
        int r;

        assert(event);
        assert(manager);

        switch (event->action) {
        case DEVICE_ACTION_ADD:
                method = "AddDevice";
                break;
        case DEVICE_ACTION_CHANGE:
                method = "ChangeDevice";
                break;
        case DEVICE_ACTION_REMOVE:
                method = "RemoveDevice";
                break;
        case DEVICE_ACTION_MOVE:
                method = "MoveDevice";
                break;
        case DEVICE_ACTION_ONLINE:
                method = "OnlineDevice";
                break;
        case DEVICE_ACTION_OFFLINE:
                method = "OfflineDevice";
                break;
        default:
                log_warning("Received unsupported device monitor event: %d", event->action);
                return -EIO;
        }

        r = sd_bus_message_new_signal(manager->bus, &m,
                                      "/org/freedesktop/device1",
                                      "org.freedesktop.device1.Device",
                                      method);
        if (r < 0)
                return log_error_errno(r, "failed to create signal: %m");

        r = sd_bus_message_append(m, "t", event->seqnum);
        if (r < 0)
                return log_error_errno(r, "failed to append seqnum: %m");

        if (event->action == DEVICE_ACTION_MOVE) {
                r = sd_bus_message_append(m, "s", event->devpath_old);
                if (r < 0)
                        return log_error_errno(r, "failed to append old devpath: %m");
        }

        r = sd_bus_message_open_container(m, 'a', "{ss}");
        if (r < 0)
                return log_error_errno(r, "failed to open array entry: %m");

        FOREACH_DEVICE_PROPERTY(event->device, key, value) {
                r = sd_bus_message_open_container(m, 'e', "ss");
                if (r < 0)
                        return log_error_errno(r, "failed to open dict entry: %m");

                r = sd_bus_message_append(m, "ss", key, value);
                if (r < 0)
                        return log_error_errno(r, "failed to append device property '%s=%s: %m", key, value);

                r = sd_bus_message_close_container(m);
                if (r < 0)
                        return log_error_errno(r, "failed to close dict entry: %m");
        }

        r = sd_bus_message_close_container(m);
        if (r < 0)
                return log_error_errno(r, "failed to close array entry: %m");

        r = sd_bus_send(manager->bus, m, NULL);
        if (r < 0)
                return log_error_errno(r, "failed to send signal: %m");

        return 0;
}
