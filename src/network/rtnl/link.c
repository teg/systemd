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

#include "rtnl/address.h"
#include "rtnl/link.h"
#include "rtnl/manager.h"
#include "rtnl/rtnl.h"
#include "rtnl/route.h"
#include "rtnl/slot.h"

void rtnl_link_data_init(RTNLLinkData *data) {
        *data = (RTNLLinkData) {};
}

int rtnl_link_data_new_from_message(RTNLLinkData **datap, sd_netlink_message *message) {
        _cleanup_(rtnl_link_data_unrefp) RTNLLinkData *data = NULL;
        const char *ifname, *kind;
        uint16_t type;
        int r;

        r = sd_netlink_message_get_errno(message);
        if (r < 0)
                return r;

        r = sd_netlink_message_get_type(message, &type);
        if (r < 0)
                return r;
        if (type != RTM_NEWLINK)
                return -EINVAL;

        data = new0(RTNLLinkData, 1);
        if (!data)
                return -ENOMEM;

        rtnl_link_data_init(data);

        data->n_ref = 1;

        r = sd_rtnl_message_link_get_ifindex(message, &data->ifindex);
        if (r < 0)
                return r;
        if (data->ifindex <= 0)
                return -EINVAL;

        r = sd_netlink_message_read_string(message, IFLA_IFNAME, &ifname);
        if (r < 0)
                return r;

        data->ifname = strdup(ifname);
        if (!data->ifname)
                return -ENOMEM;

        r = sd_netlink_message_enter_container(message, IFLA_LINKINFO);
        if (r >= 0) {
                r = sd_netlink_message_read_string(message, IFLA_INFO_KIND, &kind);
                if (r >= 0) {
                        data->kind = strdup(kind);
                        if (!data->kind)
                                return -ENOMEM;
                }

                r = sd_netlink_message_exit_container(message);
                if (r < 0)
                        return r;
        }

        (void) sd_rtnl_message_link_get_type(message, &data->iftype);
        (void) sd_netlink_message_read_u32(message, IFLA_MTU, &data->mtu);
        (void) sd_netlink_message_read_ether_addr(message, IFLA_ADDRESS, &data->address);
        (void) sd_rtnl_message_link_get_flags(message, &data->flags);

        data->operstate = IF_OPER_UNKNOWN;
        (void) sd_netlink_message_read_u8(message, IFLA_OPERSTATE, &data->operstate);
        if (data->operstate == IF_OPER_UNKNOWN) {
                if (data->flags & IFF_DORMANT)
                        data->operstate = IF_OPER_DORMANT;
                else if (data->flags & IFF_LOWER_UP)
                        data->operstate = IF_OPER_UP;
                else
                        data->operstate = IF_OPER_DOWN;
        }

        *datap = data;
        data = NULL;

        return 0;
}

RTNLLinkData *rtnl_link_data_ref(RTNLLinkData *data) {
        if (!data)
                return NULL;

        assert(data->n_ref > 0);
        data->n_ref ++;

        return data;
}

RTNLLinkData *rtnl_link_data_unref(RTNLLinkData *data) {
        if (!data)
                return NULL;

        assert(data->n_ref > 0);

        if (-- data->n_ref > 0)
                return NULL;

        free(data->kind);
        free(data->ifname);
        free(data);

        return NULL;
}

static void link_set_carrier_flag(RTNLLink *link) {
        bool carrier;

        switch (link->data->operstate) {
        case IF_OPER_UP:
                carrier = true;
                break;
        case IF_OPER_UNKNOWN:
                carrier = ((link->data->flags & IFF_LOWER_UP) && !(link->data->flags & IFF_DORMANT));
                break;
        default:
                carrier = false;
        }

        if (carrier)
                link->state |= RTNL_LINK_STATE_CARRIER;
        else
                link->state &= ~RTNL_LINK_STATE_CARRIER;

}

static int rtnl_link_new_from_data(RTNLLink **linkp, RTNLLinkData *data) {
        _cleanup_(rtnl_link_freep) RTNLLink *link = NULL;

        link = new0(RTNLLink, 1);
        if (!link)
                return -ENOMEM;

        link->ifindex = data->ifindex;
        link->data = rtnl_link_data_ref(data);

        link_set_carrier_flag(link);

        *linkp = link;
        link = NULL;

        return 0;
}

