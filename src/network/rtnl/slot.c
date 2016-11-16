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

#include "sd-netlink.h"

#include "alloc-util.h"

#include "rtnl/address.h"
#include "rtnl/link.h"
#include "rtnl/manager.h"
#include "rtnl/rtnl.h"
#include "rtnl/route.h"
#include "rtnl/slot.h"

int rtnl_slot_new(RTNLSlot **slotp, void *userdata) {
        _cleanup_(rtnl_slot_freep) RTNLSlot *slot = NULL;

        slot = new0(RTNLSlot, 1);
        if (!slot)
                return -ENOMEM;

        slot->userdata = userdata;

        *slotp = slot;
        slot = NULL;

        return 0;
}

void rtnl_slot_free(RTNLSlot *slot) {
        if (slot->serial) {
                /* this is a method call */
                sd_netlink_call_async_cancel(slot->rtnl, slot->serial);
                sd_netlink_unref(slot->rtnl);
                rtnl_address_free(slot->address);
                rtnl_route_free(slot->route);
        } else if (slot->manager) {
                /* this is a global subscription */
                if (slot == slot->manager->link_subscriptions)
                        LIST_REMOVE(slots, slot->manager->link_subscriptions, slot);
                else if(slot == slot->manager->address_subscriptions)
                        LIST_REMOVE(slots, slot->manager->address_subscriptions, slot);
                else
                        LIST_REMOVE(slots, slot->manager->route_subscriptions, slot);
        } else {
                /* this is an object subscription */
                if (slot->link)
                        LIST_REMOVE(slots, slot->link->subscriptions, slot);
                else if (slot->address)
                        LIST_REMOVE(slots, slot->address->subscriptions, slot);
                else
                        LIST_REMOVE(slots, slot->route->subscriptions, slot);
        }

        free(slot);
}
