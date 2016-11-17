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

typedef struct sd_netlink_message sd_netlink_message;

typedef struct NLRoute {
        int n_ref;

        int family;
        unsigned flags;
        union in_addr_union gw;
        union in_addr_union prefsrc;
        union in_addr_union dst;
        unsigned char dst_prefixlen;
        union in_addr_union src;
        unsigned char src_prefixlen;
        unsigned char scope;
        unsigned char protocol;
        unsigned char tos;
        unsigned char pref;
        uint32_t priority;
        uint32_t table;
        uint32_t oif;
} NLRoute;

int nl_route_new(NLRoute **routep, sd_netlink_message *message);
NLRoute *nl_route_unref(NLRoute *route);
NLRoute *nl_route_ref(NLRoute *route);

extern const struct hash_ops nl_route_hash_ops;

DEFINE_TRIVIAL_CLEANUP_FUNC(NLRoute*, nl_route_unref);
