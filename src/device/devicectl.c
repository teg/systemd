/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2014 Tom Gundersen
  Copyright 2014 Lennart Poettering

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

#include <stdbool.h>
#include <getopt.h>

#include "sd-device.h"
#include "sd-event.h"
#include "sd-bus.h"

#include "pager.h"
#include "build.h"
#include "util.h"
#include "strv.h"
#include "hashmap.h"
#include "path-util.h"
#include "event-util.h"
#include "bus-util.h"
#include "device-util.h"
#include "device-monitor.h"
#include "device-internal.h"

typedef struct MonitorContext {
        long long unsigned count;
        usec_t starttime;
        Hashmap *pending_events;
} MonitorContext;

typedef struct KernelEvent {
        uint64_t seqnum;
        usec_t timestamp;
} KernelEvent;

static bool arg_pager = true;
static bool arg_legend = true;
static bool arg_all = false;

static void pager_open_if_enabled(void) {
        if (!arg_pager)
                return;

        pager_open(false);
}

static void action_to_color(DeviceAction action, const char **on, const char **off) {
        assert(on);
        assert(off);

        switch (action) {
        case DEVICE_ACTION_ADD:
        case DEVICE_ACTION_ONLINE:
                *on = ansi_highlight_green();
                *off = ansi_highlight_off();

                break;
        case DEVICE_ACTION_REMOVE:
        case DEVICE_ACTION_OFFLINE:
                *on = ansi_highlight_red();
                *off = ansi_highlight_off();

                break;
        case DEVICE_ACTION_CHANGE:
                *on = ansi_highlight_blue();

                *off = ansi_highlight_off();

                break;
        default:
                *on = *off = "";
        }
}

static void print_event(MonitorContext *ctx, DeviceAction action, usec_t time, uint64_t seqnum, bool userspace, const char *devpath, const char *subsys, const char *devpath_old) {
        KernelEvent *ke;
        const char *on, *off, *highlight_on, *highlight_off;
        int r;

        action_to_color(action, &on, &off);

        if (!time)
                time = now(CLOCK_REALTIME);

        if (userspace) {
                highlight_on = ansi_highlight();
                highlight_off = ansi_highlight_off();

                ke = hashmap_get(ctx->pending_events, &seqnum);

                time -= ke->timestamp;
        } else {
                ke = new0(KernelEvent, 1);
                if (!ke)
                        return;

                ke->seqnum = seqnum;
                ke->timestamp = time;

                r = hashmap_ensure_allocated(&ctx->pending_events, &uint64_hash_ops);
                if (r < 0)
                        return;

                r = hashmap_put(ctx->pending_events, &ke->seqnum, ke);
                if (r < 0)
                        return;

                time -= ctx->starttime;

                highlight_on = highlight_off = "";
        }

        if (action == DEVICE_ACTION_MOVE) {
                _cleanup_free_ char *devpath_parent = NULL, *devpath_rel = NULL;

                r = path_get_parent(devpath_old, &devpath_parent);
                if (r < 0)
                        return;

                r = path_make_relative(devpath_parent, devpath, &devpath_rel);
                if (r < 0)
                        return;

                printf("%s[%s%4"PRI_TIME".%06lu]%s   %7s: %s %s %s (%s)%s\n",
                       userspace ? "DEVICED" : "KERNEL ",
                       userspace ? "+" : " ",
                       time / USEC_PER_SEC, time % USEC_PER_SEC,
                       highlight_on,
                       device_action_to_string(action), devpath_old, draw_special_char(DRAW_ARROW), devpath_rel, subsys,
                       highlight_off);
        } else
                printf("%s[%s%4"PRI_TIME".%06lu] %s%s%s%s %7s: %s (%s)%s\n",
                       userspace ? "DEVICED" : "KERNEL ",
                       userspace ? "+" : " ",
                       time / USEC_PER_SEC, time % USEC_PER_SEC,
                       on, draw_special_char(DRAW_BLACK_CIRCLE), off,
                       highlight_on, device_action_to_string(action), devpath, subsys, highlight_off);
}

