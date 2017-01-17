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

typedef struct IPManager IPManager;
typedef struct RTNLLink RTNLLink;
typedef struct sd_event sd_event;

int ip_manager_new(IPManager **managerp, RTNLLink *link, sd_event *event);
void ip_manager_free(IPManager *manager);

int ip_manager_set_unique_predictable_data(IPManager *manager, uint64_t data);

int ip_manager_start(IPManager *manager);
int ip_manager_stop(IPManager *manager);
