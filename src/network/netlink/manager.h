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

typedef struct NLManager NLManager;
typedef struct sd_event sd_event;

int nl_manager_new(NLManager **ret, sd_event *event);
void nl_manager_free(NLManager *m);

int nl_manager_start(NLManager *m);

DEFINE_TRIVIAL_CLEANUP_FUNC(NLManager*, nl_manager_free);
