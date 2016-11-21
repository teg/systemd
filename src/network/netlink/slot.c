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

#include "netlink/slot.h"

void nl_slot_free(NLSlot *slot) {
        if (slot->manager) {
                if (slot == slot->manager->link_subscriptions)
                        LIST_REMOVE(slots, slot->manager->link_subscriptions, slot);
                else if(slot == slot->manager->link_subscriptions)
                        LIST_REMOVE(slots, slot->manager->address_subscriptions, slot);
                else
                        LIST_REMOVE(slots, slot->manager->route_subscriptions, slot);
        } else if (slot->link)
                        LIST_REMOVE(slots, slot->link->subscriptions, slot);
        else if (slot->address)
                        LIST_REMOVE(slots, slot->address->subscriptions, slot);
        else
                        LIST_REMOVE(slots, slot->route->subscriptions, slot);

        free(slot);
}
