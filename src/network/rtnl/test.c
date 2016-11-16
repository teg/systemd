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

#include "rtnl/rtnl.h"

static bool got_link = false;
static bool created_address = false;

typedef struct Manager {
        RTNLManager *manager;
        RTNLSlot *slot;
} Manager;

typedef struct Link {
        RTNLSlot *slot;
        RTNLLinkData *data;
} Link;

typedef struct Address {
        RTNLSlot *slot;
        RTNLAddressData *data;
} Address;

typedef struct Route {
        RTNLSlot *slot;
        RTNLRouteData *data;
} Route;

static int link_new(Link **linkp) {
        Link *link;

        link = new0(Link, 1);
        if (!link)
                return -ENOMEM;

        *linkp = link;
        return 0;
}

static void link_free(Link *link) {
        if (!link)
                return;

        rtnl_link_data_unref(link->data);
        rtnl_slot_free(link->slot);

        free(link);
}

static int address_new(Address **addressp) {
        Address *address;

        address = new0(Address, 1);
        if (!address)
                return -ENOMEM;

        *addressp = address;
        return 0;
}

static void address_free(Address *address) {
        if (!address)
                return;

        rtnl_address_data_unref(address->data);
        rtnl_slot_free(address->slot);

        free(address);
}

static int route_new(Route **routep) {
        Route *route;

        route = new0(Route, 1);
        if (!route)
                return -ENOMEM;

        *routep = route;
        return 0;
}

static void route_free(Route *route) {
        if (!route)
                return;

        rtnl_route_data_unref(route->data);
        rtnl_slot_free(route->slot);

        free(route);
}

static void changed_link_handler(RTNLLink *rtnl_link, void *userdata) {
        Link *link = userdata;
        int r;

        if (rtnl_link) {
                RTNLLinkData *data;

                r = rtnl_link_get_data(rtnl_link, &data);
                assert(r >= 0);

                log_info("changed '%s': 0x%x -> 0x%x", data->ifname, link->data ? link->data->flags : 0, data->flags);

                rtnl_link_data_unref(link->data);
                link->data = rtnl_link_data_ref(data);
        } else {
                log_info("dropped link '%s'", link->data ? link->data->ifname : "n/a");
                link_free(link);
        }
}

static void add_link_handler(RTNLLink *rtnl_link, void *userdata) {
        RTNLLinkData *data;
        Link *link;
        int r;

        r = rtnl_link_get_data(rtnl_link, &data);
        assert(r >= 0);

        log_info("new %s '%s': 0x%x", data->kind ?: "link", data->ifname, data->flags);

        r = link_new(&link);
        assert(r >= 0);

        link->data = rtnl_link_data_ref(data);

        r = rtnl_link_subscribe(rtnl_link, &link->slot, changed_link_handler, link);
        if (r < 0)
                log_warning_errno(r, "could not subscribe to link: %m");
}

static void changed_address_handler(RTNLAddress *rtnl_address, void *userdata) {
        Address *address = userdata;
        _cleanup_free_ char *addrstr = NULL;
        int r;

        if (rtnl_address) {
                RTNLAddressData *data;

                r = rtnl_address_get_data(rtnl_address, &data);
                assert(r >= 0);

                r = in_addr_ifindex_to_string(data->family, &data->in_addr_peer, data->ifindex, &addrstr);
                if (r < 0)
                        log_warning_errno(r, "invalid address: %m");

                log_info("changed address '%s/%u'", addrstr, data->prefixlen);
        } else {
                log_info("dropped address");
                address_free(address);
        }
}

static void add_address_handler(RTNLAddress *rtnl_address, void *userdata) {
        RTNLAddressData *data;
        Address *address;
        _cleanup_free_ char *addrstr = NULL;
        int r;

        r = rtnl_address_get_data(rtnl_address, &data);
        assert(r >= 0);

        r = in_addr_ifindex_to_string(data->family, &data->in_addr_peer, data->ifindex, &addrstr);
        if (r < 0)
                log_warning_errno(r, "invalid address: %m");

        log_info("new address '%s/%u'", addrstr, data->prefixlen);

        r = address_new(&address);
        assert(r >= 0);

        r = rtnl_address_subscribe(rtnl_address, &address->slot, changed_address_handler, address);
        if (r < 0)
                log_warning_errno(r, "could not subscribe to address: %m");
}

