/***
  This file is part of systemd.

  Copyright (C) 2013 Intel Corporation. All rights reserved.
  Copyright (C) 2014 Tom Gundersen

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
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sd-dhcp-lease.h"

#include "alloc-util.h"
#include "dhcp-internal.h"
#include "dhcp-lease-internal.h"
#include "dhcp-protocol.h"
#include "dns-domain.h"
#include "fd-util.h"
#include "fileio.h"
#include "hexdecoct.h"
#include "hostname-util.h"
#include "in-addr-util.h"
#include "network-internal.h"
#include "parse-util.h"
#include "stdio-util.h"
#include "string-util.h"
#include "unaligned.h"

int sd_dhcp_lease_get_address(sd_dhcp_lease *lease, struct in_addr *addr) {
        assert_return(lease, -EINVAL);
        assert_return(addr, -EINVAL);

        if (lease->address == 0)
                return -ENODATA;

        addr->s_addr = lease->address;
        return 0;
}

int sd_dhcp_lease_get_broadcast(sd_dhcp_lease *lease, struct in_addr *addr) {
        assert_return(lease, -EINVAL);
        assert_return(addr, -EINVAL);

        if (!lease->have_broadcast)
                return -ENODATA;

        addr->s_addr = lease->broadcast;
        return 0;
}

int sd_dhcp_lease_get_lifetime(sd_dhcp_lease *lease, uint32_t *lifetime) {
        assert_return(lease, -EINVAL);
        assert_return(lifetime, -EINVAL);

        if (lease->lifetime <= 0)
                return -ENODATA;

        *lifetime = lease->lifetime;
        return 0;
}

int sd_dhcp_lease_get_t1(sd_dhcp_lease *lease, uint32_t *t1) {
        assert_return(lease, -EINVAL);
        assert_return(t1, -EINVAL);

        if (lease->t1 <= 0)
                return -ENODATA;

        *t1 = lease->t1;
        return 0;
}

int sd_dhcp_lease_get_t2(sd_dhcp_lease *lease, uint32_t *t2) {
        assert_return(lease, -EINVAL);
        assert_return(t2, -EINVAL);

        if (lease->t2 <= 0)
                return -ENODATA;

        *t2 = lease->t2;
        return 0;
}

int sd_dhcp_lease_get_mtu(sd_dhcp_lease *lease, uint16_t *mtu) {
        assert_return(lease, -EINVAL);
        assert_return(mtu, -EINVAL);

        if (lease->mtu <= 0)
                return -ENODATA;

        *mtu = lease->mtu;
        return 0;
}

int sd_dhcp_lease_get_dns(sd_dhcp_lease *lease, const struct in_addr **addr) {
        assert_return(lease, -EINVAL);
        assert_return(addr, -EINVAL);

        if (lease->dns_size <= 0)
                return -ENODATA;

        *addr = lease->dns;
        return (int) lease->dns_size;
}

int sd_dhcp_lease_get_ntp(sd_dhcp_lease *lease, const struct in_addr **addr) {
        assert_return(lease, -EINVAL);
        assert_return(addr, -EINVAL);

        if (lease->ntp_size <= 0)
                return -ENODATA;

        *addr = lease->ntp;
        return (int) lease->ntp_size;
}

int sd_dhcp_lease_get_domainname(sd_dhcp_lease *lease, const char **domainname) {
        assert_return(lease, -EINVAL);
        assert_return(domainname, -EINVAL);

        if (!lease->domainname)
                return -ENODATA;

        *domainname = lease->domainname;
        return 0;
}

int sd_dhcp_lease_get_hostname(sd_dhcp_lease *lease, const char **hostname) {
        assert_return(lease, -EINVAL);
        assert_return(hostname, -EINVAL);

        if (!lease->hostname)
                return -ENODATA;

        *hostname = lease->hostname;
        return 0;
}

int sd_dhcp_lease_get_root_path(sd_dhcp_lease *lease, const char **root_path) {
        assert_return(lease, -EINVAL);
        assert_return(root_path, -EINVAL);

        if (!lease->root_path)
                return -ENODATA;

        *root_path = lease->root_path;
        return 0;
}

int sd_dhcp_lease_get_router(sd_dhcp_lease *lease, struct in_addr *addr) {
        assert_return(lease, -EINVAL);
        assert_return(addr, -EINVAL);

        if (lease->router == 0)
                return -ENODATA;

        addr->s_addr = lease->router;
        return 0;
}

int sd_dhcp_lease_get_netmask(sd_dhcp_lease *lease, struct in_addr *addr) {
        assert_return(lease, -EINVAL);
        assert_return(addr, -EINVAL);

        if (!lease->have_subnet_mask)
                return -ENODATA;

        addr->s_addr = lease->subnet_mask;
        return 0;
}

int sd_dhcp_lease_get_server_identifier(sd_dhcp_lease *lease, struct in_addr *addr) {
        assert_return(lease, -EINVAL);
        assert_return(addr, -EINVAL);

        if (lease->server_address == 0)
                return -ENODATA;

        addr->s_addr = lease->server_address;
        return 0;
}

int sd_dhcp_lease_get_next_server(sd_dhcp_lease *lease, struct in_addr *addr) {
        assert_return(lease, -EINVAL);
        assert_return(addr, -EINVAL);

        if (lease->next_server == 0)
                return -ENODATA;

        addr->s_addr = lease->next_server;
        return 0;
}

/*
 * The returned routes array must be freed by the caller.
 * Route objects have the same lifetime of the lease and must not be freed.
 */
