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

#include "netlink/route.h"
#include "netlink/slot.h"

int nl_route_new(NLRoute **routep, sd_netlink_message *message) {
        _cleanup_(nl_route_unrefp) NLRoute *route = NULL;
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

        route = new0(NLRoute, 1);
        if (!route)
                return -ENOMEM;

        route->n_ref = 1;

        (void) sd_rtnl_message_route_get_family(message, &route->family);
        (void) sd_rtnl_message_route_get_protocol(message, &route->protocol);
        (void) sd_rtnl_message_route_get_dst_prefixlen(message, &route->dst_prefixlen);
        (void) sd_rtnl_message_route_get_src_prefixlen(message, &route->src_prefixlen);
        (void) sd_rtnl_message_route_get_tos(message, &route->tos);

        switch (route->family) {
        case AF_INET:
                (void) sd_netlink_message_read_in_addr(message, RTA_GATEWAY, &route->gw.in);
                (void) sd_netlink_message_read_in_addr(message, RTA_PREFSRC, &route->prefsrc.in);
                (void) sd_netlink_message_read_in_addr(message, RTA_DST, &route->dst.in);
                (void) sd_netlink_message_read_in_addr(message, RTA_SRC, &route->src.in);

                break;
        case AF_INET6:
                (void) sd_netlink_message_read_in6_addr(message, RTA_GATEWAY, &route->gw.in6);
                (void) sd_netlink_message_read_in6_addr(message, RTA_PREFSRC, &route->prefsrc.in6);
                (void) sd_netlink_message_read_in6_addr(message, RTA_DST, &route->dst.in6);
                (void) sd_netlink_message_read_in6_addr(message, RTA_SRC, &route->src.in6);

                break;
        default:
                break;
        }

        (void) sd_rtnl_message_route_get_table(message, &table);
        route->table = table;
        (void) sd_netlink_message_read_u32(message, RTA_TABLE, &route->table);
        (void) sd_netlink_message_read_u32(message, RTA_PRIORITY, &route->priority);
        (void) sd_netlink_message_read_u8(message, RTA_PREF, &route->pref);
        (void) sd_netlink_message_read_u32(message, RTA_OIF, &route->oif);

        *routep = route;
        route = NULL;

        return 0;
}

NLRoute *nl_route_unref(NLRoute *route) {
        if (!route || --route->n_ref > 0)
                return NULL;

        free(route);

        return NULL;
}

NLRoute *nl_route_ref(NLRoute *route) {
        if (route)
                route->n_ref ++;

        return route;
}

static void route_hash_func(const void *b, struct siphash *state) {
        const NLRoute *route = b;
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
        const NLRoute *x = a, *y = b;
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

const struct hash_ops nl_route_hash_ops = {
        .hash = route_hash_func,
        .compare = route_compare_func,
};

int nl_route_subscribe(NLRoute *route, NLSlot **slotp, nl_route_handler_t callback, void *userdata) {
        _cleanup_(nl_slot_freep) NLSlot *slot = NULL;

        slot = new0(NLSlot, 1);
        if (!slot)
                return -ENOMEM;

        slot->callback.route = callback;
        slot->userdata = userdata;

        slot->route = route;
        LIST_APPEND(slots, route->subscriptions, slot);

        if (slotp)
                /* XXX: handle cleanup */
                *slotp = slot;
        slot = NULL;

        return 0;
}
