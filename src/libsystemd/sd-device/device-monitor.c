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

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>

#include "device-monitor.h"
#include "device-util.h"
#include "device-internal.h"
#include "event-util.h"

#include "socket-util.h"
#include "missing.h"
#include "hashmap.h"
#include "refcnt.h"
#include "strv.h"

struct DeviceMonitor {
        RefCount n_ref;

        int fd;
        union sockaddr_union snl;
        bool bound;

        sd_event *event;
        sd_event_source *source;
        int priority;

        uint8_t buf[8192];

        device_monitor_cb_t callback;
        void *userdata;
};

enum {
        UDEV_MONITOR_NONE,
        UDEV_MONITOR_KERNEL,
        UDEV_MONITOR_UDEV,
};

#define DEVICE_MONITOR_MAGIC                0xfeedcafe
struct device_monitor_netlink_header {
        /* "libudev" prefix to distinguish libudev and kernel messages */
        uint8_t prefix[8];
        /*
         * magic to protect against daemon <-> library message format mismatch
         * used in the kernel from socket filter rules; needs to be stored in network order
         */
        uint32_t magic;
        /* total length of header structure known to the sender */
        uint32_t header_size;
        /* properties string buffer */
        uint32_t properties_off;
        uint32_t properties_len;
        /*
         * unused in-kernel filter
         */
        uint32_t filter_subsystem_hash;
        uint32_t filter_devtype_hash;
        uint32_t filter_tag_bloom_hi;
        uint32_t filter_tag_bloom_lo;
};

static const char* const device_action_table[_DEVICE_ACTION_MAX] = {
        [DEVICE_ACTION_ADD] = "add",
        [DEVICE_ACTION_REMOVE] = "remove",
        [DEVICE_ACTION_CHANGE] = "change",
        [DEVICE_ACTION_MOVE] = "move",
        [DEVICE_ACTION_ONLINE] = "online",
        [DEVICE_ACTION_OFFLINE] = "offline",
};

DEFINE_STRING_TABLE_LOOKUP(device_action, DeviceAction);

static int monitor_new(DeviceMonitor **ret) {
        _cleanup_device_monitor_unref_ DeviceMonitor *monitor = NULL;

        monitor = new0(DeviceMonitor, 1);
        if (!monitor)
                return -ENOMEM;

        monitor->n_ref = REFCNT_INIT;

        *ret = monitor;
        monitor = NULL;

        return 0;
}