int sd_dhcp_lease_get_routes(sd_dhcp_lease *lease, sd_dhcp_route ***routes) {
        sd_dhcp_route **ret;
        unsigned i;

        assert_return(lease, -EINVAL);
        assert_return(routes, -EINVAL);

        if (lease->static_route_size <= 0)
                return -ENODATA;

        ret = new(sd_dhcp_route *, lease->static_route_size);
        if (!ret)
                return -ENOMEM;

        for (i = 0; i < lease->static_route_size; i++)
                ret[i] = &lease->static_route[i];

        *routes = ret;
        return (int) lease->static_route_size;
}

int sd_dhcp_lease_get_vendor_specific(sd_dhcp_lease *lease, const void **data, size_t *data_len) {
        assert_return(lease, -EINVAL);
        assert_return(data, -EINVAL);
        assert_return(data_len, -EINVAL);

        if (lease->vendor_specific_len <= 0)
                return -ENODATA;

        *data = lease->vendor_specific;
        *data_len = lease->vendor_specific_len;
        return 0;
}

sd_dhcp_lease *sd_dhcp_lease_ref(sd_dhcp_lease *lease) {

        if (!lease)
                return NULL;

        assert(lease->n_ref >= 1);
        lease->n_ref++;

        return lease;
}

sd_dhcp_lease *sd_dhcp_lease_unref(sd_dhcp_lease *lease) {

        if (!lease)
                return NULL;

        assert(lease->n_ref >= 1);
        lease->n_ref--;

        if (lease->n_ref > 0)
                return NULL;

        while (lease->private_options) {
                struct sd_dhcp_raw_option *option = lease->private_options;

                LIST_REMOVE(options, lease->private_options, option);

                free(option->data);
                free(option);
        }

        free(lease->hostname);
        free(lease->domainname);
        free(lease->dns);
        free(lease->ntp);
        free(lease->static_route);
        free(lease->client_id);
        free(lease->vendor_specific);
        free(lease->error_message);
        free(lease);

        return NULL;
}

static int lease_parse_u32(const uint8_t *option, size_t len, uint32_t *ret, uint32_t min) {
        assert(option);
        assert(ret);

        if (len != 4)
                return -EINVAL;

        *ret = unaligned_read_be32((be32_t*) option);
        if (*ret < min)
                *ret = min;

        return 0;
}

