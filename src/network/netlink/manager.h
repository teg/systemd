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
#include "netlink/route.h"

typedef struct NLSlot NLSlot;
typedef struct sd_event sd_event;
typedef struct sd_netlink sd_netlink;
typedef struct Hashmap Hashmap;
typedef struct Set Set;

/* XXX: move to manager.c when slot.c no longer needs it */
typedef struct NLManager{
        sd_netlink *rtnl;
        sd_event *event;

        bool enumerating_links:1;
        bool enumerating_addresses:1;
        bool enumerating_routes:1;

        LIST_HEAD(NLSlot, link_subscriptions);
        LIST_HEAD(NLSlot, address_subscriptions);
        LIST_HEAD(NLSlot, route_subscriptions);
        Hashmap *links;
        Set *addresses;
        Set *routes;
} NLManager;

typedef void (*nl_reply_handler_t)(int error, void *userdata);

int nl_manager_new(NLManager **ret, sd_event *event);
void nl_manager_free(NLManager *m);

int nl_manager_start(NLManager *m);

int nl_manager_subscribe_links(NLManager *m, NLSlot **slot, nl_link_handler_t callback, void *userdata);
int nl_manager_subscribe_addresses(NLManager *m, NLSlot **slot, nl_address_handler_t callback, void *userdata);
int nl_manager_subscribe_routes(NLManager *m, NLSlot **slot, nl_route_handler_t callback, void *userdata);

int nl_manager_create_address(NLManager *m, NLAddress *address, NLSlot **slotp, nl_reply_handler_t callback, void *userdata);
int nl_manager_create_route(NLManager *m, NLRoute *route, NLSlot **slotp, nl_reply_handler_t callback, void *userdata);

int nl_manager_destroy_address(NLManager *m, NLAddress *address);
int nl_manager_destroy_route(NLManager *m, NLRoute *route);

DEFINE_TRIVIAL_CLEANUP_FUNC(NLManager*, nl_manager_free);
