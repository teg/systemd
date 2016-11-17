/***
  This file is part of systemd.

  Copyright 2016 Tom Gundersen <teg@jklm.no>

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

#include <sys/socket.h>

#include "sd-daemon.h"
#include "sd-event.h"
#include "sd-netlink.h"

#include "alloc-util.h"
#include "fd-util.h"
#include "hashmap.h"
#include "netlink-util.h"
#include "set.h"

#include "netlink/address.h"
#include "netlink/link.h"
#include "netlink/manager.h"

/* use 16 MB for receive socket kernel queue. */
#define RCVBUF_SIZE    (16*1024*1024)

struct NLManager{
        sd_netlink *rtnl;
        sd_event *event;

        bool enumerating_links:1;
        bool enumerating_addresses:1;
        bool enumerating_routes:1;

        Hashmap *links;
        Set *addresses;
};

int nl_manager_new(NLManager **ret, sd_event *event) {
        _cleanup_(nl_manager_freep) NLManager *m = NULL;

        m = new0(NLManager, 1);
        if (!m)
                return -ENOMEM;

        m->event = sd_event_ref(event);

        m->links = hashmap_new(NULL);
        if (!m->links)
                return -ENOMEM;

        m->addresses = set_new(&nl_address_hash_ops);
        if (!m->addresses)
                return -ENOMEM;

        *ret = m;
        m = NULL;

        return 0;
}

void nl_manager_free(NLManager *m) {
        NLAddress *address;
        NLLink *link;

        if (!m)
                return;

        while ((address = set_steal_first(m->addresses)))
                nl_address_unref(address);
        set_free(m->addresses);

        while ((link = hashmap_steal_first(m->links)))
                nl_link_unref(link);
        hashmap_free(m->links);

        sd_netlink_unref(m->rtnl);
        sd_event_unref(m->event);

        free(m);
}

static int add_link(sd_netlink *rtnl, sd_netlink_message *message, void *userdata) {
        NLManager *m = userdata;
        _cleanup_(nl_link_unrefp) NLLink *new_link = NULL, *old_link = NULL;
        int r;

        if (m->enumerating_links)
                return 0;

        r = nl_link_new(&new_link, message);
        if (r < 0)
                return r;

        old_link = hashmap_remove(m->links, INT_TO_PTR(new_link->ifindex));
        r = hashmap_put(m->links, INT_TO_PTR(new_link->ifindex), new_link);
        if (r < 0)
                return r;
        new_link = NULL;

        if (old_link) {
                log_info("rtnl: updated link");

                /* XXX: implement subscriptions */
        } else {
                log_info("rtnl: added link");

                /* XXX: implement subscriptions */
        }

        return 1;
}

static int remove_link(sd_netlink *rtnl, sd_netlink_message *message, void *userdata) {
        NLManager *m = userdata;
        _cleanup_(nl_link_unrefp) NLLink *new_link = NULL, *old_link = NULL;
        int r;

        if (m->enumerating_links)
                return 0;

        r = nl_link_new(&new_link, message);
        if (r < 0)
                return r;

        old_link = hashmap_remove(m->links, INT_TO_PTR(new_link->ifindex));
        if (!old_link)
                return -ENODEV;

        log_info("rtnl: removed link");

        /* XXX: implement subscriptions */

        return 1;
}

static int add_address(sd_netlink *rtnl, sd_netlink_message *message, void *userdata) {
        NLManager *m = userdata;
        _cleanup_(nl_address_unrefp) NLAddress *new_address = NULL, *old_address = NULL;
        int r;

        if (m->enumerating_addresses)
                return 0;

        r = nl_address_new(&new_address, message);
        if (r < 0)
                return r;

        old_address = set_remove(m->addresses, new_address);
        r = set_put(m->addresses, new_address);
        if (r < 0)
                return r;
        new_address = NULL;

        if (old_address) {
                log_info("rtnl: updated address");

                /* XXX: implement subscriptions */
        } else {
                log_info("rtnl: added address");

                /* XXX: implement subscriptions */
        }

        return 1;
}

static int remove_address(sd_netlink *rtnl, sd_netlink_message *message, void *userdata) {
        NLManager *m = userdata;
        _cleanup_(nl_address_unrefp) NLAddress *new_address = NULL, *old_address = NULL;
        int r;

        if (m->enumerating_addresses)
                return 0;

        r = nl_address_new(&new_address, message);
        if (r < 0)
                return r;

        old_address = set_remove(m->addresses, new_address);
        if (!old_address)
                return -ENODEV;

        log_info("rtnl: removed address");

        /* XXX: implement subscriptions */

        return 1;
}

static int add_route(sd_netlink *rtnl, sd_netlink_message *message, void *userdata) {
        NLManager *m = userdata;

        if (m->enumerating_routes)
                return 0;

        /* XXX: implement route tracking */
        log_info("rtnl: got new route");

        return 1;
}

static int remove_route(sd_netlink *rtnl, sd_netlink_message *message, void *userdata) {
        NLManager *m = userdata;

        if (m->enumerating_routes)
                return 0;

        /* XXX: implement route tracking */
        log_info("rtnl: lost route");

        return 1;
}

