/***
  This file is part of systemd.

  Copyright 2013 Tom Gundersen <teg@jklm.no>

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

#include <arpa/inet.h>
#include <net/if.h>
#include <sd-event.h>

#include "alloc-util.h"
#include "in-addr-util.h"

#include "netlink/address.h"
#include "netlink/link.h"
#include "netlink/manager.h"
#include "netlink/route.h"
#include "netlink/slot.h"

static void changed_link_handler(NLLink *link, void *userdata) {
        if (link)
                log_info("changed '%s'", link->ifname);
        else
                log_info("dropped link");
}

static void add_link_handler(NLLink *link, void *userdata) {
        int r;

        log_info("new %s '%s'", link->kind ?: "link", link->ifname);

        r = nl_link_subscribe(link, NULL, changed_link_handler, NULL);
        if (r < 0)
                log_warning_errno(r, "could not subscribe to link: %m");
}

static void changed_address_handler(NLAddress *address, void *userdata) {
        _cleanup_free_ char *addrstr = NULL;
        int r;

        if (address) {
                r = in_addr_ifindex_to_string(address->family, &address->in_addr_peer, address->ifindex, &addrstr);
                if (r < 0)
                        log_warning_errno(r, "invalid address: %m");

                log_info("changed address '%s/%u'", addrstr, address->prefixlen);
        } else
                log_info("dropped address");


}

static void add_address_handler(NLAddress *address, void *userdata) {
        _cleanup_free_ char *addrstr = NULL;
        int r;

        r = in_addr_ifindex_to_string(address->family, &address->in_addr_peer, address->ifindex, &addrstr);
        if (r < 0)
                log_warning_errno(r, "invalid address: %m");

        log_info("new address '%s/%u'", addrstr, address->prefixlen);

        r = nl_address_subscribe(address, NULL, changed_address_handler, NULL);
        if (r < 0)
                log_warning_errno(r, "could not subscribe to address: %m");
}

static void changed_route_handler(NLRoute *route, void *userdata) {
        if (route) {
                _cleanup_free_ char *prefix = NULL, *gw = NULL;
                char dev_buf[IF_NAMESIZE];
                static char *dev;
                int r;

                r = in_addr_to_string(route->family, &route->dst, &prefix);
                if (r < 0)
                        log_warning_errno(r, "invalid address: %m");

                if (!in_addr_is_null(route->family, &route->gw)) {
                        r = in_addr_to_string(route->family, &route->gw, &gw);
                        if (r < 0)
                                log_warning_errno(r, "invalid address: %m");
                }

                dev = if_indextoname(route->oif, dev_buf);

                log_info("changed route '%s/%u'%s%s%s%s",
                         prefix, route->dst_prefixlen,
                         gw ? " via " : "", gw ?: "",
                         dev ? " dev " : "", dev ?: "");
        } else
                log_info("dropped route");
}

static void add_route_handler(NLRoute *route, void *userdata) {
        _cleanup_free_ char *prefix = NULL, *gw = NULL;
        char dev_buf[IF_NAMESIZE];
        static char *dev;
        int r;

        r = in_addr_to_string(route->family, &route->dst, &prefix);
        if (r < 0)
                log_warning_errno(r, "invalid address: %m");

        if (!in_addr_is_null(route->family, &route->gw)) {
                r = in_addr_to_string(route->family, &route->gw, &gw);
                if (r < 0)
                        log_warning_errno(r, "invalid address: %m");
        }

        dev = if_indextoname(route->oif, dev_buf);

        log_info("new route '%s/%u'%s%s%s%s",
                 prefix, route->dst_prefixlen,
                 gw ? " via " : "", gw ?: "",
                 dev ? " dev " : "", dev ?: "");

        r = nl_route_subscribe(route, NULL, changed_route_handler, NULL);
        if (r < 0)
                log_warning_errno(r, "could not subscribe to route: %m");
}

int main(void) {
        _cleanup_(nl_manager_freep) NLManager *manager = NULL;
        _cleanup_(nl_slot_freep) NLSlot *link_subscription = NULL;
        _cleanup_(nl_slot_freep) NLSlot *address_subscription = NULL;
        _cleanup_(nl_slot_freep) NLSlot *route_subscription = NULL;
        sd_event *event;
        int r;

        r = sd_event_default(&event);
        assert(r >= 0);

        r = nl_manager_new(&manager, event);
        assert(r >= 0);

        r = nl_manager_start(manager);
        assert(r >= 0);

        r = nl_manager_subscribe_links(manager, &link_subscription, add_link_handler, NULL);
        assert(r >= 0);

        r = nl_manager_subscribe_addresses(manager, &address_subscription, add_address_handler, NULL);
        assert(r >= 0);

        r = nl_manager_subscribe_routes(manager, &route_subscription, add_route_handler, NULL);
        assert(r >= 0);

        r = sd_event_loop(event);
        assert(r >= 0);

        sd_event_unref(event);
}
