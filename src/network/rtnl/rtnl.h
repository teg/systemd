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

#include "in-addr-util.h"

typedef struct RTNLManager RTNLManager;
typedef struct RTNLLink RTNLLink;
typedef struct RTNLAddress RTNLAddress;
typedef struct RTNLRoute RTNLRoute;
typedef struct RTNLSlot RTNLSlot;

typedef struct RTNLLinkData {
        unsigned int n_ref;

        int ifindex;

        char *ifname;
        char *kind;
        unsigned short iftype;
        struct ether_addr address;
        uint32_t mtu;

        unsigned flags;
        uint8_t operstate;
} RTNLLinkData;

typedef struct RTNLAddressData {
        unsigned int n_ref;

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
} RTNLAddressData;

typedef struct RTNLRouteData {
        unsigned int n_ref;

        int family;
        uint32_t table;
        uint32_t priority;
        union in_addr_union dst;
        unsigned char dst_prefixlen;
        unsigned char tos;
        uint32_t oif;

        unsigned flags;
        union in_addr_union gw;
        union in_addr_union prefsrc;
        union in_addr_union src;
        unsigned char src_prefixlen;
        unsigned char scope;
        unsigned char protocol;
        unsigned char pref;
} RTNLRouteData;

/* states */
enum {
        RTNL_LINK_STATE_CARRIER =       1ULL << 0,
        RTNL_LINK_STATE_IPV6LL  =       1ULL << 1,
};

/* callbacks */

typedef void (*rtnl_link_handler_t)(RTNLLink *link, void *userdata);
typedef void (*rtnl_address_handler_t)(RTNLAddress *address, void *userdata);
typedef void (*rtnl_route_handler_t)(RTNLRoute *route, void *userdata);

/* manager */

int rtnl_manager_new(RTNLManager **ret, sd_event *event);
void rtnl_manager_free(RTNLManager *m);

int rtnl_manager_start(RTNLManager *m);

/* links */

int rtnl_manager_get_link(RTNLManager *m, RTNLLinkData *link, RTNLSlot **slot, rtnl_link_handler_t callback, void *userdata);
int rtnl_manager_subscribe_links(RTNLManager *m, RTNLSlot **slot, rtnl_link_handler_t callback, void *userdata);

void rtnl_link_data_init(RTNLLinkData *data);
RTNLLinkData *rtnl_link_data_ref(RTNLLinkData *data);
RTNLLinkData *rtnl_link_data_unref(RTNLLinkData *data);

int rtnl_link_subscribe(RTNLLink *link, RTNLSlot **slotp, rtnl_link_handler_t callback, void *userdata);
int rtnl_link_get_data(RTNLLink *link, RTNLLinkData **datap);
int rtnl_link_get_state(RTNLLink *link, int *statep);

int rtnl_link_create_address(RTNLLink *link, RTNLAddressData *address, RTNLSlot **slotp, rtnl_address_handler_t callback, void *userdata);
int rtnl_link_destroy_address(RTNLLink *link, RTNLAddress *address);
int rtnl_link_create_route(RTNLLink *link, RTNLRouteData *route, RTNLSlot **slotp, rtnl_route_handler_t callback, void *userdata);
int rtnl_link_destroy_route(RTNLLink *link, RTNLRoute *route);

/* addresses */

int rtnl_manager_subscribe_addresses(RTNLManager *m, RTNLSlot **slot, rtnl_address_handler_t callback, void *userdata);
int rtnl_manager_create_address(RTNLManager *m, RTNLAddressData *address, RTNLSlot **slotp, rtnl_address_handler_t callback, void *userdata);
int rtnl_manager_destroy_address(RTNLManager *m, RTNLAddress *address);

void rtnl_address_data_init(RTNLAddressData *data);
RTNLAddressData *rtnl_address_data_ref(RTNLAddressData *data);
RTNLAddressData *rtnl_address_data_unref(RTNLAddressData *data);

int rtnl_address_subscribe(RTNLAddress *address, RTNLSlot **slotp, rtnl_address_handler_t callback, void *userdata);
int rtnl_address_get_data(RTNLAddress *address, RTNLAddressData **datap);

/* routes */

void rtnl_route_data_init(RTNLRouteData *data);
RTNLRouteData *rtnl_route_data_ref(RTNLRouteData *data);
RTNLRouteData *rtnl_route_data_unref(RTNLRouteData *data);

int rtnl_route_subscribe(RTNLRoute *route, RTNLSlot **slotp, rtnl_route_handler_t callback, void *userdata);
int rtnl_route_get_data(RTNLRoute *route, RTNLRouteData **datap);

int rtnl_manager_subscribe_routes(RTNLManager *m, RTNLSlot **slot, rtnl_route_handler_t callback, void *userdata);
int rtnl_manager_create_route(RTNLManager *m, RTNLRouteData *route, RTNLSlot **slotp, rtnl_route_handler_t callback, void *userdata);
int rtnl_manager_destroy_route(RTNLManager *m, RTNLRoute *route);

/* slots */

void rtnl_slot_free(RTNLSlot *slot);

/* convenience */

DEFINE_TRIVIAL_CLEANUP_FUNC(RTNLManager*, rtnl_manager_free);
DEFINE_TRIVIAL_CLEANUP_FUNC(RTNLLinkData*, rtnl_link_data_unref);
DEFINE_TRIVIAL_CLEANUP_FUNC(RTNLAddressData*, rtnl_address_data_unref);
DEFINE_TRIVIAL_CLEANUP_FUNC(RTNLRouteData*, rtnl_route_data_unref);
