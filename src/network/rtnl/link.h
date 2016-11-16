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

struct RTNLLink {
        RTNLManager *manager;

        /* hash key */
        int ifindex;

        RTNLLinkData *data;
        int state;

        LIST_HEAD(RTNLSlot, subscriptions);
};

int rtnl_link_data_new_from_message(RTNLLinkData **datap, sd_netlink_message *message);

int rtnl_link_new_from_message(RTNLLink **linkp, sd_netlink_message *message);
void rtnl_link_free(RTNLLink *link);

int rtnl_link_attach(RTNLManager *manager, RTNLLink *link);
void rtnl_link_detach(RTNLLink *link);

int rtnl_link_add_address(RTNLLink *link, RTNLAddress *address);
int rtnl_link_update_data(RTNLLink *link, RTNLLinkData *data);

DEFINE_TRIVIAL_CLEANUP_FUNC(RTNLLink*, rtnl_link_free);
