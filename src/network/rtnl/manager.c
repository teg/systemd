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

#include <sys/socket.h>

#include "sd-daemon.h"
#include "sd-event.h"
#include "sd-netlink.h"

#include "alloc-util.h"
#include "fd-util.h"
#include "hashmap.h"
#include "list.h"
#include "netlink-util.h"
#include "set.h"

#include "rtnl/address.h"
#include "rtnl/link.h"
#include "rtnl/manager.h"
#include "rtnl/route.h"
#include "rtnl/rtnl.h"
#include "rtnl/slot.h"

/* use 16 MB for receive socket kernel queue. */
#define RCVBUF_SIZE    (16*1024*1024)

int rtnl_manager_new(RTNLManager **ret, sd_event *event) {
        _cleanup_(rtnl_manager_freep) RTNLManager *m = NULL;

        m = new0(RTNLManager, 1);
        if (!m)
                return -ENOMEM;

        m->event = sd_event_ref(event);

        m->links = hashmap_new(NULL);
        if (!m->links)
                return -ENOMEM;

        m->addresses = set_new(&rtnl_address_hash_ops);
        if (!m->addresses)
                return -ENOMEM;

        m->routes = set_new(&rtnl_route_hash_ops);
        if (!m->routes)
                return -ENOMEM;

        *ret = m;
        m = NULL;

        return 0;
}

void rtnl_manager_free(RTNLManager *m) {
        RTNLRoute *route;
        RTNLAddress *address;
        RTNLLink *link;

        if (!m)
                return;

        while ((route = set_steal_first(m->routes)))
                rtnl_route_free(route);
        set_free(m->routes);

        while ((address = set_steal_first(m->addresses)))
                rtnl_address_free(address);
        set_free(m->addresses);

        while ((link = hashmap_steal_first(m->links)))
                rtnl_link_free(link);
        hashmap_free(m->links);

        sd_netlink_unref(m->rtnl);
        sd_event_unref(m->event);

        free(m);
}

static int add_link(sd_netlink *rtnl, sd_netlink_message *message, void *userdata) {
        RTNLManager *m = userdata;
        RTNLLink *link;
        int r, ifindex;

        if (m->enumerating_links)
                return 0;

        r = sd_rtnl_message_link_get_ifindex(message, &ifindex);
        if (r < 0)
                return r;
        else if (ifindex == 0)
                return -EIO;

        link = hashmap_get(m->links, INT_TO_PTR(ifindex));
        if (link) {
                _cleanup_(rtnl_link_data_unrefp) RTNLLinkData *data = NULL;

                r = rtnl_link_data_new_from_message(&data, message);
                if (r < 0)
                        return r;

                r = rtnl_link_update_data(link, data);
                if (r < 0)
                        return r;
        } else {
                _cleanup_(rtnl_link_freep) RTNLLink *new_link = NULL;

                r = rtnl_link_new_from_message(&new_link, message);
                if (r < 0)
                        return r;

                r = rtnl_link_attach(m, new_link);
                if (r < 0)
                        return r;

                new_link = NULL;
        }

        return 1;
}

static int remove_link(sd_netlink *rtnl, sd_netlink_message *message, void *userdata) {
        RTNLManager *m = userdata;
        _cleanup_(rtnl_link_freep) RTNLLink *link = NULL;
        int r, ifindex;

        if (m->enumerating_links)
                return 0;

        r = sd_rtnl_message_link_get_ifindex(message, &ifindex);
        if (r < 0)
                return r;
        else if (ifindex == 0)
                return -EIO;

        link = hashmap_get(m->links, INT_TO_PTR(ifindex));
        if (!link)
                return -ENODEV;

        rtnl_link_detach(link);

        return 1;
}

static int add_address(sd_netlink *rtnl, sd_netlink_message *message, void *userdata) {
        RTNLManager *m = userdata;
        _cleanup_(rtnl_address_freep) RTNLAddress *new_address = NULL;
        RTNLAddress *address = NULL;
        int r;

        if (m->enumerating_addresses)
                return 0;

        r = rtnl_address_new_from_message(&new_address, message);
        if (r < 0)
                return r;

        address = set_get(m->addresses, new_address);
        if (address) {
                RTNLAddressData *data;

                r = rtnl_address_get_data(new_address, &data);
                if (r < 0)
                        return r;

                r = rtnl_address_update_data(address, data);
                if (r < 0)
                        return r;
        } else {
                r = rtnl_address_attach(m, new_address);
                if (r < 0)
                        return r;

                new_address = NULL;
        }

        return 1;
}

