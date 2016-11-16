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

#include <sd-event.h>

#include "macro.h"

#include "netlink/manager.h"

int main(void) {
        _cleanup_(nl_manager_freep) NLManager *manager = NULL;
        sd_event *event;
        int r;

        r = sd_event_default(&event);
        assert(r >= 0);

        r = nl_manager_new(&manager, event);
        assert(r >= 0);

        r = nl_manager_start(manager);
        assert(r >= 0);

        r = sd_event_loop(event);
        assert(r >= 0);

        sd_event_unref(event);
}
