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

#include <net/ethernet.h>
#include <net/if_arp.h>

#include "sd-dhcp-client.h"
#include "sd-dhcp6-client.h"
#include "sd-event.h"
#include "sd-ipv4ll.h"
#include "sd-ndisc.h"

#include "alloc-util.h"
#include "ip/ip.h"
#include "ip/manager.h"
#include "rtnl/rtnl.h"
#include "string-util.h"

DEFINE_TRIVIAL_CLEANUP_FUNC(IPManager*, ip_manager_free);

static void rtnl_handler(RTNLLink *link, void *userdata);
static int ip_manager_set_ifindex(IPManager *manager, unsigned int ifindex);

int ip_manager_new(IPManager **managerp, RTNLLink *link, sd_event *event) {
        _cleanup_(ip_manager_freep) IPManager *manager = NULL;
        RTNLLinkData *data;
        int r;

        manager = new0(IPManager, 1);
        if (!manager)
                return -ENOMEM;

        manager->link = link;

        r = rtnl_link_subscribe(link, &manager->link_slot, rtnl_handler, manager);
        if (r < 0)
                return r;

        r = sd_ipv4ll_new(&manager->ipv4ll);
        if (r < 0)
                return r;

        r = sd_ipv4ll_attach_event(manager->ipv4ll, event, SD_EVENT_PRIORITY_NORMAL);
        if (r < 0)
                return r;

        r = sd_ipv4ll_set_callback(manager->ipv4ll, ipv4ll_handler, manager);
        if (r < 0)
                return r;

        r = sd_dhcp_client_new(&manager->dhcp4_client);
        if (r < 0)
                return r;

        r = sd_dhcp_client_attach_event(manager->dhcp4_client, event, SD_EVENT_PRIORITY_NORMAL);
        if (r < 0)
                return r;
/*
        r = sd_dhcp_client_set_callback(manager->dhcp4_client, dhcp4_client_handler, manager);
        if (r < 0)
                return r;
*/
        r = sd_dhcp6_client_new(&manager->dhcp6_client);
        if (r < 0)
                return r;

        r = sd_dhcp6_client_attach_event(manager->dhcp6_client, event, SD_EVENT_PRIORITY_NORMAL);
        if (r < 0)
                return r;
/*
        r = sd_dhcp6_client_set_callback(manager->dhcp6_client, dhcp6_client_handler, manager);
        if (r < 0)
                return r;
*/
        r = sd_ndisc_new(&manager->ndisc);
        if (r < 0)
                return r;

        r = sd_ndisc_attach_event(manager->ndisc, event, SD_EVENT_PRIORITY_NORMAL);
        if (r < 0)
                return r;
/*
        r = sd_ndisc_set_callback(manager->ndisc, ndisc_handler, manager);
        if (r < 0)
                return r;
*/
        r = rtnl_link_get_data(link, &data);
        if (r < 0)
                return r;

        ip_manager_set_ifindex(manager, data->ifindex);

        *managerp = manager;
        manager = NULL;

        return 0;
}

void ip_manager_free(IPManager *manager) {
        sd_ipv4ll_unref(manager->ipv4ll);
        sd_dhcp_client_unref(manager->dhcp4_client);
        sd_dhcp6_client_unref(manager->dhcp6_client);
        sd_ndisc_unref(manager->ndisc);
        rtnl_slot_free(manager->link_slot);
        free(manager->ifname);
        free(manager);
}

static int ip_manager_set_ifindex(IPManager *manager, unsigned int ifindex) {
        int r;

        if (ifindex == 0)
                return -EINVAL;

        r = sd_ipv4ll_set_ifindex(manager->ipv4ll, ifindex);
        if (r < 0)
                return r;

        r = sd_dhcp_client_set_ifindex(manager->dhcp4_client, ifindex);
        if (r < 0)
                return r;

        r = sd_dhcp6_client_set_ifindex(manager->dhcp6_client, ifindex);
        if (r < 0)
                return r;

        r = sd_ndisc_set_ifindex(manager->ndisc, ifindex);
        if (r < 0)
                return r;

        manager->ifindex = ifindex;

        return 0;
}

static int ip_manager_set_ifname(IPManager *manager, const char *ifname) {
        if (!ifname)
                return -EINVAL;

        return free_and_strdup(&manager->ifname, ifname);
}