static int remove_address(sd_netlink *rtnl, sd_netlink_message *message, void *userdata) {
        RTNLManager *m = userdata;
        _cleanup_(rtnl_address_freep) RTNLAddress *new_address = NULL, *old_address = NULL;
        int r;

        if (m->enumerating_addresses)
                return 0;

        r = rtnl_address_new_from_message(&new_address, message);
        if (r < 0)
                return r;

        old_address = set_get(m->addresses, new_address);
        if (!old_address)
                return -ENODEV;

        rtnl_address_detach(old_address);

        return 1;
}

static int add_route(sd_netlink *rtnl, sd_netlink_message *message, void *userdata) {
        RTNLManager *m = userdata;
        _cleanup_(rtnl_route_freep) RTNLRoute *new_route = NULL;
        RTNLRoute *route = NULL;
        int r;

        if (m->enumerating_routes)
                return 0;

        r = rtnl_route_new_from_message(&new_route, message);
        if (r < 0)
                return r;

        route = set_get(m->routes, new_route);
        if (route) {
                RTNLRouteData *data;

                r = rtnl_route_get_data(new_route, &data);
                if (r < 0)
                        return r;

                r = rtnl_route_update_data(route, data);
                if (r < 0)
                        return r;
        } else {
                r = rtnl_route_attach(m, new_route);
                if (r < 0)
                        return r;

                new_route = NULL;
        }

        return 1;
}

static int remove_route(sd_netlink *rtnl, sd_netlink_message *message, void *userdata) {
        RTNLManager *m = userdata;
        _cleanup_(rtnl_route_freep) RTNLRoute *new_route = NULL, *old_route = NULL;
        int r;

        if (m->enumerating_routes)
                return 0;

        r = rtnl_route_new_from_message(&new_route, message);
        if (r < 0)
                return r;

        old_route = set_get(m->routes, new_route);
        if (!old_route)
                return -ENODEV;

        rtnl_route_detach(old_route);

        return 1;
}

static int enumerate_routes_handler(sd_netlink *rtnl, sd_netlink_message *reply, void *userdata) {
        RTNLManager *m = userdata;
        sd_netlink_message *route;
        int r = 0;

        m->enumerating_routes = false;

        for (route = reply; route; route = sd_netlink_message_next(route)) {
                int k;

                k = add_route(m->rtnl, route, m);
                if (k < 0)
                        r = k;
        }

        return r;
}

static int enumerate_routes(RTNLManager *m) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL, *reply = NULL;
        int r;

        r = sd_rtnl_message_new_route(m->rtnl, &req, RTM_GETROUTE, 0, 0);
        if (r < 0)
                return r;

        r = sd_netlink_message_request_dump(req, true);
        if (r < 0)
                return r;

        r = sd_netlink_call_async(m->rtnl, req, enumerate_routes_handler, m, 0, NULL);
        if (r < 0)
                return r;

        m->enumerating_routes = true;

        return 0;
}

static int enumerate_addresses_handler(sd_netlink *rtnl, sd_netlink_message *reply, void *userdata) {
        RTNLManager *m = userdata;
        sd_netlink_message *address;
        int r = 0;

        m->enumerating_addresses = false;

        for (address = reply; address; address = sd_netlink_message_next(address)) {
                int k;

                k = add_address(m->rtnl, address, m);
                if (k < 0)
                        r = k;
        }

        r = enumerate_routes(m);
        if (r < 0)
                return r;

        return r;
}

static int enumerate_addresses(RTNLManager *m) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL, *reply = NULL;
        int r;

        r = sd_rtnl_message_new_addr(m->rtnl, &req, RTM_GETADDR, 0, 0);
        if (r < 0)
                return r;

        r = sd_netlink_message_request_dump(req, true);
        if (r < 0)
                return r;

        r = sd_netlink_call_async(m->rtnl, req, enumerate_addresses_handler, m, 0, NULL);
        if (r < 0)
                return r;

        m->enumerating_addresses = true;

        return r;
}

