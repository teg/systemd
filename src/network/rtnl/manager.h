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

typedef struct RTNLSlot RTNLSlot;
typedef struct sd_event sd_event;
typedef struct sd_netlink sd_netlink;
typedef struct Hashmap Hashmap;
typedef struct Set Set;

typedef struct RTNLManager{
        sd_netlink *rtnl;
        sd_event *event;

        bool enumerating_links:1;
        bool enumerating_addresses:1;
        bool enumerating_routes:1;

        LIST_HEAD(RTNLSlot, link_subscriptions);
        LIST_HEAD(RTNLSlot, address_subscriptions);
        LIST_HEAD(RTNLSlot, route_subscriptions);
        Hashmap *links;
        Set *addresses;
        Set *routes;
} RTNLManager;