static int ip_manager_set_mac(IPManager *manager, const uint8_t *addr, size_t addr_len, uint16_t arp_type) {
        int r;

        /* if we want to support non-ethernet, we need to handle IPv4LL and NDisc properly */
        assert(arp_type == ARPHRD_ETHER);

        r = sd_ipv4ll_set_mac(manager->ipv4ll, (struct ether_addr*) addr);
        if (r < 0)
                return r;

        r = sd_ndisc_set_mac(manager->ndisc, (struct ether_addr*) addr);
        if (r < 0)
                return r;

        r = sd_dhcp_client_set_mac(manager->dhcp4_client, addr, addr_len, arp_type);
        if (r < 0)
                return r;

        r = sd_dhcp6_client_set_mac(manager->dhcp6_client, addr, addr_len, arp_type);
        if (r < 0)
                return r;

        return 0;
}

int ip_manager_set_unique_predictable_data(IPManager *manager, uint64_t data) {
        int r;

        r = sd_ipv4ll_set_address_seed(manager->ipv4ll, data);
        if (r < 0)
                return r;

        return 0;
}

int ip_manager_start(IPManager *manager) {
        RTNLLinkData *data;
        int state;
        int r;

        /* XXX: schedule an idle rtnl event instead? */

        r = rtnl_link_get_data(manager->link, &data);
        if (r < 0)
                return r;

        ip_manager_set_ifname(manager, data->ifname);
        ip_manager_set_mac(manager, (const uint8_t*)&data->address, ETHER_ADDR_LEN, ARPHRD_ETHER);

        r = rtnl_link_get_state(manager->link, &state);
        if (r < 0)
                return r;

        if (state & RTNL_LINK_STATE_CARRIER) {
                r = sd_ipv4ll_start(manager->ipv4ll);
                if (r < 0)
                        return r;

                r = sd_dhcp_client_start(manager->dhcp4_client);
                if (r < 0)
                        return r;

                if (state & RTNL_LINK_STATE_IPV6LL) {
                        r = sd_ndisc_start(manager->ndisc);
                        if (r < 0)
                                return r;
                }
        }

        manager->state = state;

        return 0;
}

int ip_manager_stop(IPManager *manager) {
        int r;

        /* XXX: don't stop things that are not running */

        r = sd_ipv4ll_stop(manager->ipv4ll);
        if (r < 0)
                return r;

        r = sd_dhcp_client_stop(manager->dhcp4_client);
        if (r < 0)
                return r;

        r = sd_dhcp6_client_stop(manager->dhcp6_client);
        if (r < 0)
                return r;

        r = sd_ndisc_stop(manager->ndisc);
        if (r < 0)
                return r;

        return 0;
}

static void rtnl_handler(RTNLLink *link, void *userdata) {
        IPManager *manager = userdata;
        int r, state;

        if (link) {
                r = rtnl_link_get_state(link, &state);
                if (r < 0)
                        return;
        } else {
                manager->link = NULL;
                rtnl_slot_free(manager->link_slot);
                manager->link_slot = NULL;

                state = 0;
        }

        if ((state & RTNL_LINK_STATE_CARRIER) && !(manager->state & RTNL_LINK_STATE_CARRIER)) {
                /* gained carrier */
                r = sd_ipv4ll_start(manager->ipv4ll);
                if (r < 0)
                        return;

                r = sd_dhcp_client_start(manager->dhcp4_client);
                if (r < 0)
                        return;
        }

        if (!(state & RTNL_LINK_STATE_CARRIER) && (manager->state & RTNL_LINK_STATE_CARRIER)) {
                /* lost carrier */
                r = sd_ipv4ll_stop(manager->ipv4ll);
                if (r < 0)
                        return;

                r = sd_dhcp_client_stop(manager->dhcp4_client);
                if (r < 0)
                        return;
        }

        if ((state & RTNL_LINK_STATE_IPV6LL) && (state & RTNL_LINK_STATE_CARRIER) &&
            !((manager->state & RTNL_LINK_STATE_IPV6LL) && (manager->state & RTNL_LINK_STATE_CARRIER))) {
                /* gained ipv6ll+carrier */
                r = sd_ndisc_start(manager->ndisc);
                if (r < 0)
                        return;
        }

        if (!((state & RTNL_LINK_STATE_IPV6LL) && (state & RTNL_LINK_STATE_CARRIER)) &&
            (manager->state & RTNL_LINK_STATE_IPV6LL) && (manager->state & RTNL_LINK_STATE_CARRIER)) {
                /* lost ipv6ll+carrier */
                r = sd_ndisc_stop(manager->ndisc);
                if (r < 0)
                        return;
        }

        manager->state = state;

        return;
}
