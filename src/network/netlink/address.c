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

#include "netlink/address.h"

#define CACHE_INFO_INFINITY_LIFE_TIME 0xFFFFFFFFU

int nl_address_new(NLAddress **addressp, sd_netlink_message *message) {
        _cleanup_(nl_address_unrefp) NLAddress *address = NULL;
        const char *label;
        uint16_t type;
        int r;

        r = sd_netlink_message_get_errno(message);
        if (r < 0)
                return r;

        r = sd_netlink_message_get_type(message, &type);
        if (r < 0)
                return r;
        if (type != RTM_NEWADDR)
                return -EINVAL;

        address = new0(NLAddress, 1);
        if (!address)
                return -ENOMEM;

        address->n_ref = 1;
        nl_address_init(address);

        r = sd_rtnl_message_addr_get_ifindex(message, &address->ifindex);
        if (r < 0)
                return r;
        else if (address->ifindex <= 0)
                return -EINVAL;

        (void) sd_rtnl_message_addr_get_family(message, &address->family);
        (void) sd_rtnl_message_addr_get_prefixlen(message, &address->prefixlen);

        switch (address->family) {
        case AF_INET:
                (void) sd_netlink_message_read_in_addr(message, IFA_LOCAL, &address->in_addr.in);
                (void) sd_netlink_message_read_in_addr(message, IFA_ADDRESS, &address->in_addr_peer.in);

                break;
        case AF_INET6:
                (void) sd_netlink_message_read_in6_addr(message, IFA_LOCAL, &address->in_addr.in6);
                (void) sd_netlink_message_read_in6_addr(message, IFA_ADDRESS, &address->in_addr_peer.in6);

                break;
        default:
                break;
        }

        (void) sd_netlink_message_read_in_addr(message, IFA_BROADCAST, &address->broadcast);
        (void) sd_rtnl_message_addr_get_scope(message, &address->scope);
        (void) sd_rtnl_message_addr_get_flags(message, &address->flags);

        r = sd_netlink_message_read_string(message, IFA_LABEL, &label);
        if (r >= 0) {
                address->label = strdup(label);
                if (!address->label)
                        return -ENOMEM;
        }

        (void) sd_netlink_message_read_cache_info(message, IFA_CACHEINFO, &address->cinfo);

        *addressp = address;
        address = NULL;

        return 0;
}

NLAddress *nl_address_unref(NLAddress *address) {
        if (!address || --address->n_ref > 0)
                return NULL;

        free(address);

        return NULL;
}

NLAddress *nl_address_ref(NLAddress *address) {
        if (address)
                address->n_ref ++;

        return address;
}

void nl_address_init(NLAddress *address) {
        address->family = AF_UNSPEC;
        address->scope = RT_SCOPE_UNIVERSE;
        address->cinfo.ifa_prefered = CACHE_INFO_INFINITY_LIFE_TIME;
        address->cinfo.ifa_valid = CACHE_INFO_INFINITY_LIFE_TIME;
}

static void address_hash_func(const void *b, struct siphash *state) {
        const NLAddress *address = b;

        siphash24_compress(&address->ifindex, sizeof(address->ifindex), state);
        siphash24_compress(&address->family, sizeof(address->family), state);

        switch (address->family) {
        case AF_INET:
                siphash24_compress(&address->prefixlen, sizeof(address->prefixlen), state);

                /* peer prefix */
                if (address->prefixlen != 0) {
                        uint32_t prefix;

                        if (address->in_addr_peer.in.s_addr != 0)
                                prefix = be32toh(address->in_addr_peer.in.s_addr) >> (32 - address->prefixlen);
                        else
                                prefix = be32toh(address->in_addr.in.s_addr) >> (32 - address->prefixlen);

                        siphash24_compress(&prefix, sizeof(prefix), state);
                }

                /* fallthrough */
        case AF_INET6:
                siphash24_compress(&address->in_addr, FAMILY_ADDRESS_SIZE(address->family), state);

                break;
        default:
                /* treat any other address family as AF_UNSPEC */
                break;
        }
}

static int address_compare_func(const void *a, const void *b) {
        const NLAddress *x = a, *y = b;

        if (x->ifindex < y->ifindex)
                return -1;
        if (x->ifindex > y->ifindex)
                return 1;

        if (x->family < y->family)
                return -1;
        if (x->family > y->family)
                return 1;

        switch (x->family) {
        /* use the same notion of equality as the kernel does */
        case AF_INET:
                if (x->prefixlen < y->prefixlen)
                        return -1;
                if (x->prefixlen > y->prefixlen)
                        return 1;

                /* compare the peer prefixes */
                if (x->prefixlen != 0) {
                        /* make sure we don't try to shift by 32.
                         * See ISO/IEC 9899:TC3 § 6.5.7.3. */
                        uint32_t x2, y2;

                        if (x->in_addr_peer.in.s_addr != 0)
                                x2 = be32toh(x->in_addr_peer.in.s_addr) >> (32 - x->prefixlen);
                        else
                                x2 = be32toh(x->in_addr.in.s_addr) >> (32 - x->prefixlen);

                        if (y->in_addr_peer.in.s_addr != 0)
                                y2 = be32toh(y->in_addr_peer.in.s_addr) >> (32 - y->prefixlen);
                        else
                                y2 = be32toh(y->in_addr.in.s_addr) >> (32 - y->prefixlen);

                        if (x2 < y2)
                                return -1;
                        if (x2 > y2)
                                return 1;
                }

                /* fall-through */
        case AF_INET6:
                return memcmp(&x->in_addr, &y->in_addr, FAMILY_ADDRESS_SIZE(x->family));
        default:
                /* treat any other address family as AF_UNSPEC */
                return 0;
        }
}

const struct hash_ops nl_address_hash_ops = {
        .hash = address_hash_func,
        .compare = address_compare_func,
};