static int enumerate_links_handler(sd_netlink *rtnl, sd_netlink_message *reply, void *userdata) {
        RTNLManager *m = userdata;
        sd_netlink_message *link;
        int r = 0;

        m->enumerating_links = false;

        for (link = reply; link; link = sd_netlink_message_next(link)) {
                int k;

                k = add_link(m->rtnl, link, m);
                if (k < 0)
                        r = k;
        }

        r = enumerate_addresses(m);
        if (r < 0)
                return r;

        return r;
}

static int enumerate_links(RTNLManager *m) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL, *reply = NULL;
        int r;

        r = sd_rtnl_message_new_link(m->rtnl, &req, RTM_GETLINK, 0);
        if (r < 0)
                return r;

        r = sd_netlink_message_request_dump(req, true);
        if (r < 0)
                return r;

        r = sd_netlink_call_async(m->rtnl, req, enumerate_links_handler, m, 0, NULL);
        if (r < 0)
                return r;

        m->enumerating_links = true;

        return 0;
}

static int systemd_netlink_fd(void) {
        int n, fd, rtnl_fd = -EINVAL;

        n = sd_listen_fds(true);
        if (n <= 0)
                return -EINVAL;

        for (fd = SD_LISTEN_FDS_START; fd < SD_LISTEN_FDS_START + n; fd ++) {
                if (sd_is_socket(fd, AF_NETLINK, SOCK_RAW, -1) > 0) {
                        if (rtnl_fd >= 0)
                                return -EINVAL;

                        rtnl_fd = fd;
                }
        }

        return rtnl_fd;
}

int rtnl_manager_start(RTNLManager *m) {
        int r, fd;

        fd = systemd_netlink_fd();
        if (fd < 0)
                r = sd_netlink_open(&m->rtnl);
        else
                r = sd_netlink_open_fd(&m->rtnl, fd);
        if (r < 0)
                return r;

        r = sd_netlink_inc_rcvbuf(m->rtnl, RCVBUF_SIZE);
        if (r < 0)
                return r;

        r = sd_netlink_attach_event(m->rtnl, m->event, 0);
        if (r < 0)
                return r;

        r = sd_netlink_add_match(m->rtnl, RTM_NEWLINK, &add_link, m);
        if (r < 0)
                return r;

        r = sd_netlink_add_match(m->rtnl, RTM_DELLINK, &remove_link, m);
        if (r < 0)
                return r;

        r = sd_netlink_add_match(m->rtnl, RTM_NEWADDR, &add_address, m);
        if (r < 0)
                return r;

        r = sd_netlink_add_match(m->rtnl, RTM_DELADDR, &remove_address, m);
        if (r < 0)
                return r;

        r = sd_netlink_add_match(m->rtnl, RTM_NEWROUTE, &add_route, m);
        if (r < 0)
                return r;

        r = sd_netlink_add_match(m->rtnl, RTM_DELROUTE, &remove_route, m);
        if (r < 0)
                return r;

        r = enumerate_links(m);
        if (r < 0)
                return r;

        while (m->enumerating_links || m->enumerating_addresses || m->enumerating_routes) {
                r = sd_netlink_process(m->rtnl, NULL);
                if (r < 0)
                        return r;
        }

        return 0;
}

/* XXX: handle subscribe being called after rtnl_manager_start() */

int rtnl_manager_subscribe_links(RTNLManager *m, RTNLSlot **slotp, rtnl_link_handler_t callback, void *userdata) {
        _cleanup_(rtnl_slot_freep) RTNLSlot *slot = NULL;

        slot = new0(RTNLSlot, 1);
        if (!slot)
                return -ENOMEM;

        slot->manager = m;
        slot->callback.link = callback;
        slot->userdata = userdata;

        LIST_APPEND(slots, m->link_subscriptions, slot);

        *slotp = slot;
        slot = NULL;

        return 0;
}

int rtnl_manager_subscribe_addresses(RTNLManager *m, RTNLSlot **slotp, rtnl_address_handler_t callback, void *userdata) {
        _cleanup_(rtnl_slot_freep) RTNLSlot *slot = NULL;

        slot = new0(RTNLSlot, 1);
        if (!slot)
                return -ENOMEM;

        slot->manager = m;
        slot->callback.address = callback;
        slot->userdata = userdata;

        LIST_APPEND(slots, m->address_subscriptions, slot);

        *slotp = slot;
        slot = NULL;

        return 0;
}

