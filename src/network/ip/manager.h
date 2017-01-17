#pragma once

/***
  This file is part of systemd.

  Copyright 2017 Tom Gundersen <teg@jklm.no>

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

#include "sd-ndisc.h"

typedef struct RTNLLink RTNLLink;
typedef struct RTNLAddress RTNLAddress;
typedef struct RTNLRoute RTNLRoute;
typedef struct RTNLSlot RTNLSlot;
typedef struct sd_ipv4ll sd_ipv4ll;
typedef struct sd_dhcp_client sd_dhcp_client;
typedef struct sd_dhcp6_client sd_dhcp6_client;
typedef struct sd_ndisc sd_ndisc;

struct IPManager {
        RTNLLink *link;
        RTNLSlot *link_slot;

        unsigned int ifindex;
        char *ifname;
        int state;

        sd_ipv4ll *ipv4ll;
        sd_dhcp_client *dhcp4_client;
        sd_dhcp6_client *dhcp6_client;
        sd_ndisc *ndisc;

        RTNLRoute *ipv4ll_route;
        RTNLSlot *ipv4ll_route_slot;
        RTNLAddress *ipv4ll_address;
        RTNLSlot *ipv4ll_address_slot;
};

typedef struct sd_ipv4ll sd_ipv4ll;
typedef struct sd_dhcp_client sd_dhcp_client;
typedef struct sd_dhcp6_client sd_dhcp6_client;

void ipv4ll_handler(sd_ipv4ll *ll, int event, void *userdata);
void dhcp4_client_handler(sd_dhcp_client *client, int event, void *userdata);
void dhcp6_client_handler(sd_dhcp6_client *client, int event, void *userdata);
void ndisc_handler(sd_ndisc *nd, sd_ndisc_event event, sd_ndisc_router *rt, void *userdata);

#define ADDRESS_FMT_VAL(address)                   \
        be32toh((address).s_addr) >> 24,           \
        (be32toh((address).s_addr) >> 16) & 0xFFu, \
        (be32toh((address).s_addr) >> 8) & 0xFFu,  \
        be32toh((address).s_addr) & 0xFFu
