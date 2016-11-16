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

#include "rtnl/address.h"
#include "rtnl/link.h"
#include "rtnl/manager.h"
#include "rtnl/slot.h"

#define CACHE_INFO_INFINITY_LIFE_TIME 0xFFFFFFFFU

void rtnl_address_data_init(RTNLAddressData *data) {
        *data = (RTNLAddressData) {};
        data->family = AF_UNSPEC;
        data->scope = RT_SCOPE_UNIVERSE;
        data->cinfo.ifa_prefered = CACHE_INFO_INFINITY_LIFE_TIME;
        data->cinfo.ifa_valid = CACHE_INFO_INFINITY_LIFE_TIME;
}

int rtnl_address_data_new_from_message(RTNLAddressData **datap, sd_netlink_message *message) {
        _cleanup_(rtnl_address_data_unrefp) RTNLAddressData *data = NULL;
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

        data = new0(RTNLAddressData, 1);
        if (!data)
                return -ENOMEM;

        rtnl_address_data_init(data);

        data->n_ref = 1;

        r = sd_rtnl_message_addr_get_ifindex(message, &data->ifindex);
        if (r < 0)
                return r;
        else if (data->ifindex <= 0)
                return -EINVAL;

        (void) sd_rtnl_message_addr_get_family(message, &data->family);
        (void) sd_rtnl_message_addr_get_prefixlen(message, &data->prefixlen);
        (void) sd_rtnl_message_addr_get_flags(message, &data->flags);

        switch (data->family) {
        case AF_INET:
                (void) sd_netlink_message_read_in_addr(message, IFA_LOCAL, &data->in_addr.in);
                (void) sd_netlink_message_read_in_addr(message, IFA_ADDRESS, &data->in_addr_peer.in);

                break;
        case AF_INET6:
                (void) sd_netlink_message_read_in6_addr(message, IFA_LOCAL, &data->in_addr.in6);
                (void) sd_netlink_message_read_in6_addr(message, IFA_ADDRESS, &data->in_addr_peer.in6);

                break;
        default:
                break;
        }

        (void) sd_netlink_message_read_in_addr(message, IFA_BROADCAST, &data->broadcast);
        (void) sd_rtnl_message_addr_get_scope(message, &data->scope);
        (void) sd_rtnl_message_addr_get_flags(message, &data->flags);

        r = sd_netlink_message_read_string(message, IFA_LABEL, &label);
        if (r >= 0) {
                data->label = strdup(label);
                if (!data->label)
                        return -ENOMEM;
        }

        (void) sd_netlink_message_read_cache_info(message, IFA_CACHEINFO, &data->cinfo);

        *datap = data;
        data = NULL;

        return 0;
}

RTNLAddressData *rtnl_address_data_ref(RTNLAddressData *data) {
        if (!data)
                return NULL;

        assert(data->n_ref > 0);
        data->n_ref ++;

        return data;
}

RTNLAddressData *rtnl_address_data_unref(RTNLAddressData *data) {
        if (!data)
                return NULL;

        assert(data->n_ref > 0);

        if (--data->n_ref > 0)
                return NULL;

        free(data->label);
        free(data);

        return NULL;
}

int rtnl_address_new_from_data(RTNLAddress **addressp, RTNLAddressData *data) {
        _cleanup_(rtnl_address_freep) RTNLAddress *address = NULL;

        address = new0(RTNLAddress, 1);
        if (!address)
                return -ENOMEM;

        address->ifindex = data->ifindex;
        address->family = data->family;
        address->prefixlen = data->prefixlen;
        address->in_addr_peer = data->in_addr_peer;
        address->in_addr = data->in_addr;

        *addressp = address;
        address = NULL;
        return 0;
}

int rtnl_address_new_from_message(RTNLAddress **addressp, sd_netlink_message *message) {
        _cleanup_(rtnl_address_freep) RTNLAddress *address = NULL;
        _cleanup_(rtnl_address_data_unrefp) RTNLAddressData *data = NULL;
        int r;

        r = rtnl_address_data_new_from_message(&data, message);
        if (r < 0)
                return r;

        r = rtnl_address_new_from_data(&address, data);
        if (r < 0)
                return r;

        address->data = rtnl_address_data_ref(data);

        *addressp = address;
        address = NULL;

        return 0;
}

void rtnl_address_free(RTNLAddress *address) {
        if (!address)
                return;

        rtnl_address_data_unref(address->data);

        free(address);
}

int rtnl_address_attach(RTNLManager *manager, RTNLAddress *address) {
        RTNLSlot *slot;
        RTNLLink *link;
        int r;

        r = set_put(manager->addresses, address);
        if (r < 0)
                return r;

        address->manager = manager;

        LIST_FOREACH(slots, slot, manager->address_subscriptions)
                slot->callback.address(address, slot->userdata);

        link = hashmap_get(manager->links, INT_TO_PTR(address->ifindex));
        if (link)
                rtnl_link_add_address(link, address);

        return 0;
}

void rtnl_address_detach(RTNLAddress *address) {
        RTNLSlot *slot;

        set_remove(address->manager->addresses, address);

        address->manager = NULL;

        LIST_FOREACH(slots, slot, address->subscriptions)
                slot->callback.address(NULL, slot->userdata);
}

int rtnl_address_subscribe(RTNLAddress *address, RTNLSlot **slotp, rtnl_address_handler_t callback, void *userdata) {
        _cleanup_(rtnl_slot_freep) RTNLSlot *slot = NULL;
        int r;

        r = rtnl_slot_new(&slot, userdata);
        if (r < 0)
                return 0;

        slot->callback.address = callback;

        slot->address = address;
        LIST_APPEND(slots, address->subscriptions, slot);

        *slotp = slot;
        slot = NULL;

        return 0;
}

int rtnl_address_get_data(RTNLAddress *address, RTNLAddressData **datap) {
        *datap = address->data;
        return 0;
}

int rtnl_address_update_data(RTNLAddress *address, RTNLAddressData *data) {
        RTNLSlot *slot;

        rtnl_address_data_unref(address->data);
        address->data = rtnl_address_data_ref(data);

        LIST_FOREACH(slots, slot, address->subscriptions)
                slot->callback.address(address, slot->userdata);

        return 0;
}

static void address_hash_func(const void *b, struct siphash *state) {
        const RTNLAddress *address = b;

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
        const RTNLAddress *x = a, *y = b;

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

const struct hash_ops rtnl_address_hash_ops = {
        .hash = address_hash_func,
        .compare = address_compare_func,
};
