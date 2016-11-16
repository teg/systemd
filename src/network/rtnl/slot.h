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

typedef struct sd_netlink sd_netlink;

struct RTNLSlot {
        LIST_FIELDS(RTNLSlot, slots);
        union {
                rtnl_link_handler_t link;
                rtnl_address_handler_t address;
                rtnl_route_handler_t route;
        } callback;
        void *userdata;

        sd_netlink *rtnl;
        uint32_t serial;

        /* XXX: drop these and use a better linked list */
        RTNLManager *manager;
        int ifindex;
        RTNLLink *link;
        RTNLAddress *address;
        RTNLRoute *route;
};

int rtnl_slot_new(RTNLSlot **slotp, void *userdata);

DEFINE_TRIVIAL_CLEANUP_FUNC(RTNLSlot*, rtnl_slot_free);