static int lease_parse_u16(const uint8_t *option, size_t len, uint16_t *ret, uint16_t min) {
        assert(option);
        assert(ret);

        if (len != 2)
                return -EINVAL;

        *ret = unaligned_read_be16((be16_t*) option);
        if (*ret < min)
                *ret = min;

        return 0;
}

static int lease_parse_be32(const uint8_t *option, size_t len, be32_t *ret) {
        assert(option);
        assert(ret);

        if (len != 4)
                return -EINVAL;

        memcpy(ret, option, 4);
        return 0;
}

static int lease_parse_string(const uint8_t *option, size_t len, char **ret) {
        assert(option);
        assert(ret);

        if (len <= 0)
                *ret = mfree(*ret);
        else {
                char *string;

                /*
                 * One trailing NUL byte is OK, we don't mind. See:
                 * https://github.com/systemd/systemd/issues/1337
                 */
                if (memchr(option, 0, len - 1))
                        return -EINVAL;

                string = strndup((const char *) option, len);
                if (!string)
                        return -ENOMEM;

                free(*ret);
                *ret = string;
        }

        return 0;
}

static int lease_parse_domain(const uint8_t *option, size_t len, char **ret) {
        _cleanup_free_ char *name = NULL, *normalized = NULL;
        int r;

        assert(option);
        assert(ret);

        r = lease_parse_string(option, len, &name);
        if (r < 0)
                return r;
        if (!name) {
                *ret = mfree(*ret);
                return 0;
        }

        r = dns_name_normalize(name, &normalized);
        if (r < 0)
                return r;

        if (is_localhost(normalized))
                return -EINVAL;

        if (dns_name_is_root(normalized))
                return -EINVAL;

        free(*ret);
        *ret = normalized;
        normalized = NULL;

        return 0;
}

static int lease_parse_in_addrs(const uint8_t *option, size_t len, struct in_addr **ret, size_t *n_ret) {
        assert(option);
        assert(ret);
        assert(n_ret);

        if (len <= 0) {
                *ret = mfree(*ret);
                *n_ret = 0;
        } else {
                size_t n_addresses;
                struct in_addr *addresses;

                if (len % 4 != 0)
                        return -EINVAL;

                n_addresses = len / 4;

                addresses = newdup(struct in_addr, option, n_addresses);
                if (!addresses)
                        return -ENOMEM;

                free(*ret);
                *ret = addresses;
                *n_ret = n_addresses;
        }

        return 0;
}

static int lease_parse_routes(
                const uint8_t *option, size_t len,
                struct sd_dhcp_route **routes, size_t *routes_size, size_t *routes_allocated) {

        struct in_addr addr;

        assert(option || len <= 0);
        assert(routes);
        assert(routes_size);
        assert(routes_allocated);

        if (len <= 0)
                return 0;

        if (len % 8 != 0)
                return -EINVAL;

        if (!GREEDY_REALLOC(*routes, *routes_allocated, *routes_size + (len / 8)))
                return -ENOMEM;

        while (len >= 8) {
                struct sd_dhcp_route *route = *routes + *routes_size;
                int r;

                r = in_addr_default_prefixlen((struct in_addr*) option, &route->dst_prefixlen);
                if (r < 0) {
                        log_debug("Failed to determine destination prefix length from class based IP, ignoring");
                        continue;
                }

                assert_se(lease_parse_be32(option, 4, &addr.s_addr) >= 0);
                route->dst_addr = inet_makeaddr(inet_netof(addr), 0);
                option += 4;

                assert_se(lease_parse_be32(option, 4, &route->gw_addr.s_addr) >= 0);
                option += 4;

                len -= 8;
                (*routes_size)++;
        }

        return 0;
}

