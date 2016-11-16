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

#include "list.h"

#include "rtnl/rtnl.h"

typedef struct RTNLManager RTNLManager;
typedef struct sd_netlink_message sd_netlink_message;

typedef struct RTNLAddress {
        RTNLManager *manager;

        int ifindex;
        int family;
        unsigned char prefixlen;
        unsigned char flags;
        union in_addr_union in_addr_peer;
        union in_addr_union in_addr;

        RTNLAddressData *data;

        LIST_HEAD(RTNLSlot, subscriptions);
} RTNLAddress;

int rtnl_address_data_new_from_message(RTNLAddressData **datap, sd_netlink_message *message);

int rtnl_address_new_from_data(RTNLAddress **addressp, RTNLAddressData *data);
int rtnl_address_new_from_message(RTNLAddress **addressp, sd_netlink_message *message);
void rtnl_address_free(RTNLAddress *address);

int rtnl_address_attach(RTNLManager *manager, RTNLAddress *address);
void rtnl_address_detach(RTNLAddress *address);

int rtnl_address_update_data(RTNLAddress *address, RTNLAddressData *data);

extern const struct hash_ops rtnl_address_hash_ops;

DEFINE_TRIVIAL_CLEANUP_FUNC(RTNLAddress*, rtnl_address_free);