int device_monitor_new_from_netlink(DeviceMonitor **ret, int fd, sd_event *_event, int priority) {
        _cleanup_device_monitor_unref_ DeviceMonitor *monitor = NULL;
        _cleanup_event_unref_ sd_event *event = NULL;
        int r;

        assert_return(ret, -EINVAL);

        r = monitor_new(&monitor);
        if (r < 0)
                return r;

        if (_event)
                event = sd_event_ref(_event);
        else {
                r = sd_event_default(&event);
                if (r < 0)
                        return r;
        }

        if (fd < 0) {
                fd = socket(PF_NETLINK, SOCK_RAW|SOCK_CLOEXEC|SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
                if (fd < 0)
                        return -errno;
        } else
                monitor->bound = true;

        monitor->fd = fd;

        monitor->snl.nl.nl_family = AF_NETLINK;
        monitor->snl.nl.nl_groups = UDEV_MONITOR_KERNEL;

        monitor->priority = priority;
        monitor->event = event;
        event = NULL;

        *ret = monitor;
        monitor = NULL;

        return 0;
}

int device_monitor_set_receive_buffer_size(DeviceMonitor *monitor, int size) {
        int r;

        assert(monitor);
        assert(size > 0);

        r = setsockopt(monitor->fd, SOL_SOCKET, SO_RCVBUFFORCE, &size, sizeof(size));
        if (r < 0)
                return -errno;

        return 0;
}

static int monitor_receive_device(DeviceMonitor *monitor, sd_device **ret,
                                  usec_t *_timestamp, uint64_t *seqnum, DeviceAction *action, const char **devpath_old) {
        _cleanup_strv_free_ char **properties = NULL;
        struct iovec iov = {
                .iov_base = monitor->buf,
                .iov_len = sizeof(monitor->buf),
        };
        char cred_msg[CMSG_SPACE(sizeof(struct ucred)) +
                      CMSG_SPACE(sizeof(struct timeval))];
        union sockaddr_union snl;
        struct msghdr smsg = {
                .msg_iov = &iov,
                .msg_iovlen = 1,
                .msg_control = cred_msg,
                .msg_controllen = sizeof(cred_msg),
                .msg_name = &snl,
                .msg_namelen = sizeof(snl),
        };
        struct cmsghdr *cmsg;
        struct ucred *ucred;
        usec_t timestamp = 0;
        ssize_t buflen;
        size_t bufpos;
        int r;

        assert(monitor);
        assert(ret);
        assert(seqnum);
        assert(action);
        assert(devpath_old);

        buflen = recvmsg(monitor->fd, &smsg, 0);
        if (buflen < 0) {
                if (errno == EINTR)
                        return 0;

                log_debug("device-monitor: unable to receive message: %m");
                return -errno;
        }

        if ((size_t)buflen < sizeof(struct device_monitor_netlink_header) || (size_t)buflen >= sizeof(monitor->buf)) {
                log_debug("device-monitor: invalid message length: %zi", buflen);

                return 0;
        }

        if (snl.nl.nl_groups != UDEV_MONITOR_KERNEL) {
                log_debug("device-monitor: non-kernel netlink message from %"PRIu32" ignored", snl.nl.nl_pid);
                return 0;
        } else {
                if (snl.nl.nl_pid > 0) {
                        log_debug("device-monitor: multicast kernel netlink message from %"PRIu32" ignored", snl.nl.nl_pid);
                        return 0;
                }
        }

        for (cmsg = CMSG_FIRSTHDR(&smsg); cmsg; cmsg = CMSG_NXTHDR(&smsg, cmsg)) {
                if (cmsg->cmsg_level != SOL_SOCKET) {
                        log_warning("device-monitor: got unexpected sockopt level");
                        continue;
                }

                if (cmsg->cmsg_type == SCM_CREDENTIALS) {
                        if (cmsg->cmsg_len != CMSG_LEN(sizeof(struct ucred))) {
                                log_warning("device-monitor: wrong ucred size");
                                continue;
                        }

                        ucred = (struct ucred*) CMSG_DATA(cmsg);
                } else if (cmsg->cmsg_type == SO_TIMESTAMP) {
                        if (cmsg->cmsg_len != CMSG_LEN(sizeof(struct timeval))) {
                                log_warning("device-monitor: wrong timeval size");
                                continue;
                        }

                        timestamp = timeval_load((struct timeval*)CMSG_DATA(cmsg));
                } else
                        log_warning("device-monitor: got unexpected sockopt");
        }

        if (!ucred) {
                log_debug("device-monitor: no sender credentials received, message ignored\n");
                return 0;
        }

        if (ucred->uid != 0) {
                log_debug("device-monitor: sender uid=%d, message ignored\n", ucred->uid);
                return 0;
        }

        /* kernel message with header */
        bufpos = strlen((char*)monitor->buf) + 1;
        if (bufpos < sizeof("a@/d") || bufpos >= (size_t)buflen) {
                log_debug("device-monitor: invalid message header length: %zd", bufpos);
                return 0;
        }

        /* check message header */
        if (strstr((char*)monitor->buf, "@/") == NULL) {
                log_debug("device-monitor: unrecognized message header: %s", monitor->buf);
                return 0;
        }

        r = device_new_from_nulstr(ret, seqnum, action, devpath_old, monitor->buf + bufpos, buflen - bufpos);
        if (r < 0)
                return r;

        *_timestamp = timestamp;

        return 1;
}

static int device_handler(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        DeviceMonitorEvent event = {};
        DeviceMonitor *monitor = userdata;
        int r;

        assert(monitor);

        r = monitor_receive_device(monitor, &event.device, &event.timestamp, &event.seqnum, &event.action, &event.devpath_old);
        if (r < 0) {
                return 1;
        } else if (r == 0)
                return 1;

        if (monitor->callback)
                monitor->callback(monitor, &event, monitor->userdata);

        sd_device_unref(event.device);

        return 1;
}

int device_monitor_start(DeviceMonitor *monitor) {
        _cleanup_event_source_unref_ sd_event_source *source = NULL;
        union sockaddr_union snl;
        socklen_t addrlen = sizeof(struct sockaddr_nl);
        const int on = 1;
        int r;

        assert_return(monitor, -EINVAL);
        assert_return(monitor->fd >= 0, -ENXIO);
        assert_return(monitor->event, -ENXIO);
        assert_return(!monitor->source, -EBUSY);

        if (!monitor->bound) {
                r = bind(monitor->fd, &monitor->snl.sa, sizeof(struct sockaddr_nl));
                if (r < 0)
                        return -errno;

                monitor->bound = true;
        }

        /*
         * get the address the kernel has assigned us
         * it is usually, but not necessarily the pid
         */
        r = getsockname(monitor->fd, &snl.sa, &addrlen);
        if (r < 0)
                return -errno;

        monitor->snl.nl.nl_pid = snl.nl.nl_pid;

        /* enable receiving of sender credentials */
        r = setsockopt(monitor->fd, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on));
        if (r < 0)
                return -errno;

        /* use kernel timestamping for improved debugging */
        r = setsockopt(monitor->fd, SOL_SOCKET, SO_TIMESTAMP, &on, sizeof(on));
        if (r < 0) {
                log_warning("could not set TIMESTAMP: %m");
                return -errno;
        }

        r = sd_event_add_io(monitor->event, &source, monitor->fd, EPOLLIN, device_handler, monitor);
        if (r < 0)
                return r;

        r = sd_event_source_set_priority(source, monitor->priority);
        if (r < 0)
                return r;

        monitor->source = source;
        source = NULL;

        return 0;
}

int device_monitor_stop(DeviceMonitor *monitor) {
        assert_return(monitor, -EINVAL);

        monitor->source = sd_event_source_unref(monitor->source);

        return 0;
}

DeviceMonitor *device_monitor_ref(DeviceMonitor *monitor) {
        assert_return(monitor, NULL);

        assert_se(REFCNT_INC(monitor->n_ref) >= 2);

        return monitor;
}

DeviceMonitor *device_monitor_unref(DeviceMonitor *monitor) {
        if (monitor && REFCNT_DEC(monitor->n_ref) <= 0) {
                device_monitor_stop(monitor);
                sd_event_unref(monitor->event);
                free(monitor);
        }

        return NULL;
}

int device_monitor_set_callback(DeviceMonitor *monitor, device_monitor_cb_t cb, void *userdata) {
        assert_return(monitor, -EINVAL);

        monitor->callback = cb;
        monitor->userdata = userdata;

        return 0;
}