static void changed_route_handler(RTNLRoute *rtnl_route, void *userdata) {
        Route *route = userdata;

        if (route) {
                RTNLRouteData *data;
                _cleanup_free_ char *prefix = NULL, *gw = NULL;
                char dev_buf[IF_NAMESIZE];
                static char *dev;
                int r;

                r = rtnl_route_get_data(rtnl_route, &data);
                assert(r >= 0);

                r = in_addr_to_string(data->family, &data->dst, &prefix);
                assert(r >= 0);

                if (!in_addr_is_null(data->family, &data->gw)) {
                        r = in_addr_to_string(data->family, &data->gw, &gw);
                        assert(r >= 0);
                }

                dev = if_indextoname(data->oif, dev_buf);

                log_info("changed route '%s/%u'%s%s%s%s",
                         prefix, data->dst_prefixlen,
                         gw ? " via " : "", gw ?: "",
                         dev ? " dev " : "", dev ?: "");
        } else {
                log_info("dropped route");
                route_free(route);
        }
}

static void add_route_handler(RTNLRoute *rtnl_route, void *userdata) {
        RTNLRouteData *data;
        Route *route;
        _cleanup_free_ char *prefix = NULL, *gw = NULL;
        char dev_buf[IF_NAMESIZE];
        static char *dev;
        int r;

        r = rtnl_route_get_data(rtnl_route, &data);
        assert(r >= 0);

        r = in_addr_to_string(data->family, &data->dst, &prefix);
        assert(r >= 0);

        if (!in_addr_is_null(data->family, &data->gw)) {
                r = in_addr_to_string(data->family, &data->gw, &gw);
                assert(r >= 0);
        }

        dev = if_indextoname(data->oif, dev_buf);

        log_info("new route '%s/%u'%s%s%s%s",
                 prefix, data->dst_prefixlen,
                 gw ? " via " : "", gw ?: "",
                 dev ? " dev " : "", dev ?: "");

        r = route_new(&route);
        assert(r >= 0);

        r = rtnl_route_subscribe(rtnl_route, &route->slot, changed_route_handler, route);
        assert(r >= 0);
}

static void create_address_handler(RTNLAddress *address, void *userdata) {
        Manager *context = userdata;
        RTNLManager *manager = context->manager;
        int r;

        assert(address);

        log_info("created address");
        created_address = true;

        rtnl_slot_free(context->slot);

        r = rtnl_manager_destroy_address(manager, address);
        assert(r >= 0);
}

static void get_link_handler(RTNLLink *link, void *userdata) {
        Manager *context = userdata;
        RTNLManager *manager = context->manager;
        RTNLLinkData *data;
        RTNLAddressData address;
        int r;

        assert(link);

        got_link = true;

        r = rtnl_link_get_data(link, &data);
        assert(r >= 0);

        assert(data->ifindex == 1);

        log_info("got link '%s'", data->ifname);

        rtnl_address_data_init(&address);

        address.family = AF_INET;
        address.prefixlen = 8;
        address.scope = RT_SCOPE_HOST;
        address.ifindex = 1;
        r = inet_pton(AF_INET, "127.1.1.1", &address.in_addr);
        assert(r == 1);

        rtnl_slot_free(context->slot);

        r = rtnl_manager_create_address(manager, &address, &context->slot, create_address_handler, context);
        assert(r >= 0);
}

int main(void) {
        Manager context = {};
        _cleanup_(rtnl_manager_freep) RTNLManager *manager = NULL;
        RTNLSlot *link_subscription, *address_subscription, *route_subscription;
        sd_event *event;
        RTNLLinkData link = {
                .ifindex = 1,
        };
        int r;

        r = sd_event_default(&event);
        assert(r >= 0);

        r = rtnl_manager_new(&manager, event);
        assert(r >= 0);

        context.manager = manager;

        r = rtnl_manager_subscribe_links(manager, &link_subscription, add_link_handler, NULL);
        assert(r >= 0);

        r = rtnl_manager_subscribe_addresses(manager, &address_subscription, add_address_handler, NULL);
        assert(r >= 0);

        r = rtnl_manager_subscribe_routes(manager, &route_subscription, add_route_handler, NULL);
        assert(r >= 0);

        r = rtnl_manager_start(manager);
        assert(r >= 0);

        r = rtnl_manager_get_link(manager, &link, &context.slot, get_link_handler, &context);
        assert(r >= 0);

        r = sd_event_loop(event);
        assert(r >= 0);

        assert(got_link);
        assert(created_address);

        rtnl_slot_free(link_subscription);
        rtnl_slot_free(address_subscription);
        rtnl_slot_free(route_subscription);

        sd_event_unref(event);
}
