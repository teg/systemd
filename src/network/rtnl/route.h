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

typedef struct RTNLRoute {
        RTNLManager *manager;

        int family;
        uint32_t table;
        uint32_t priority;
        union in_addr_union dst;
        unsigned char dst_prefixlen;
        unsigned char tos;
        uint32_t oif;

        RTNLRouteData *data;

        LIST_HEAD(RTNLSlot, subscriptions);
} RTNLRoute;

int rtnl_route_data_new_from_message(RTNLRouteData **datap, sd_netlink_message *message);

int rtnl_route_new_from_data(RTNLRoute **routep, RTNLRouteData *data);
int rtnl_route_new_from_message(RTNLRoute **routep, sd_netlink_message *message);
void rtnl_route_free(RTNLRoute *route);

int rtnl_route_attach(RTNLManager *manager, RTNLRoute *route);
void rtnl_route_detach(RTNLRoute *route);

int rtnl_route_update_data(RTNLRoute *route, RTNLRouteData *data);

extern const struct hash_ops rtnl_route_hash_ops;

DEFINE_TRIVIAL_CLEANUP_FUNC(RTNLRoute*, rtnl_route_free);
