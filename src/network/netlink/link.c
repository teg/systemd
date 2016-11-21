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

#include "netlink/link.h"
#include "netlink/slot.h"

int nl_link_new(NLLink **linkp, sd_netlink_message *message) {
        _cleanup_(nl_link_unrefp) NLLink *link = NULL;
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

        link = new0(NLLink, 1);
        if (!link)
                return -ENOMEM;

        link->n_ref = 1;

        r = sd_rtnl_message_link_get_ifindex(message, &link->ifindex);
        if (r < 0)
                return r;
        if (link->ifindex <= 0)
                return -EINVAL;

        r = sd_netlink_message_read_string(message, IFLA_IFNAME, &ifname);
        if (r < 0)
                return r;

        link->ifname = strdup(ifname);
        if (!link->ifname)
                return -ENOMEM;

        r = sd_netlink_message_enter_container(message, IFLA_LINKINFO);
        if (r >= 0) {
                r = sd_netlink_message_read_string(message, IFLA_INFO_KIND, &kind);
                if (r >= 0) {
                        link->kind = strdup(kind);
                        if (!link->kind)
                                return -ENOMEM;
                }

                r = sd_netlink_message_exit_container(message);
                if (r < 0)
                        return r;
        }

        (void) sd_rtnl_message_link_get_type(message, &link->iftype);
        (void) sd_netlink_message_read_u32(message, IFLA_MTU, &link->mtu);
        (void) sd_netlink_message_read_ether_addr(message, IFLA_ADDRESS, &link->address);
        (void) sd_rtnl_message_link_get_flags(message, &link->flags);

        link->operstate = IF_OPER_UNKNOWN;
        (void) sd_netlink_message_read_u8(message, IFLA_OPERSTATE, &link->operstate);
        if (link->operstate == IF_OPER_UNKNOWN) {
                if (link->flags & IFF_DORMANT)
                        link->operstate = IF_OPER_DORMANT;
                else if (link->flags & IFF_LOWER_UP)
                        link->operstate = IF_OPER_UP;
                else
                        link->operstate = IF_OPER_DOWN;
        }

        *linkp = link;
        link = NULL;

        return 0;
}

NLLink *nl_link_unref(NLLink *link) {
        if (!link || --link->n_ref > 0)
                return NULL;

        free(link->kind);
        free(link->ifname);
        free(link);

        return NULL;
}

NLLink *nl_link_ref(NLLink *link) {
        if (link)
                link->n_ref ++;

        return link;
}

int nl_link_subscribe(NLLink *link, NLSlot **slotp, nl_link_handler_t callback, void *userdata) {
        _cleanup_(nl_slot_freep) NLSlot *slot = NULL;

        slot = new0(NLSlot, 1);
        if (!slot)
                return -ENOMEM;

        slot->callback.link = callback;
        slot->userdata = userdata;

        slot->link = link;
        LIST_APPEND(slots, link->subscriptions, slot);

        if (slotp)
                /* XXX: handle cleanup */
                *slotp = slot;
        slot = NULL;

        return 0;
}