static void device_monitor_handler(DeviceMonitor *monitor, DeviceMonitorEvent *event, void *userdata) {
        MonitorContext *ctx = userdata;
        const char *devpath, *subsys;
        int r;

        assert(monitor);
        assert(event);
        assert(ctx);

        r = sd_device_get_devpath(event->device, &devpath);
        if (r < 0) {
                log_warning("could not get DEVPATH, ignoring event");
                return;
        }

        r = sd_device_get_subsystem(event->device, &subsys);
        if (r < 0) {
                log_warning("could not get SUBSYSTEM, ignoring event");
                return;
        }

        print_event(ctx, event->action, event->timestamp, event->seqnum, false, devpath, subsys, event->devpath_old);

        if (arg_all) {
                const char *key, *value;

                FOREACH_DEVICE_PROPERTY(event->device, key, value)
                        printf("%s=%s\n", key, value);

                printf("\n");
        }

        ctx->count ++;
}

static int device_bus_handler(sd_bus *bus, sd_bus_message *m, void *userdata, sd_bus_error *error) {
        MonitorContext *ctx = userdata;
        DeviceAction action;
        const char *devpath = NULL, *subsystem = NULL, *devpath_old = NULL;
        uint64_t seqnum;
        usec_t time;
        int r;

        assert(bus);
        assert(m);
        assert(ctx);

        r = sd_bus_message_read(m, "t", &seqnum);
        if (r < 0)
                return 0;

        if (sd_bus_message_is_signal(m, NULL, "AddDevice"))
                action = DEVICE_ACTION_ADD;
        else if (sd_bus_message_is_signal(m, NULL, "ChangeDevice"))
                action = DEVICE_ACTION_CHANGE;
        else if (sd_bus_message_is_signal(m, NULL, "RemoveDevice"))
                action = DEVICE_ACTION_REMOVE;
        else if (sd_bus_message_is_signal(m, NULL, "MoveDevice")) {
                action = DEVICE_ACTION_MOVE;

                r = sd_bus_message_read(m, "s", &devpath_old);
                if (r < 0)
                        return 0;
        } else if (sd_bus_message_is_signal(m, NULL, "OnlineDevice"))
                action = DEVICE_ACTION_ONLINE;
        else if (sd_bus_message_is_signal(m, NULL, "OfflineDevice"))
                action = DEVICE_ACTION_OFFLINE;
        else
                return 0;

        r = sd_bus_message_enter_container(m, 'a', "{ss}");
        if (r < 0)
                return 0;

        while ((r = sd_bus_message_enter_container(m, 'e', "ss")) > 0) {
                const char *key, *value;

                r = sd_bus_message_read(m, "ss", &key, &value);
                if (r < 0)
                        return 0;

                if (streq(key, "DEVPATH"))
                        devpath = value;
                else if (streq(key, "SUBSYSTEM"))
                        subsystem = value;

                r = sd_bus_message_exit_container(m);
                if (r < 0)
                        return 0;
        }
        if (r < 0)
                return 0;

        r = sd_bus_message_exit_container(m);
        if (r < 0)
                return 0;

        if (!devpath || !subsystem)
                return 0;

        r = sd_bus_message_get_realtime_usec(m, &time);
        if (r < 0)
                time = 0;

        print_event(ctx, action, time, seqnum, true, devpath, subsystem, devpath_old);

        return 0;
}