/* parses RFC3442 Classless Static Route Option */
static int lease_parse_classless_routes(
                const uint8_t *option, size_t len,
                struct sd_dhcp_route **routes, size_t *routes_size, size_t *routes_allocated) {

        assert(option || len <= 0);
        assert(routes);
        assert(routes_size);
        assert(routes_allocated);

        if (len <= 0)
                return 0;

        /* option format: (subnet-mask-width significant-subnet-octets gateway-ip)*  */

        while (len > 0) {
                uint8_t dst_octets;
                struct sd_dhcp_route *route;

                if (!GREEDY_REALLOC(*routes, *routes_allocated, *routes_size + 1))
                        return -ENOMEM;

                route = *routes + *routes_size;

                dst_octets = (*option == 0 ? 0 : ((*option - 1) / 8) + 1);
                route->dst_prefixlen = *option;
                option++;
                len--;

                /* can't have more than 4 octets in IPv4 */
                if (dst_octets > 4 || len < dst_octets)
                        return -EINVAL;

                route->dst_addr.s_addr = 0;
                memcpy(&route->dst_addr.s_addr, option, dst_octets);
                option += dst_octets;
                len -= dst_octets;

                if (len < 4)
                        return -EINVAL;

                assert_se(lease_parse_be32(option, 4, &route->gw_addr.s_addr) >= 0);
                option += 4;
                len -= 4;

                (*routes_size)++;
        }

        return 0;
}

static int dhcp_lease_insert_private_option(sd_dhcp_lease *lease, uint8_t tag, const void *data, uint8_t len) {
        struct sd_dhcp_raw_option *cur, *option;

        assert(lease);

        LIST_FOREACH(options, cur, lease->private_options) {
                if (tag < cur->tag)
                        break;
                if (tag == cur->tag) {
                        log_debug("Ignoring duplicate option, tagged %i.", tag);
                        return 0;
                }
        }

        option = new(struct sd_dhcp_raw_option, 1);
        if (!option)
                return -ENOMEM;

        option->tag = tag;
        option->length = len;
        option->data = memdup(data, len);
        if (!option->data) {
                free(option);
                return -ENOMEM;
        }

        LIST_INSERT_BEFORE(options, lease->private_options, cur, option);
        return 0;
}

