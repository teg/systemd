/***
  This file is part of systemd.

  Copyright 2013-2014 Tom Gundersen <teg@jklm.no>

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

#include <netinet/ether.h>
#include <linux/if.h>

#include "sd-ipv4ll.h"

#include "ip/ip.h"
#include "ip/manager.h"
#include "rtnl/rtnl.h"

static int ipv4ll_address_lost(IPManager *manager) {
        int r;

        r = rtnl_link_destroy_address(manager->link, manager->ipv4ll_address);
        if (r < 0)
                return r;

        rtnl_slot_free(manager->ipv4ll_address_slot);
        manager->ipv4ll_address_slot = NULL;

        return 0;
}

static void create_address_handler(RTNLAddress *address, void *userdata) {
//        IPManager *manager = userdata;

        /* XXX */
}

static int ipv4ll_address_claimed(IPManager *manager, const struct in_addr *address) {
        RTNLAddressData data;
        int r;

        log_debug("IPv4 link-local claim %u.%u.%u.%u", ADDRESS_FMT_VAL(*address));

        rtnl_address_data_init(&data);

        data.family = AF_INET;
        data.in_addr.in = *address;
        data.prefixlen = 16;
        data.broadcast.s_addr = data.in_addr.in.s_addr | htobe32(0xfffffffflu >> data.prefixlen);
        data.scope = RT_SCOPE_LINK;

        r = rtnl_link_create_address(manager->link, &data, &manager->ipv4ll_address_slot, create_address_handler, manager);
        if (r < 0)
                return r;

        return 0;
}

void ipv4ll_handler(sd_ipv4ll *ll, int event, void *userdata) {
        IPManager *manager = userdata;
        struct in_addr address;
        int r;

        switch(event) {
                case SD_IPV4LL_EVENT_STOP:
                case SD_IPV4LL_EVENT_CONFLICT:
                        ipv4ll_address_lost(manager);

                        break;
                case SD_IPV4LL_EVENT_BIND:
                        r = sd_ipv4ll_get_address(manager->ipv4ll, &address);
                        if (r < 0)
                                return;

                        ipv4ll_address_claimed(manager, &address);

                        break;
                default:
                        break;
        }
}