int rtnl_manager_subscribe_routes(RTNLManager *m, RTNLSlot **slotp, rtnl_route_handler_t callback, void *userdata) {
        _cleanup_(rtnl_slot_freep) RTNLSlot *slot = NULL;

        slot = new0(RTNLSlot, 1);
        if (!slot)
                return -ENOMEM;

        slot->callback.route = callback;
        slot->userdata = userdata;

        LIST_APPEND(slots, m->route_subscriptions, slot);
        slot->manager = m;

        *slotp = slot;
        slot = NULL;

        return 0;
}

static int get_link_handler(sd_netlink *rtnl, sd_netlink_message *message, void *userdata) {
        RTNLSlot *slot = userdata;
        RTNLLink *link;

        if (!sd_netlink_message_is_error(message))
                link = hashmap_get(slot->manager->links, INT_TO_PTR(slot->ifindex));
        else
                link = NULL;

        slot->callback.link(link, slot->userdata);

        return 1;
}

int rtnl_manager_get_link(RTNLManager *m, RTNLLinkData *data, RTNLSlot **slotp, rtnl_link_handler_t callback, void *userdata) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *message = NULL;
        _cleanup_(rtnl_slot_freep) RTNLSlot *slot = NULL;
        int r;

        r = rtnl_slot_new(&slot, userdata);
        if (r < 0)
                return r;

        slot->callback.link = callback;
        slot->manager = m;
        slot->ifindex = data->ifindex;

        r = sd_rtnl_message_new_link(m->rtnl, &message, RTM_GETLINK, data->ifindex);
        if (r < 0)
                return r;

        r = sd_netlink_call_async(m->rtnl, message, get_link_handler, slot, 0, &slot->serial);
        if (r < 0)
                return r;

        slot->rtnl = sd_netlink_ref(m->rtnl);

        *slotp = slot;
        slot = NULL;
        return 0;
}

static int create_address_handler(sd_netlink *rtnl, sd_netlink_message *message, void *userdata) {
        RTNLSlot *slot = userdata;
        RTNLAddress *address;

        if (sd_netlink_message_get_errno(message) != 0)
                address = NULL;
        else
                address = set_get(slot->manager->addresses, slot->address);

        slot->callback.address(address, slot->userdata);

        return 1;
}

int rtnl_manager_create_address(RTNLManager *m, RTNLAddressData *data, RTNLSlot **slotp, rtnl_address_handler_t callback, void *userdata) {
        _cleanup_(rtnl_address_freep) RTNLAddress *address = NULL;
        _cleanup_(rtnl_slot_freep) RTNLSlot *slot = NULL;
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *message = NULL;
        int r;

        r = rtnl_address_new_from_data(&address, data);
        if (r < 0)
                return r;

        r = rtnl_slot_new(&slot, userdata);
        if (r < 0)
                return r;

        slot->callback.address = callback;
        slot->manager = m;

        r = sd_rtnl_message_new_addr(m->rtnl, &message, RTM_NEWADDR, data->ifindex, data->family);
        if (r < 0)
                return r;

        r = sd_rtnl_message_addr_set_prefixlen(message, data->prefixlen);
        if (r < 0)
                return r;

        r = sd_rtnl_message_addr_set_scope(message, data->scope);
        if (r < 0)
                return r;

        r = sd_rtnl_message_addr_set_flags(message, data->flags & 0xff);
        if (r < 0)
                return r;

        if (data->flags & ~0xff) {
                r = sd_netlink_message_append_u32(message, IFA_FLAGS, data->flags);
                if (r < 0)
                        return r;
        }

        if (data->label) {
                r = sd_netlink_message_append_string(message, IFA_LABEL, data->label);
                if (r < 0)
                        return r;
        }

        r = sd_netlink_message_append_cache_info(message, IFA_CACHEINFO, &data->cinfo);
        if (r < 0)
                return r;

        switch (data->family) {
        case AF_INET:
                r = sd_netlink_message_append_in_addr(message, IFA_LOCAL, &data->in_addr.in);
                if (r < 0)
                        return r;

                if (!in_addr_is_null(AF_INET, &data->in_addr_peer)) {
                        r = sd_netlink_message_append_in_addr(message, IFA_ADDRESS, &data->in_addr_peer.in);
                        if (r < 0)
                                return r;
                } else if (!in_addr_is_null(AF_INET, (union in_addr_union*)&data->broadcast)) {
                        r = sd_netlink_message_append_in_addr(message, IFA_BROADCAST, &data->broadcast);
                        if (r < 0)
                                return r;

                }

                break;
        case AF_INET6:
                r = sd_netlink_message_append_in6_addr(message, IFA_LOCAL, &data->in_addr.in6);
                if (r < 0)
                        return r;

                if (!in_addr_is_null(AF_INET6, &data->in_addr_peer)) {
                        r = sd_netlink_message_append_in6_addr(message, IFA_ADDRESS, &data->in_addr_peer.in6);
                        if (r < 0)
                                return r;
                }

                break;
        default:
                break;
        }

        r = sd_netlink_call_async(m->rtnl, message, create_address_handler, slot, 0, &slot->serial);
        if (r < 0)
                return r;

        slot->rtnl = sd_netlink_ref(m->rtnl);

        slot->address = address;
        address = NULL;
        *slotp = slot;
        slot = NULL;
        return 0;
}