static int dhcp_lease_parse_options(uint8_t code, uint8_t len, const void *option, void *userdata) {
        sd_dhcp_lease *lease = userdata;
        int r;

        assert(lease);

        switch(code) {

        case SD_DHCP_OPTION_IP_ADDRESS_LEASE_TIME:
                r = lease_parse_u32(option, len, &lease->lifetime, 1);
                if (r < 0)
                        log_debug_errno(r, "Failed to parse lease time, ignoring: %m");

                break;

        case SD_DHCP_OPTION_SERVER_IDENTIFIER:
                r = lease_parse_be32(option, len, &lease->server_address);
                if (r < 0)
                        log_debug_errno(r, "Failed to parse server identifier, ignoring: %m");

                break;

        case SD_DHCP_OPTION_SUBNET_MASK:
                r = lease_parse_be32(option, len, &lease->subnet_mask);
                if (r < 0)
                        log_debug_errno(r, "Failed to parse subnet mask, ignoring: %m");
                else
                        lease->have_subnet_mask = true;
                break;

        case SD_DHCP_OPTION_BROADCAST:
                r = lease_parse_be32(option, len, &lease->broadcast);
                if (r < 0)
                        log_debug_errno(r, "Failed to parse broadcast address, ignoring: %m");
                else
                        lease->have_broadcast = true;
                break;

        case SD_DHCP_OPTION_ROUTER:
                if (len >= 4) {
                        r = lease_parse_be32(option, 4, &lease->router);
                        if (r < 0)
                                log_debug_errno(r, "Failed to parse router address, ignoring: %m");
                }
                break;

        case SD_DHCP_OPTION_DOMAIN_NAME_SERVER:
                r = lease_parse_in_addrs(option, len, &lease->dns, &lease->dns_size);
                if (r < 0)
                        log_debug_errno(r, "Failed to parse DNS server, ignoring: %m");
                break;

        case SD_DHCP_OPTION_NTP_SERVER:
                r = lease_parse_in_addrs(option, len, &lease->ntp, &lease->ntp_size);
                if (r < 0)
                        log_debug_errno(r, "Failed to parse NTP server, ignoring: %m");
                break;

        case SD_DHCP_OPTION_STATIC_ROUTE:
                r = lease_parse_routes(option, len, &lease->static_route, &lease->static_route_size, &lease->static_route_allocated);
                if (r < 0)
                        log_debug_errno(r, "Failed to parse static routes, ignoring: %m");
                break;

        case SD_DHCP_OPTION_INTERFACE_MTU:
                r = lease_parse_u16(option, len, &lease->mtu, 68);
                if (r < 0)
                        log_debug_errno(r, "Failed to parse MTU, ignoring: %m");
                break;

        case SD_DHCP_OPTION_DOMAIN_NAME:
                r = lease_parse_domain(option, len, &lease->domainname);
                if (r < 0) {
                        log_debug_errno(r, "Failed to parse domain name, ignoring: %m");
                        return 0;
                }

                break;

        case SD_DHCP_OPTION_HOST_NAME:
                r = lease_parse_domain(option, len, &lease->hostname);
                if (r < 0) {
                        log_debug_errno(r, "Failed to parse host name, ignoring: %m");
                        return 0;
                }

                break;

        case SD_DHCP_OPTION_ROOT_PATH:
                r = lease_parse_string(option, len, &lease->root_path);
                if (r < 0)
                        log_debug_errno(r, "Failed to parse root path, ignoring: %m");
                break;

        case SD_DHCP_OPTION_RENEWAL_T1_TIME:
                r = lease_parse_u32(option, len, &lease->t1, 1);
                if (r < 0)
                        log_debug_errno(r, "Failed to parse T1 time, ignoring: %m");
                break;

        case SD_DHCP_OPTION_REBINDING_T2_TIME:
                r = lease_parse_u32(option, len, &lease->t2, 1);
                if (r < 0)
                        log_debug_errno(r, "Failed to parse T2 time, ignoring: %m");
                break;

        case SD_DHCP_OPTION_CLASSLESS_STATIC_ROUTE:
                r = lease_parse_classless_routes(
                                option, len,
                                &lease->static_route,
                                &lease->static_route_size,
                                &lease->static_route_allocated);
                if (r < 0)
                        log_debug_errno(r, "Failed to parse classless routes, ignoring: %m");
                break;

        case SD_DHCP_OPTION_NEW_TZDB_TIMEZONE: {
                _cleanup_free_ char *tz = NULL;

                r = lease_parse_string(option, len, &tz);
                if (r < 0) {
                        log_debug_errno(r, "Failed to parse timezone option, ignoring: %m");
                        return 0;
                }

                if (!timezone_is_valid(tz)) {
                        log_debug_errno(r, "Timezone is not valid, ignoring: %m");
                        return 0;
                }

                free(lease->timezone);
                lease->timezone = tz;
                tz = NULL;

                break;
        }

        case SD_DHCP_OPTION_VENDOR_SPECIFIC:

                if (len <= 0)
                        lease->vendor_specific = mfree(lease->vendor_specific);
                else {
                        void *p;

                        p = memdup(option, len);
                        if (!p)
                                return -ENOMEM;

                        free(lease->vendor_specific);
                        lease->vendor_specific = p;
                }

                lease->vendor_specific_len = len;
                break;

        case SD_DHCP_OPTION_PRIVATE_BASE ... SD_DHCP_OPTION_PRIVATE_LAST:
                r = dhcp_lease_insert_private_option(lease, code, option, len);
                if (r < 0)
                        return r;

                break;

        default:
                log_debug("Ignoring option DHCP option %"PRIu8" while parsing.", code);
                break;
        }

        return 0;
}

static int dhcp_lease_set_default_subnet_mask(sd_dhcp_lease *lease) {
        struct in_addr address, mask;
        int r;

        assert(lease);

        if (lease->address == 0)
                return -ENODATA;

        address.s_addr = lease->address;

        /* fall back to the default subnet masks based on address class */
        r = in_addr_default_subnet_mask(&address, &mask);
        if (r < 0)
                return r;

        lease->subnet_mask = mask.s_addr;
        lease->have_subnet_mask = true;

        return 0;
}

