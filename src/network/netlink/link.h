#pragma once

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

#include <net/ethernet.h>

#include "list.h"

typedef struct sd_netlink_message sd_netlink_message;
typedef struct NLSlot NLSlot;

typedef struct NLLink {
        int n_ref;

        int ifindex;
        char *ifname;
        char *kind;
        unsigned short iftype;
        struct ether_addr address;
        uint32_t mtu;

        unsigned flags;
        uint8_t operstate;

        LIST_HEAD(NLSlot, subscriptions);
} NLLink;

typedef void (*nl_link_handler_t)(NLLink *link, void *userdata);

int nl_link_new(NLLink **linkp, sd_netlink_message *message);
NLLink *nl_link_unref(NLLink *link);
NLLink *nl_link_ref(NLLink *link);

int nl_link_subscribe(NLLink *link, NLSlot **slotp, nl_link_handler_t callback, void *userdata);

DEFINE_TRIVIAL_CLEANUP_FUNC(NLLink*, nl_link_unref);