static int create_route_handler(sd_netlink *rtnl, sd_netlink_message *message, void *userdata) {
        RTNLSlot *slot = userdata;
        RTNLRoute *route;

        if (sd_netlink_message_get_errno(message) != 0)
                route = NULL;
        else
                route = set_get(slot->manager->routes, slot->route);

        slot->callback.route(route, slot->userdata);

        return 1;
}

int rtnl_manager_create_route(RTNLManager *m, RTNLRouteData *data, RTNLSlot **slotp, rtnl_route_handler_t callback, void *userdata) {
        _cleanup_(rtnl_route_freep) RTNLRoute *route = NULL;
        _cleanup_(rtnl_slot_freep) RTNLSlot *slot = NULL;
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *message = NULL;
        int r;

        r = rtnl_route_new_from_data(&route, data);
        if (r < 0)
                return r;

        r = rtnl_slot_new(&slot, userdata);
        if (r < 0)
                return r;

        slot->callback.route = callback;
        slot->manager = m;

        r = sd_rtnl_message_new_route(m->rtnl, &message, RTM_NEWROUTE, data->family, data->protocol);
        if (r < 0)
                return r;

        r = sd_rtnl_message_route_set_dst_prefixlen(message, data->dst_prefixlen);
        if (r < 0)
                return 0;

        r = sd_rtnl_message_route_set_src_prefixlen(message, data->src_prefixlen);
        if (r < 0)
                return 0;

        r = sd_rtnl_message_route_set_scope(message, data->scope);
        if (r < 0)
                return 0;

        r = sd_rtnl_message_route_set_flags(message, data->flags);
        if (r < 0)
                return 0;

        switch (data->family) {
        case AF_INET:
                if (!in_addr_is_null(data->family, &data->gw)) {
                        r = sd_netlink_message_append_in_addr(message, RTA_GATEWAY, &data->gw.in);
                        if (r < 0)
                                return r;
                }

                if (!in_addr_is_null(data->family, &data->prefsrc)) {
                        r = sd_netlink_message_append_in_addr(message, RTA_PREFSRC, &data->prefsrc.in);
                        if (r < 0)
                                return r;
                }

                if (data->dst_prefixlen) {
                        r = sd_netlink_message_append_in_addr(message, RTA_DST, &data->dst.in);
                        if (r < 0)
                                return r;
                }

                if (data->src_prefixlen) {
                        r = sd_netlink_message_append_in_addr(message, RTA_SRC, &data->src.in);
                        if (r < 0)
                                return r;
                }

                break;
        case AF_INET6:
                if (!in_addr_is_null(data->family, &data->gw)) {
                        r = sd_netlink_message_append_in_addr(message, RTA_GATEWAY, &data->gw.in);
                        if (r < 0)
                                return r;
                }

                if (!in_addr_is_null(data->family, &data->prefsrc)) {
                        r = sd_netlink_message_append_in6_addr(message, RTA_PREFSRC, &data->prefsrc.in6);
                        if (r < 0)
                                return r;
                }

                if (data->dst_prefixlen) {
                        r = sd_netlink_message_append_in6_addr(message, RTA_DST, &data->dst.in6);
                        if (r < 0)
                                return r;
                }

                if (data->src_prefixlen) {
                        r = sd_netlink_message_append_in6_addr(message, RTA_SRC, &data->src.in6);
                        if (r < 0)
                                return r;
                }
                break;
        default:
                break;
        }

        if (data->table <= 0xff) {
                r = sd_rtnl_message_route_set_table(message, data->table);
                if (r < 0)
                        return r;
        } else {
                r = sd_rtnl_message_route_set_table(message, RT_TABLE_UNSPEC);
                if (r < 0)
                        return r;

                r = sd_netlink_message_append_data(message, RTA_TABLE, &data->table, sizeof(data->table));
                if (r < 0)
                        return r;
        }

        r = sd_netlink_message_append_u32(message, RTA_PRIORITY, data->priority);
        if (r < 0)
                return r;

        r = sd_netlink_message_append_u8(message, RTA_PREF, data->pref);
        if (r < 0)
                return r;

        r = sd_netlink_message_append_u32(message, RTA_OIF, data->oif);
        if (r < 0)
                return r;

        r = sd_netlink_call_async(m->rtnl, message, create_route_handler, slot, 0, &slot->serial);
        route = NULL;
        if (r < 0)
                return r;

        slot->rtnl = sd_netlink_ref(m->rtnl);

        slot->route = route;
        route = NULL;
        *slotp = slot;
        slot = NULL;
        return 0;
}

