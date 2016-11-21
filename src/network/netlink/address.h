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

#include "in-addr-util.h"
#include "list.h"

typedef struct sd_netlink_message sd_netlink_message;
typedef struct NLSlot NLSlot;

typedef struct NLAddress {
        int n_ref;

        int ifindex;
        int family;
        unsigned char prefixlen;
        union in_addr_union in_addr_peer;
        union in_addr_union in_addr;

        struct in_addr broadcast;
        unsigned char scope;
        unsigned char flags;
        char *label;

        struct ifa_cacheinfo cinfo;

        LIST_HEAD(NLSlot, subscriptions);
} NLAddress;

typedef void (*nl_address_handler_t)(NLAddress *address, void *userdata);

int nl_address_new(NLAddress **addressp, sd_netlink_message *message);
NLAddress *nl_address_unref(NLAddress *address);
NLAddress *nl_address_ref(NLAddress *address);

void nl_address_init(NLAddress *address);

int nl_address_subscribe(NLAddress *address, NLSlot **slotp, nl_address_handler_t callback, void *userdata);

extern const struct hash_ops nl_address_hash_ops;

DEFINE_TRIVIAL_CLEANUP_FUNC(NLAddress*, nl_address_unref);