int sd_dhcp_lease_from_raw(sd_dhcp_lease **ret, uint64_t timestamp, const void *raw, size_t raw_size) {
        _cleanup_(sd_dhcp_lease_unrefp) sd_dhcp_lease *lease = NULL;
        const DHCPMessage *message = raw;
        int r;

        assert_return(timestamp != 0, -EINVAL);
        assert_return(raw, -EINVAL);
        assert_return(raw_size != 0, -EINVAL);

        lease = malloc0(ALIGN(sizeof(sd_dhcp_lease)) + raw_size);
        if (!lease)
                return -ENOMEM;

        lease->n_ref = 1;
        lease->timestamp = timestamp;
        lease->raw_size = raw_size;
        memcpy(DHCP_LEASE_RAW(lease), raw, raw_size);

        r = dhcp_option_parse(message, raw_size, dhcp_lease_parse_options, lease, &lease->error_message);
        if (r < 0)
                return r;
        else
                lease->type = r;

        lease->next_server = message->siaddr;
        lease->address = message->yiaddr;

        if (lease->address == INADDR_ANY ||
            lease->server_address == INADDR_ANY ||
            lease->lifetime == 0)
                return -ENOMSG;

        if (lease->subnet_mask == INADDR_ANY) {
                r = dhcp_lease_set_default_subnet_mask(lease);
                if (r < 0)
                        return -ENOMSG;
        }

        *ret = lease;
        lease = NULL;

        return r;
}

int sd_dhcp_lease_get_raw(sd_dhcp_lease *lease, uint64_t *timestamp, void **raw, size_t *raw_size) {
        assert_return(lease, -EINVAL);
        assert_return(timestamp, -EINVAL);
        assert_return(raw, -EINVAL);
        assert_return(raw_size, -EINVAL);

        *timestamp = lease->timestamp;
        *raw = DHCP_LEASE_RAW(lease);
        *raw_size = lease->raw_size;

        return 0;
}

int sd_dhcp_lease_get_client_id(sd_dhcp_lease *lease, const void **client_id, size_t *client_id_len) {
        assert_return(lease, -EINVAL);
        assert_return(client_id, -EINVAL);
        assert_return(client_id_len, -EINVAL);

        if (!lease->client_id)
                return -ENODATA;

        *client_id = lease->client_id;
        *client_id_len = lease->client_id_len;

        return 0;
}

int dhcp_lease_set_client_id(sd_dhcp_lease *lease, const void *client_id, size_t client_id_len) {
        assert_return(lease, -EINVAL);
        assert_return(client_id || client_id_len <= 0, -EINVAL);

        if (client_id_len <= 0)
                lease->client_id = mfree(lease->client_id);
        else {
                void *p;

                p = memdup(client_id, client_id_len);
                if (!p)
                        return -ENOMEM;

                free(lease->client_id);
                lease->client_id = p;
                lease->client_id_len = client_id_len;
        }

        return 0;
}

int sd_dhcp_lease_get_timezone(sd_dhcp_lease *lease, const char **tz) {
        assert_return(lease, -EINVAL);
        assert_return(tz, -EINVAL);

        if (!lease->timezone)
                return -ENODATA;

        *tz = lease->timezone;
        return 0;
}

int sd_dhcp_route_get_destination(sd_dhcp_route *route, struct in_addr *destination) {
        assert_return(route, -EINVAL);
        assert_return(destination, -EINVAL);

        *destination = route->dst_addr;
        return 0;
}

int sd_dhcp_route_get_destination_prefix_length(sd_dhcp_route *route, uint8_t *length) {
        assert_return(route, -EINVAL);
        assert_return(length, -EINVAL);

        *length = route->dst_prefixlen;
        return 0;
}

int sd_dhcp_route_get_gateway(sd_dhcp_route *route, struct in_addr *gateway) {
        assert_return(route, -EINVAL);
        assert_return(gateway, -EINVAL);

        *gateway = route->gw_addr;
        return 0;
}