int rtnl_manager_destroy_address(RTNLManager *m, RTNLAddress *address) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *message = NULL;
        int r;

        r = sd_rtnl_message_new_addr(m->rtnl, &message, RTM_DELADDR, address->ifindex, address->family);
        if (r < 0)
                return r;

        switch (address->family) {
        case AF_INET:
                r = sd_rtnl_message_addr_set_prefixlen(message, address->prefixlen);
                if (r < 0)
                        return r;

                if (!in_addr_is_null(AF_INET, &address->in_addr_peer)) {
                        r = sd_netlink_message_append_in_addr(message, IFA_ADDRESS, &address->in_addr_peer.in);
                        if (r < 0)
                                return r;
                }

                r = sd_netlink_message_append_in_addr(message, IFA_LOCAL, &address->in_addr.in);
                if (r < 0)
                        return r;
                break;

        case AF_INET6:
                r = sd_netlink_message_append_in6_addr(message, IFA_LOCAL, &address->in_addr.in6);
                if (r < 0)
                        return r;
                break;

        default:
                break;
        }

        r = sd_netlink_call_async(m->rtnl, message, NULL, NULL, 0, NULL);
        if (r < 0)
                return r;

        return 0;
}

int rtnl_manager_destroy_route(RTNLManager *m, RTNLRoute *route) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *message = NULL;
        int r;

        r = sd_rtnl_message_new_route(m->rtnl, &message, RTM_NEWROUTE, route->family, 0);
        if (r < 0)
                return r;

        if (route->table <= 0xff) {
                r = sd_rtnl_message_route_set_table(message, route->table);
                if (r < 0)
                        return r;
        } else {
                r = sd_rtnl_message_route_set_table(message, RT_TABLE_UNSPEC);
                if (r < 0)
                        return r;

                r = sd_netlink_message_append_data(message, RTA_TABLE, &route->table, sizeof(route->table));
                if (r < 0)
                        return r;
        }

        r = sd_netlink_message_append_u32(message, RTA_PRIORITY, route->priority);
        if (r < 0)
                return r;

        r = sd_rtnl_message_route_set_dst_prefixlen(message, route->dst_prefixlen);
        if (r < 0)
                return r;

        switch (route->family) {
        case AF_INET:
                r = sd_netlink_message_append_in_addr(message, RTA_DST, &route->dst.in);
                if (r < 0)
                        return r;

                r = sd_rtnl_message_route_set_tos(message, route->tos);
                if (r < 0)
                        return r;

                break;
        case AF_INET6:
                r = sd_netlink_message_append_in6_addr(message, RTA_DST, &route->dst.in6);
                if (r < 0)
                        return r;

                r = sd_netlink_message_append_u32(message, RTA_OIF, route->oif);
                if (r < 0)
                        return r;

                break;
        default:
                break;
        }

        r = sd_netlink_call_async(m->rtnl, message, NULL, NULL, 0, NULL);
        if (r < 0)
                return r;

        return 0;
}
