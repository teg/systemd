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

#include <arpa/inet.h>
#include <linux/if.h>

#include "sd-netlink.h"

#include "alloc-util.h"
#include "hashmap.h"
#include "set.h"

#include "rtnl/manager.h"
#include "rtnl/route.h"
#include "rtnl/slot.h"

void rtnl_route_data_init(RTNLRouteData *data) {
        *data = (RTNLRouteData) {};
}

int rtnl_route_data_new_from_message(RTNLRouteData **datap, sd_netlink_message *message) {
        _cleanup_(rtnl_route_data_unrefp) RTNLRouteData *data = NULL;
        uint16_t type;
        uint8_t table;
        int r;

        r = sd_netlink_message_get_errno(message);
        if (r < 0)
                return r;

        r = sd_netlink_message_get_type(message, &type);
        if (r < 0)
                return r;
        if (type != RTM_NEWROUTE)
                return -EINVAL;

        data = new0(RTNLRouteData, 1);
        if (!data)
                return -ENOMEM;

        rtnl_route_data_init(data);

        data->n_ref = 1;

        (void) sd_rtnl_message_route_get_family(message, &data->family);
        (void) sd_rtnl_message_route_get_protocol(message, &data->protocol);
        (void) sd_rtnl_message_route_get_dst_prefixlen(message, &data->dst_prefixlen);
        (void) sd_rtnl_message_route_get_src_prefixlen(message, &data->src_prefixlen);
        (void) sd_rtnl_message_route_get_tos(message, &data->tos);

        switch (data->family) {
        case AF_INET:
                (void) sd_netlink_message_read_in_addr(message, RTA_GATEWAY, &data->gw.in);
                (void) sd_netlink_message_read_in_addr(message, RTA_PREFSRC, &data->prefsrc.in);
                (void) sd_netlink_message_read_in_addr(message, RTA_DST, &data->dst.in);
                (void) sd_netlink_message_read_in_addr(message, RTA_SRC, &data->src.in);

                break;
        case AF_INET6:
                (void) sd_netlink_message_read_in6_addr(message, RTA_GATEWAY, &data->gw.in6);
                (void) sd_netlink_message_read_in6_addr(message, RTA_PREFSRC, &data->prefsrc.in6);
                (void) sd_netlink_message_read_in6_addr(message, RTA_DST, &data->dst.in6);
                (void) sd_netlink_message_read_in6_addr(message, RTA_SRC, &data->src.in6);

                break;
        default:
                break;
        }

        (void) sd_rtnl_message_route_get_table(message, &table);
        data->table = table;
        (void) sd_netlink_message_read_u32(message, RTA_TABLE, &data->table);
        (void) sd_netlink_message_read_u32(message, RTA_PRIORITY, &data->priority);
        (void) sd_netlink_message_read_u8(message, RTA_PREF, &data->pref);
        (void) sd_netlink_message_read_u32(message, RTA_OIF, &data->oif);

        *datap = data;
        data = NULL;

        return 0;
}

RTNLRouteData *rtnl_route_data_ref(RTNLRouteData *data) {
        if (!data)
                return NULL;

        assert(data->n_ref > 0);
        data->n_ref ++;

        return data;
}

RTNLRouteData *rtnl_route_data_unref(RTNLRouteData *data) {
        if (!data)
                return NULL;

        assert(data->n_ref > 0);

        if (-- data->n_ref > 0)
                return NULL;

        free(data);

        return NULL;
}

int rtnl_route_new_from_data(RTNLRoute **routep, RTNLRouteData *data) {
        _cleanup_(rtnl_route_freep) RTNLRoute *route = NULL;

        route = new0(RTNLRoute, 1);
        if (!route)
                return -ENOMEM;

        route->family = data->family;
        route->table = data->table;
        route->priority = data->priority;
        route->dst = data->dst;
        route->dst_prefixlen = data->dst_prefixlen;
        route->tos = data->tos;
        route->oif = data->oif;

        *routep = route;
        route = NULL;
        return 0;
}

int rtnl_route_new_from_message(RTNLRoute **routep, sd_netlink_message *message) {
        _cleanup_(rtnl_route_freep) RTNLRoute *route = NULL;
        _cleanup_(rtnl_route_data_unrefp) RTNLRouteData *data = NULL;
        int r;

        r = rtnl_route_data_new_from_message(&data, message);
        if (r < 0)
                return r;

        r = rtnl_route_new_from_data(&route, data);
        if (r < 0)
                return r;

        route->data = rtnl_route_data_ref(data);

        *routep = route;
        route = NULL;

        return 0;
}

