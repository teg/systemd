/***
  This file is part of systemd.

  Copyright 2013 Tom Gundersen <teg@jklm.no>

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

#include <arpa/inet.h>
#include <net/if.h>
#include <sd-event.h>

#include "alloc-util.h"
#include "in-addr-util.h"

#include "ip/ip.h"
#include "rtnl/rtnl.h"

typedef struct Manager {
        RTNLManager *manager;
        RTNLSlot *slot;
        sd_event *event;
} Manager;

typedef struct Link {
        IPManager *ip;
} Link;

static void link_free(Link *link);

static int link_new(Link **linkp, RTNLLink *rtnl_link, sd_event *event) {
        Link *link;
        int r;

        link = new0(Link, 1);
        if (!link)
                return -ENOMEM;

        r = ip_manager_new(&link->ip, rtnl_link, event);
        if (r < 0) {
                link_free(link);
                return r;
        }

        *linkp = link;
        return 0;
}

static void link_free(Link *link) {
        if (!link)
                return;

        ip_manager_stop(link->ip);
        ip_manager_free(link->ip);

        free(link);
}

static void add_link_handler(RTNLLink *rtnl_link, void *userdata) {
        Manager *manager = userdata;
        RTNLLinkData *data;
        Link *link;
        int r;

        r = rtnl_link_get_data(rtnl_link, &data);
        assert(r >= 0);

        log_info("new %s '%s': 0x%x", data->kind ?: "link", data->ifname, data->flags);

        if (data->flags & IFF_LOOPBACK) {
                log_info("  ignoring loopback device");
                return;
        }

        r = link_new(&link, rtnl_link, manager->event);
        assert(r >= 0);

        r = ip_manager_start(link->ip);
        assert(r >= 0);
}

int main(void) {
        Manager context = {};
        _cleanup_(rtnl_manager_freep) RTNLManager *manager = NULL;
        RTNLSlot *link_subscription;
        sd_event *event;
        int r;

        r = sd_event_default(&event);
        assert(r >= 0);

        r = rtnl_manager_new(&manager, event);
        assert(r >= 0);

        context.manager = manager;
        context.event = event;

        r = rtnl_manager_subscribe_links(manager, &link_subscription, add_link_handler, &context);
        assert(r >= 0);

        r = rtnl_manager_start(manager);
        assert(r >= 0);

        r = sd_event_loop(event);
        assert(r >= 0);

        rtnl_slot_free(link_subscription);

        sd_event_unref(event);
}
