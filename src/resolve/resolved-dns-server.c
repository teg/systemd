/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2014 Lennart Poettering

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

#include "siphash24.h"

#include "resolved-dns-server.h"

int dns_server_new(
                Manager *m,
                DnsServer **ret,
                DnsServerType type,
                Link *l,
                int family,
                const union in_addr_union *in_addr) {

        DnsServer *s, *tail;

        assert(m);
        assert((type == DNS_SERVER_LINK) == !!l);
        assert(in_addr);

        s = new0(DnsServer, 1);
        if (!s)
                return -ENOMEM;

        s->n_ref = 1;
        s->verified_features = _DNS_SERVER_FEATURE_LEVEL_INVALID;
        s->possible_features = DNS_SERVER_FEATURE_LEVEL_BEST;
        s->type = type;
        s->family = family;
        s->address = *in_addr;

        if (type == DNS_SERVER_LINK) {
                LIST_FIND_TAIL(servers, l->dns_servers, tail);
                LIST_INSERT_AFTER(servers, l->dns_servers, tail, s);
                s->link = l;
        } else if (type == DNS_SERVER_SYSTEM) {
                LIST_FIND_TAIL(servers, m->dns_servers, tail);
                LIST_INSERT_AFTER(servers, m->dns_servers, tail, s);
        } else if (type == DNS_SERVER_FALLBACK) {
                LIST_FIND_TAIL(servers, m->fallback_dns_servers, tail);
                LIST_INSERT_AFTER(servers, m->fallback_dns_servers, tail, s);
        } else
                assert_not_reached("Unknown server type");

        s->manager = m;

        /* A new DNS server that isn't fallback is added and the one
         * we used so far was a fallback one? Then let's try to pick
         * the new one */
        if (type != DNS_SERVER_FALLBACK &&
            m->current_dns_server &&
            m->current_dns_server->type == DNS_SERVER_FALLBACK)
                manager_set_dns_server(m, NULL);

        if (ret)
                *ret = s;

        return 0;
}

DnsServer* dns_server_ref(DnsServer *s)  {
        if (!s)
                return NULL;

        assert(s->n_ref > 0);

        s->n_ref ++;

        return s;
}

static DnsServer* dns_server_free(DnsServer *s)  {
        if (!s)
                return NULL;

        if (s->link && s->link->current_dns_server == s)
                link_set_dns_server(s->link, NULL);

        if (s->manager && s->manager->current_dns_server == s)
                manager_set_dns_server(s->manager, NULL);

        free(s);

        return NULL;
}

DnsServer* dns_server_unref(DnsServer *s)  {
        if (!s)
                return NULL;

        assert(s->n_ref > 0);

        if (s->n_ref == 1)
                dns_server_free(s);
        else
                s->n_ref --;

        return NULL;
}

DnsServerFeatureLevel dns_server_possible_features(DnsServer *s) {
        assert(s);

        if (s->last_failed_attempt != 0 &&
            s->possible_features != DNS_SERVER_FEATURE_LEVEL_BEST &&
            s->last_failed_attempt + DNS_SERVER_FEATURE_RETRY_USEC < now(CLOCK_MONOTONIC)) {
                _cleanup_free_ char *ip = NULL;

                s->possible_features = DNS_SERVER_FEATURE_LEVEL_BEST;
                s->n_failed_attempts = 0;

                in_addr_to_string(s->family, &s->address, &ip);
                log_info("Grace period over, resuming full feature set for DNS server %s", strna(ip));
        } else if (s->possible_features <= s->verified_features)
                s->possible_features = s->verified_features;
        else if (s->n_failed_attempts >= DNS_SERVER_FEATURE_RETRY_ATTEMPTS &&
                 s->possible_features > DNS_SERVER_FEATURE_LEVEL_WORST) {
                _cleanup_free_ char *ip = NULL;

                s->possible_features --;
                s->n_failed_attempts = 0;

                in_addr_to_string(s->family, &s->address, &ip);
                log_warning("Using degraded feature set (%s) for DNS server %s",
                            dns_server_feature_level_to_string(s->possible_features), strna(ip));
        }

        return s->possible_features;
}

static unsigned long dns_server_hash_func(const void *p, const uint8_t hash_key[HASH_KEY_SIZE]) {
        const DnsServer *s = p;
        uint64_t u;

        siphash24((uint8_t*) &u, &s->address, FAMILY_ADDRESS_SIZE(s->family), hash_key);
        u = u * hash_key[0] + u + s->family;

        return u;
}

static int dns_server_compare_func(const void *a, const void *b) {
        const DnsServer *x = a, *y = b;

        if (x->family < y->family)
                return -1;
        if (x->family > y->family)
                return 1;

        return memcmp(&x->address, &y->address, FAMILY_ADDRESS_SIZE(x->family));
}

const struct hash_ops dns_server_hash_ops = {
        .hash = dns_server_hash_func,
        .compare = dns_server_compare_func
};

static const char* const dns_server_feature_level_table[_DNS_SERVER_FEATURE_LEVEL_MAX] = {
        [DNS_SERVER_FEATURE_LEVEL_TCP] = "TCP",
        [DNS_SERVER_FEATURE_LEVEL_UDP] = "UDP",
        [DNS_SERVER_FEATURE_LEVEL_EDNS0] = "UDP+EDNS0",
};
DEFINE_STRING_TABLE_LOOKUP(dns_server_feature_level, DnsServerFeatureLevel);