static int monitor_devices(char **args, unsigned n) {
        MonitorContext ctx = {};
        _cleanup_event_unref_ sd_event *event = NULL;
        _cleanup_device_monitor_unref_ DeviceMonitor *monitor = NULL;
        _cleanup_bus_unref_ sd_bus *bus = NULL;
        int r;

        r = sd_event_new(&event);
        if (r < 0)
                return r;

        r = sigprocmask_many(SIG_BLOCK, SIGTERM, SIGINT, -1);
        if (r < 0)
                return r;

        r = sd_event_add_signal(event, NULL, SIGTERM, NULL, NULL);
        if (r < 0)
                return r;

        r = sd_event_add_signal(event, NULL, SIGINT, NULL, NULL);
        if (r < 0)
                return r;

        r = device_monitor_new_from_netlink(&monitor, -1, event, 0);
        if (r < 0)
                return r;

        r = device_monitor_set_receive_buffer_size(monitor, 128*1024*1024);
        if (r == -EPERM)
                log_info("Lacking permissions to increase receivebuffer size, continuing with default size");
        else if (r < 0)
                return r;

        r = device_monitor_set_callback(monitor, device_monitor_handler, &ctx);
        if (r < 0)
                return r;

        r = sd_bus_default_system(&bus);
        if (r < 0)
                return r;

        r = sd_bus_negotiate_timestamp(bus, true);
        if (r < 0)
                return r;

        r = sd_bus_add_match(bus, NULL,
                             "type='signal',"
                             "sender='org.freedesktop.device1',"
                             "interface='org.freedesktop.device1.Manager',"
                             "path='/org/freedesktop/device1'",
                             device_bus_handler, &ctx);
        if (r < 0)
                return r;

        r = sd_bus_attach_event(bus, event, SD_EVENT_PRIORITY_NORMAL);
        if (r < 0)
                return r;

        r = device_monitor_start(monitor);
        if (r < 0)
                return r;

        if (arg_legend)
                printf("Kernel uevents:\n");

        ctx.starttime = now(CLOCK_REALTIME);

        r = sd_event_loop(event);
        if (r < 0)
                return r;

        printf("\n");

        if (arg_legend)
                printf("Received %llu uevents\n", ctx.count);

        hashmap_free_free(ctx.pending_events);

        return 0;
}

static int print_device(sd_device *device, bool all) {
        const char *devpath, *devnode, *devlink, *key, *value;
        int priority;
        int r;

        assert(device);

        r = sd_device_get_devpath(device, &devpath);
        if (r < 0)
                return r;

        printf("P: %s\n", devpath);

        if (all) {
                r = sd_device_get_devnode(device, &devnode);
                if (r >= 0) {
                        printf("N: %s\n", devnode + strlen("/dev/"));
                }

                r = device_get_devlink_priority(device, &priority);
                if (r < 0)
                        return r;

                if (priority != 0)
                        printf("L: %i\n", priority);

                FOREACH_DEVICE_DEVLINK(device, devlink)
                        printf("S: %s\n", devlink);

                FOREACH_DEVICE_PROPERTY(device, key, value)
                        printf("E: %s=%s\n", key, value);

                printf("\n");
        }

        return 0;
}

