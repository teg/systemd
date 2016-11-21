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

#include "netlink/address.h"
#include "netlink/link.h"
#include "netlink/manager.h"
#include "netlink/route.h"

struct NLSlot {
        LIST_FIELDS(NLSlot, slots);
        union {
                nl_link_handler_t link;
                nl_address_handler_t address;
                nl_route_handler_t route;
        } callback;
        void *userdata;

        /* XXX: drop these and use a better linked list */
        NLManager *manager;
        NLLink *link;
        NLAddress *address;
        NLRoute *route;
};

void nl_slot_free(NLSlot *slot);

DEFINE_TRIVIAL_CLEANUP_FUNC(NLSlot*, nl_slot_free);