static int enumerate_routes_handler(sd_netlink *rtnl, sd_netlink_message *reply, void *userdata) {
        NLManager *m = userdata;
        sd_netlink_message *route;
        int r = 0;

        m->enumerating_routes = false;

        for (route = reply; route; route = sd_netlink_message_next(route)) {
                int k;

                k = add_route(m->rtnl, route, m);
                if (k < 0)
                        r = k;
        }

        return r;
}

static int enumerate_routes(NLManager *m) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL, *reply = NULL;
        int r;

        r = sd_rtnl_message_new_route(m->rtnl, &req, RTM_GETROUTE, 0, 0);
        if (r < 0)
                return r;

        r = sd_netlink_message_request_dump(req, true);
        if (r < 0)
                return r;

        r = sd_netlink_call_async(m->rtnl, req, enumerate_routes_handler, m, 0, NULL);
        if (r < 0)
                return r;

        m->enumerating_routes = true;

        return 0;
}

static int enumerate_addresses_handler(sd_netlink *rtnl, sd_netlink_message *reply, void *userdata) {
        NLManager *m = userdata;
        sd_netlink_message *address;
        int r = 0;

        m->enumerating_addresses = false;

        for (address = reply; address; address = sd_netlink_message_next(address)) {
                int k;

                k = add_address(m->rtnl, address, m);
                if (k < 0)
                        r = k;
        }

        r = enumerate_routes(m);
        if (r < 0)
                return r;

        return r;
}

static int enumerate_addresses(NLManager *m) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL, *reply = NULL;
        int r;

        r = sd_rtnl_message_new_addr(m->rtnl, &req, RTM_GETADDR, 0, 0);
        if (r < 0)
                return r;

        r = sd_netlink_message_request_dump(req, true);
        if (r < 0)
                return r;

        r = sd_netlink_call_async(m->rtnl, req, enumerate_addresses_handler, m, 0, NULL);
        if (r < 0)
                return r;

        m->enumerating_addresses = true;

        return r;
}

static int enumerate_links_handler(sd_netlink *rtnl, sd_netlink_message *reply, void *userdata) {
        NLManager *m = userdata;
        sd_netlink_message *link;
        int r = 0;

        m->enumerating_links = false;

        for (link = reply; link; link = sd_netlink_message_next(link)) {
                int k;

                k = add_link(m->rtnl, link, m);
                if (k < 0)
                        r = k;
        }

        r = enumerate_addresses(m);
        if (r < 0)
                return r;

        return r;
}

static int enumerate_links(NLManager *m) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL, *reply = NULL;
        int r;

        r = sd_rtnl_message_new_link(m->rtnl, &req, RTM_GETLINK, 0);
        if (r < 0)
                return r;

        r = sd_netlink_message_request_dump(req, true);
        if (r < 0)
                return r;

        r = sd_netlink_call_async(m->rtnl, req, enumerate_links_handler, m, 0, NULL);
        if (r < 0)
                return r;

        return 0;
}

static int systemd_netlink_fd(void) {
        int n, fd, rtnl_fd = -EINVAL;

        n = sd_listen_fds(true);
        if (n <= 0)
                return -EINVAL;

        for (fd = SD_LISTEN_FDS_START; fd < SD_LISTEN_FDS_START + n; fd ++) {
                if (sd_is_socket(fd, AF_NETLINK, SOCK_RAW, -1) > 0) {
                        if (rtnl_fd >= 0)
                                return -EINVAL;

                        rtnl_fd = fd;
                }
        }

        return rtnl_fd;
}

int nl_manager_start(NLManager *m) {
        int r, fd;

        fd = systemd_netlink_fd();
        if (fd < 0)
                r = sd_netlink_open(&m->rtnl);
        else
                r = sd_netlink_open_fd(&m->rtnl, fd);
        if (r < 0)
                return r;

        r = sd_netlink_inc_rcvbuf(m->rtnl, RCVBUF_SIZE);
        if (r < 0)
                return r;

        r = sd_netlink_attach_event(m->rtnl, m->event, 0);
        if (r < 0)
                return r;

        r = sd_netlink_add_match(m->rtnl, RTM_NEWLINK, &add_link, m);
        if (r < 0)
                return r;

        r = sd_netlink_add_match(m->rtnl, RTM_DELLINK, &remove_link, m);
        if (r < 0)
                return r;

        r = sd_netlink_add_match(m->rtnl, RTM_NEWADDR, &add_address, m);
        if (r < 0)
                return r;

        r = sd_netlink_add_match(m->rtnl, RTM_DELADDR, &remove_address, m);
        if (r < 0)
                return r;

        r = sd_netlink_add_match(m->rtnl, RTM_NEWROUTE, &add_route, m);
        if (r < 0)
                return r;

        r = sd_netlink_add_match(m->rtnl, RTM_DELROUTE, &remove_route, m);
        if (r < 0)
                return r;

        r = enumerate_links(m);
        if (r < 0)
                return r;

        return 0;
}