void rtnl_route_free(RTNLRoute *route) {
        if (!route)
                return;

        rtnl_route_data_unref(route->data);
        free(route);
}

int rtnl_route_attach(RTNLManager *manager, RTNLRoute *route) {
        RTNLSlot *slot;
        int r;

        r = set_put(manager->routes, route);
        if (r < 0)
                return r;

        route->manager = manager;

        LIST_FOREACH(slots, slot, manager->route_subscriptions)
                slot->callback.route(route, slot->userdata);

        return 0;
}

void rtnl_route_detach(RTNLRoute *route) {
        RTNLSlot *slot;

        set_remove(route->manager->routes, route);

        route->manager = NULL;

        LIST_FOREACH(slots, slot, route->subscriptions)
                slot->callback.route(NULL, slot->userdata);
}

int rtnl_route_subscribe(RTNLRoute *route, RTNLSlot **slotp, rtnl_route_handler_t callback, void *userdata) {
        _cleanup_(rtnl_slot_freep) RTNLSlot *slot = NULL;
        int r;

        r = rtnl_slot_new(&slot, userdata);
        if (r < 0)
                return r;

        slot->callback.route = callback;

        slot->route = route;
        LIST_APPEND(slots, route->subscriptions, slot);

        *slotp = slot;
        slot = NULL;

        return 0;
}

int rtnl_route_get_data(RTNLRoute *route, RTNLRouteData **datap) {
        *datap = route->data;
        return 0;
}

int rtnl_route_update_data(RTNLRoute *route, RTNLRouteData *data) {
        RTNLSlot *slot;

        rtnl_route_data_unref(route->data);
        route->data = rtnl_route_data_ref(data);

        LIST_FOREACH(slots, slot, route->subscriptions)
                slot->callback.route(route, slot->userdata);

        return 0;
}

static void route_hash_func(const void *b, struct siphash *state) {
        const RTNLRoute *route = b;
        union in_addr_union prefix;

        /* XXX: make sure this implements the same semantics as the kernel */

        siphash24_compress(&route->family, sizeof(route->family), state);
        siphash24_compress(&route->table, sizeof(route->table), state);
        siphash24_compress(&route->priority, sizeof(route->priority), state);
        siphash24_compress(&route->dst_prefixlen, sizeof(route->dst_prefixlen), state);

        prefix = route->dst;
        assert_se(!in_addr_mask(route->family, &prefix, route->dst_prefixlen));
        siphash24_compress(&prefix, FAMILY_ADDRESS_SIZE(route->family), state);

        switch (route->family) {
        case AF_INET:
                siphash24_compress(&route->tos, sizeof(route->tos), state);

                break;
        case AF_INET6:
                siphash24_compress(&route->oif, sizeof(route->oif), state);

                break;
        default:
                /* treat any other address family as AF_UNSPEC */
                break;
        }
}

static int route_compare_func(const void *a, const void *b) {
        const RTNLRoute *x = a, *y = b;
        union in_addr_union prefix1, prefix2;

        /* XXX: make sure this implements the same semantics as the kernel */

        if (x->family < y->family)
                return -1;
        if (x->family > y->family)
                return 1;

        if (x->table < y->table)
                return -1;
        if (x->table > y->table)
                return 1;

        if (x->priority < y->priority)
                return -1;
        if (x->priority > y->priority)
                return 1;

        if (x->dst_prefixlen < y->dst_prefixlen)
                return -1;
        if (x->dst_prefixlen > y->dst_prefixlen)
                return 1;

        switch (x->family) {
        case AF_INET:
                if (x->tos < y->tos)
                        return -1;
                if (x->tos > y->tos)
                        return 1;

                break;
        case AF_INET6:
                if (x->oif < y->oif)
                        return -1;
                if (x->oif > y->oif)
                        return 1;

                break;
        default:
                /* treat any other address family as AF_UNSPEC */
                break;
        }

        prefix1 = x->dst;
        assert_se(!in_addr_mask(x->family, &prefix1, x->dst_prefixlen));
        prefix2 = y->dst;
        assert_se(!in_addr_mask(y->family, &prefix2, y->dst_prefixlen));
        return memcmp(&prefix1, &prefix2, FAMILY_ADDRESS_SIZE(x->family));
}

const struct hash_ops rtnl_route_hash_ops = {
        .hash = route_hash_func,
        .compare = route_compare_func,
};
