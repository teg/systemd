/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

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
#include "sd-daemon.h"
#include "mkdir.h"
#include "label.h"

#include "deviced-manager.h"

int main(int argc, char *argv[]) {
        _cleanup_(manager_freep) Manager *m = NULL;
        int r, netlink_fd;

        log_set_target(LOG_TARGET_AUTO);
        log_parse_environment();
        log_open();

        if (argc != 1) {
                log_error("This program takes no arguments.");
                r = -EINVAL;
                goto finish;
        }

        r = sd_listen_fds(true);
        if (r < 0) {
                r = log_error_errno(r, "Failed to get netlink fd: %m");
                goto finish;
        } else if (r != 1) {
                log_error("Expected one socket, got %d", r);
                r = -EINVAL;
                goto finish;
        }

        if (!sd_is_socket(SD_LISTEN_FDS_START, AF_NETLINK, SOCK_RAW, -1)) {
                log_error("Socket is not netlink");
                r = -EINVAL;
                goto finish;
        } else
                netlink_fd = SD_LISTEN_FDS_START;

        umask(0022);

        r = mac_selinux_init("/dev");
        if (r < 0) {
                log_error_errno(r, "SELinux setup failed: %m");
                goto finish;
        }

        r = mkdir_label("/run/systemd/device", 0755);
        if (r < 0 && r != -EEXIST) {
                log_error_errno(r, "Could not create runtime directory: %m");
                goto finish;
        }

        assert_se(sigprocmask_many(SIG_BLOCK, SIGTERM, SIGINT, -1) == 0);

        r = manager_new(&m, netlink_fd);
        if (r < 0) {
                log_error_errno(r, "Could not create manager: %m");
                goto finish;
        }

        r = manager_start(m);
        if (r < 0) {
                log_error_errno(r, "Colud not start manager: %m");
                goto finish;
        }

        sd_notify(false,
                  "READY=1\n"
                  "STATUS=Processing requests...");

        r = sd_event_loop(m->event);
        if (r < 0) {
                log_error_errno(r, "Event loop failed: %m");
                goto finish;
        }

        sd_event_get_exit_code(m->event, &r);

finish:
        sd_notify(false,
                  "STOPPING=1\n"
                  "STATUS=Shutting down...");

        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