static int list_devices(char **args, unsigned n) {
        _cleanup_device_enumerator_unref_ sd_device_enumerator *enumerator = NULL;
        sd_device *device;
        int r;

        r = sd_device_enumerator_new(&enumerator);
        if (r < 0)
                return r;

        pager_open_if_enabled();

        FOREACH_DEVICE(enumerator, device) {
                r = print_device(device, arg_all);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int trigger(char **args, unsigned n) {
        _cleanup_device_enumerator_unref_ sd_device_enumerator *enumerator = NULL;
        sd_device *device;
        int r;

        r = sd_device_enumerator_new(&enumerator);
        if (r < 0)
                return r;

        FOREACH_DEVICE(enumerator, device) {
                _cleanup_close_ int fd = -1;
                const char *syspath;
                char *path;

                r = sd_device_get_syspath(device, &syspath);
                if (r < 0) {
                        log_warning("trigger: could not get syspath: %s", strerror(-r));
                        continue;
                }

                path = strappenda(syspath, "/uevent");

                fd = open(path, O_WRONLY|O_CLOEXEC);
                if (fd < 0) {
                        log_warning("trigger: could not open uevent file '%s': %m", path);
                        continue;
                }

                r = write(fd, "change", strlen("change"));
                if (r < 0) {
                        log_warning("trigger: could not write 'change' to '%s': %m", path);
                        continue;
                }
        }

        return 0;
}

static int show_device(char **args, unsigned n) {
        _cleanup_device_unref_ sd_device *device = NULL;
        int r;

        assert(args);
        assert(n == 2);

        r = sd_device_new_from_syspath(&device, args[1]);
        if (r < 0) {
                log_error("Could not get '%s': %s", args[1], strerror(-r));
                return r;
        }

        r = print_device(device, true);
        if (r < 0)
                return r;

        return 0;
}

static void help(void) {
        printf("%s [OPTIONS...]\n\n"
               "Query and control the networking subsystem.\n\n"
               "  -h --help             Show this help\n"
               "     --version          Show package version\n"
               "  -a --all              Show all information about devices\n"
               "     --no-pager         Disable the pager\n"
               "     --no-legend        Do not show the headers and footers\n"
               "Commands:\n"
               "  monitor               Monitor kernel events\n"
               "  show                  Show device properties\n"
               "  list                  List all devices\n"
               "  trigger               Trigger kernel events\n"
               , program_invocation_short_name);
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_VERSION = 0x100,
                ARG_NO_PAGER,
                ARG_NO_LEGEND,
        };

        static const struct option options[] = {
                { "help",      no_argument,       NULL, 'h'           },
                { "all",       no_argument,       NULL, 'a'           },
                { "version",   no_argument,       NULL, ARG_VERSION   },
                { "no-pager",  no_argument,       NULL, ARG_NO_PAGER  },
                { "no-legend", no_argument,       NULL, ARG_NO_LEGEND },
                {}
        };

        int c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "ha", options, NULL)) >= 0) {

                switch (c) {

                case 'h':
                        help();
                        return 0;

                case 'a':
                        arg_all = true;
                        break;

                case ARG_VERSION:
                        puts(PACKAGE_STRING);
                        puts(SYSTEMD_FEATURES);
                        return 0;

                case ARG_NO_PAGER:
                        arg_pager = false;
                        break;

                case ARG_NO_LEGEND:
                        arg_legend = false;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached("Unhandled option");
                }
        }

        return 1;
}

static int devicectl_main(int argc, char *argv[]) {

        static const struct {
                const char* verb;
                const enum {
                        MORE,
                        LESS,
                        EQUAL
                } argc_cmp;
                const int argc;
                int (* const dispatch)(char **args, unsigned n);
        } verbs[] = {
                { "monitor", LESS, 1, monitor_devices },
                { "show", EQUAL, 2, show_device },
                { "list", LESS, 1, list_devices },
                { "trigger", LESS, 1, trigger },
        };

        int left;
        unsigned i;

        assert(argc >= 0);
        assert(argv);

        left = argc - optind;

        if (left <= 0) {
                /* Special rule: no arguments means "help" */
                help();
                return 0;
        } else {
                if (streq(argv[optind], "help")) {
                        help();
                        return 0;
                }

                for (i = 0; i < ELEMENTSOF(verbs); i++)
                        if (streq(argv[optind], verbs[i].verb))
                                break;

                if (i >= ELEMENTSOF(verbs)) {
                        log_error("Unknown operation %s", argv[optind]);
                        return -EINVAL;
                }
        }

        switch (verbs[i].argc_cmp) {

        case EQUAL:
                if (left != verbs[i].argc) {
                        log_error("Invalid number of arguments.");
                        return -EINVAL;
                }

                break;

        case MORE:
                if (left < verbs[i].argc) {
                        log_error("Too few arguments.");
                        return -EINVAL;
                }

                break;

        case LESS:
                if (left > verbs[i].argc) {
                        log_error("Too many arguments.");
                        return -EINVAL;
                }

                break;

        default:
                assert_not_reached("Unknown comparison operator.");
        }

        return verbs[i].dispatch(argv + optind, left);
}

int main(int argc, char* argv[]) {
        int r;

        log_parse_environment();
        log_open();

        r = parse_argv(argc, argv);
        if (r <= 0)
                goto finish;

        r = devicectl_main(argc, argv);

finish:
        pager_close();

        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