int rtnl_link_new_from_message(RTNLLink **linkp, sd_netlink_message *message) {
        _cleanup_(rtnl_link_freep) RTNLLink *link = NULL;
        _cleanup_(rtnl_link_data_unrefp) RTNLLinkData *data = NULL;
        int r;

        r = rtnl_link_data_new_from_message(&data, message);
        if (r < 0)
                return r;

        r = rtnl_link_new_from_data(&link, data);
        if (r < 0)
                return r;

        *linkp = link;
        link = NULL;

        return 0;
}

void rtnl_link_free(RTNLLink *link) {
        if (!link)
                return;

        rtnl_link_data_unref(link->data);
        free(link);
}

int rtnl_link_attach(RTNLManager *manager, RTNLLink *link) {
        RTNLSlot *slot;
        int r;

        r = hashmap_put(manager->links, INT_TO_PTR(link->ifindex), link);
        if (r < 0)
                return r;

        link->manager = manager;

        LIST_FOREACH(slots, slot, manager->link_subscriptions)
                slot->callback.link(link, slot->userdata);

        return 0;
}

void rtnl_link_detach(RTNLLink *link) {
        RTNLSlot *slot;

        hashmap_remove(link->manager->links, INT_TO_PTR(link->ifindex));

        link->manager = NULL;

        LIST_FOREACH(slots, slot, link->subscriptions)
                slot->callback.link(NULL, slot->userdata);
}

int rtnl_link_subscribe(RTNLLink *link, RTNLSlot **slotp, rtnl_link_handler_t callback, void *userdata) {
        _cleanup_(rtnl_slot_freep) RTNLSlot *slot = NULL;
        int r;

        r = rtnl_slot_new(&slot, userdata);
        if (r < 0)
                return r;

        slot->callback.link = callback;

        slot->link = link;
        LIST_APPEND(slots, link->subscriptions, slot);

        *slotp = slot;
        slot = NULL;

        return 0;
}

int rtnl_link_get_data(RTNLLink *link, RTNLLinkData **datap) {
        *datap = link->data;

        return 0;
}

int rtnl_link_get_state(RTNLLink *link, int *statep) {
        *statep = link->state;

        return 0;
}

int rtnl_link_update_data(RTNLLink *link, RTNLLinkData *data) {
        RTNLSlot *slot;

        rtnl_link_data_unref(link->data);
        link->data = rtnl_link_data_ref(data);

        link_set_carrier_flag(link);

        LIST_FOREACH(slots, slot, link->subscriptions)
                slot->callback.link(link, slot->userdata);

        return 0;
}

int rtnl_link_add_address(RTNLLink *link, RTNLAddress *address) {
        RTNLAddressData *data;
        RTNLSlot *slot;
        int r;

        r = rtnl_address_get_data(address, &data);
        if (r < 0)
                return r;

        /* ignore if we already have IPv6LL address on this link */
        if (link->state & RTNL_LINK_STATE_IPV6LL)
                return 0;

        /* ignore if this is not an IPv6 address */
        if (address->family == AF_INET6)
                return 0;

        /* ignore if this is not a link-local address */
        if (!IN6_IS_ADDR_LINKLOCAL(&address->in_addr.in6))
                return 0;

        /* ignore if the address is not valid */
        if (address->flags & (IFA_F_TENTATIVE | IFA_F_DEPRECATED))
                return 0;

        link->state |= RTNL_LINK_STATE_IPV6LL;

        LIST_FOREACH(slots, slot, link->subscriptions)
                slot->callback.link(link, slot->userdata);

        return 0;
}

int rtnl_link_create_address(RTNLLink *link, RTNLAddressData *address, RTNLSlot **slotp, rtnl_address_handler_t callback, void *userdata) {
        if (address->ifindex != link->ifindex)
                return -EINVAL;

        return rtnl_manager_create_address(link->manager, address, slotp, callback, userdata);
}

int rtnl_link_destroy_address(RTNLLink *link, RTNLAddress *address) {
        if (address->ifindex != link->ifindex)
                return -EINVAL;

        return rtnl_manager_destroy_address(link->manager, address);
}

int rtnl_link_create_route(RTNLLink *link, RTNLRouteData *route, RTNLSlot **slotp, rtnl_route_handler_t callback, void *userdata) {
        if ((int) route->oif != link->ifindex)
                return -EINVAL;

        return rtnl_manager_create_route(link->manager, route, slotp, callback, userdata);
}

int rtnl_link_destroy_route(RTNLLink *link, RTNLRoute *route) {
        if ((int) route->oif != link->ifindex)
                return -EINVAL;

        return rtnl_manager_destroy_route(link->manager, route);
}
