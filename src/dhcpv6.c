/*
 *
 *  Connection Manager
 *
 *  Copyright (C) 2011  Intel Corporation. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#include <connman/ipconfig.h>
#include <connman/storage.h>

#include <gdhcp/gdhcp.h>

#include <glib.h>

#include "connman.h"

/* Transmission params in msec, RFC 3315 chapter 5.5 */
#define INF_MAX_DELAY	(1 * 1000)
#define INF_TIMEOUT	(1 * 1000)
#define INF_MAX_RT	(120 * 1000)
#define SOL_MAX_DELAY   (1 * 1000)
#define SOL_TIMEOUT     (1 * 1000)
#define SOL_MAX_RT      (120 * 1000)


struct connman_dhcpv6 {
	struct connman_network *network;
	dhcp_cb callback;

	char **nameservers;
	char **timeservers;

	GDHCPClient *dhcp_client;

	guint timeout;		/* operation timeout in msec */
	guint RT;		/* in msec */
	gboolean use_ta;	/* set to TRUE if IPv6 privacy is enabled */
	GSList *prefixes;	/* network prefixes from radvd */
};

static GHashTable *network_table;

static inline float get_random()
{
	return (rand() % 200 - 100) / 1000.0;
}

/* Calculate a random delay, RFC 3315 chapter 14 */
/* RT and MRT are milliseconds */
static guint calc_delay(guint RT, guint MRT)
{
	float delay = get_random();
	float rt = RT * (2 + delay);

	if (rt > MRT)
		rt = MRT * (1 + delay);

	if (rt < 0)
		rt = MRT;

	return (guint)rt;
}

static void free_prefix(gpointer data, gpointer user_data)
{
	g_free(data);
}

static void dhcpv6_free(struct connman_dhcpv6 *dhcp)
{
	g_strfreev(dhcp->nameservers);
	g_strfreev(dhcp->timeservers);

	dhcp->nameservers = NULL;
	dhcp->timeservers = NULL;

	g_slist_foreach(dhcp->prefixes, free_prefix, NULL);
	g_slist_free(dhcp->prefixes);
}

static gboolean compare_string_arrays(char **array_a, char **array_b)
{
	int i;

	if (array_a == NULL || array_b == NULL)
		return FALSE;

	if (g_strv_length(array_a) != g_strv_length(array_b))
		return FALSE;

	for (i = 0; array_a[i] != NULL && array_b[i] != NULL; i++)
		if (g_strcmp0(array_a[i], array_b[i]) != 0)
			return FALSE;

	return TRUE;
}

static void dhcpv6_debug(const char *str, void *data)
{
	connman_info("%s: %s\n", (const char *) data, str);
}

static gchar *convert_to_hex(unsigned char *buf, int len)
{
	gchar *ret = g_try_malloc(len * 2 + 1);
	int i;

	for (i = 0; ret != NULL && i < len; i++)
		g_snprintf(ret + i * 2, 3, "%02x", buf[i]);

	return ret;
}

/*
 * DUID should not change over time so save it to file.
 * See RFC 3315 chapter 9 for details.
 */
static int set_duid(struct connman_service *service,
			struct connman_network *network,
			GDHCPClient *dhcp_client, int index)
{
	GKeyFile *keyfile;
	const char *ident;
	char *hex_duid;
	unsigned char *duid;
	int duid_len;

	ident = __connman_service_get_ident(service);

	keyfile = connman_storage_load_service(ident);
	if (keyfile == NULL)
		return -EINVAL;

	hex_duid = g_key_file_get_string(keyfile, ident, "IPv6.DHCP.DUID",
					NULL);
	if (hex_duid != NULL) {
		unsigned int i, j = 0, hex;
		size_t hex_duid_len = strlen(hex_duid);

		duid = g_try_malloc0(hex_duid_len / 2);
		if (duid == NULL) {
			g_key_file_free(keyfile);
			g_free(hex_duid);
			return -ENOMEM;
		}

		for (i = 0; i < hex_duid_len; i += 2) {
			sscanf(hex_duid + i, "%02x", &hex);
			duid[j++] = hex;
		}

		duid_len = hex_duid_len / 2;
	} else {
		int ret;
		int type = __connman_ipconfig_get_type_from_index(index);

		ret = g_dhcpv6_create_duid(G_DHCPV6_DUID_LLT, index, type,
					&duid, &duid_len);
		if (ret < 0) {
			g_key_file_free(keyfile);
			return ret;
		}

		hex_duid = convert_to_hex(duid, duid_len);
		if (hex_duid == NULL) {
			g_key_file_free(keyfile);
			return -ENOMEM;
		}

		g_key_file_set_string(keyfile, ident, "IPv6.DHCP.DUID",
				hex_duid);

		__connman_storage_save_service(keyfile, ident);
	}
	g_free(hex_duid);

	g_key_file_free(keyfile);

	g_dhcpv6_client_set_duid(dhcp_client, duid, duid_len);

	return 0;
}

static void clear_callbacks(GDHCPClient *dhcp_client)
{
	g_dhcp_client_register_event(dhcp_client,
				G_DHCP_CLIENT_EVENT_SOLICITATION,
				NULL, NULL);

	g_dhcp_client_register_event(dhcp_client,
				G_DHCP_CLIENT_EVENT_ADVERTISE,
				NULL, NULL);

	g_dhcp_client_register_event(dhcp_client,
				G_DHCP_CLIENT_EVENT_INFORMATION_REQ,
				NULL, NULL);
}

static void info_req_cb(GDHCPClient *dhcp_client, gpointer user_data)
{
	struct connman_dhcpv6 *dhcp = user_data;
	struct connman_service *service;
	int entries, i;
	GList *option, *list;
	char **nameservers, **timeservers;

	DBG("dhcpv6 information-request %p", dhcp);

	service = __connman_service_lookup_from_network(dhcp->network);
	if (service == NULL) {
		connman_error("Can not lookup service");
		return;
	}

	option = g_dhcp_client_get_option(dhcp_client, G_DHCPV6_DNS_SERVERS);
	entries = g_list_length(option);

	nameservers = g_try_new0(char *, entries + 1);
	if (nameservers != NULL) {
		for (i = 0, list = option; list; list = list->next, i++)
			nameservers[i] = g_strdup(list->data);
	}

	if (compare_string_arrays(nameservers, dhcp->nameservers) == FALSE) {
		if (dhcp->nameservers != NULL) {
			for (i = 0; dhcp->nameservers[i] != NULL; i++)
				__connman_service_nameserver_remove(service,
							dhcp->nameservers[i],
							FALSE);
			g_strfreev(dhcp->nameservers);
		}

		dhcp->nameservers = nameservers;

		for (i = 0; dhcp->nameservers[i] != NULL; i++)
			__connman_service_nameserver_append(service,
						dhcp->nameservers[i],
						FALSE);
	} else
		g_strfreev(nameservers);


	option = g_dhcp_client_get_option(dhcp_client, G_DHCPV6_SNTP_SERVERS);
	entries = g_list_length(option);

	timeservers = g_try_new0(char *, entries + 1);
	if (timeservers != NULL) {
		for (i = 0, list = option; list; list = list->next, i++)
			timeservers[i] = g_strdup(list->data);
	}

	if (compare_string_arrays(timeservers, dhcp->timeservers) == FALSE) {
		if (dhcp->timeservers != NULL) {
			for (i = 0; dhcp->timeservers[i] != NULL; i++)
				__connman_service_timeserver_remove(service,
							dhcp->timeservers[i]);
			g_strfreev(dhcp->timeservers);
		}

		dhcp->timeservers = timeservers;

		for (i = 0; dhcp->timeservers[i] != NULL; i++)
			__connman_service_timeserver_append(service,
							dhcp->timeservers[i]);
	} else
		g_strfreev(timeservers);


	if (dhcp->callback != NULL) {
		uint16_t status = g_dhcpv6_client_get_status(dhcp_client);
		dhcp->callback(dhcp->network, status == 0 ? TRUE : FALSE);
	}
}

static int dhcpv6_info_request(struct connman_dhcpv6 *dhcp)
{
	struct connman_service *service;
	GDHCPClient *dhcp_client;
	GDHCPClientError error;
	int index, ret;

	DBG("dhcp %p", dhcp);

	index = connman_network_get_index(dhcp->network);

	dhcp_client = g_dhcp_client_new(G_DHCP_IPV6, index, &error);
	if (error != G_DHCP_CLIENT_ERROR_NONE)
		return -EINVAL;

	if (getenv("CONNMAN_DHCPV6_DEBUG"))
		g_dhcp_client_set_debug(dhcp_client, dhcpv6_debug, "DHCPv6");

	service = __connman_service_lookup_from_network(dhcp->network);
	if (service == NULL)
		return -EINVAL;

	ret = set_duid(service, dhcp->network, dhcp_client, index);
	if (ret < 0)
		return ret;

	g_dhcp_client_set_request(dhcp_client, G_DHCPV6_CLIENTID);
	g_dhcp_client_set_request(dhcp_client, G_DHCPV6_DNS_SERVERS);
	g_dhcp_client_set_request(dhcp_client, G_DHCPV6_SNTP_SERVERS);

	g_dhcpv6_client_set_oro(dhcp_client, 2, G_DHCPV6_DNS_SERVERS,
				G_DHCPV6_SNTP_SERVERS);

	g_dhcp_client_register_event(dhcp_client,
			G_DHCP_CLIENT_EVENT_INFORMATION_REQ, info_req_cb, dhcp);

	dhcp->dhcp_client = dhcp_client;

	return g_dhcp_client_start(dhcp_client, NULL);
}

static int check_ipv6_addr_prefix(GSList *prefixes, char *address)
{
	struct in6_addr addr_prefix, addr;
	GSList *list;
	int ret = 128, len;

	for (list = prefixes; list; list = list->next) {
		char *prefix = list->data;
		const char *slash = g_strrstr(prefix, "/");
		const unsigned char bits[] = { 0x00, 0xFE, 0xFC, 0xF8,
						0xF0, 0xE0, 0xC0, 0x80 };
		int left, count, i, plen;

		if (slash == NULL)
			continue;

		prefix = g_strndup(prefix, slash - prefix);
		len = strtol(slash + 1, NULL, 10);
		plen = 128 - len;

		count = plen / 8;
		left = plen % 8;
		i = 16 - count;

		inet_pton(AF_INET6, prefix, &addr_prefix);
		inet_pton(AF_INET6, address, &addr);

		memset(&addr_prefix.s6_addr[i], 0, count);
		memset(&addr.s6_addr[i], 0, count);

		if (left) {
			addr_prefix.s6_addr[i - 1] &= bits[left];
			addr.s6_addr[i - 1] &= bits[left];
		}

		g_free(prefix);

		if (memcmp(&addr_prefix, &addr, 16) == 0) {
			ret = len;
			break;
		}
	}

	return ret;
}

static int set_addresses(GDHCPClient *dhcp_client,
						struct connman_dhcpv6 *dhcp)
{
	struct connman_service *service;
	struct connman_ipconfig *ipconfig;
	int entries, i;
	GList *option, *list;
	char **nameservers, **timeservers;
	const char *c_address;
	char *address = NULL;

	service = __connman_service_lookup_from_network(dhcp->network);
	if (service == NULL) {
		connman_error("Can not lookup service");
		return -EINVAL;
	}

	ipconfig = __connman_service_get_ip6config(service);
	if (ipconfig == NULL) {
		connman_error("Could not lookup ip6config");
		return -EINVAL;
	}

	option = g_dhcp_client_get_option(dhcp_client, G_DHCPV6_DNS_SERVERS);
	entries = g_list_length(option);

	nameservers = g_try_new0(char *, entries + 1);
	if (nameservers != NULL) {
		for (i = 0, list = option; list; list = list->next, i++)
			nameservers[i] = g_strdup(list->data);
	}

	if (compare_string_arrays(nameservers, dhcp->nameservers) == FALSE) {
		if (dhcp->nameservers != NULL) {
			for (i = 0; dhcp->nameservers[i] != NULL; i++)
				__connman_service_nameserver_remove(service,
							dhcp->nameservers[i],
							FALSE);
			g_strfreev(dhcp->nameservers);
		}

		dhcp->nameservers = nameservers;

		for (i = 0; dhcp->nameservers[i] != NULL; i++)
			__connman_service_nameserver_append(service,
							dhcp->nameservers[i],
							FALSE);
	} else
		g_strfreev(nameservers);


	option = g_dhcp_client_get_option(dhcp_client, G_DHCPV6_SNTP_SERVERS);
	entries = g_list_length(option);

	timeservers = g_try_new0(char *, entries + 1);
	if (timeservers != NULL) {
		for (i = 0, list = option; list; list = list->next, i++)
			timeservers[i] = g_strdup(list->data);
	}

	if (compare_string_arrays(timeservers, dhcp->timeservers) == FALSE) {
		if (dhcp->timeservers != NULL) {
			for (i = 0; dhcp->timeservers[i] != NULL; i++)
				__connman_service_timeserver_remove(service,
							dhcp->timeservers[i]);
			g_strfreev(dhcp->timeservers);
		}

		dhcp->timeservers = timeservers;

		for (i = 0; dhcp->timeservers[i] != NULL; i++)
			__connman_service_timeserver_append(service,
							dhcp->timeservers[i]);
	} else
		g_strfreev(timeservers);


	option = g_dhcp_client_get_option(dhcp_client, G_DHCPV6_IA_NA);
	if (option != NULL)
		address = g_strdup(option->data);
	else {
		option = g_dhcp_client_get_option(dhcp_client, G_DHCPV6_IA_TA);
		if (option != NULL)
			address = g_strdup(option->data);
	}

	c_address = __connman_ipconfig_get_local(ipconfig);

	if (address != NULL &&
			((c_address != NULL &&
				g_strcmp0(address, c_address) != 0) ||
			(c_address == NULL))) {
		int prefix_len;

		/* Is this prefix part of the subnet we are suppose to use? */
		prefix_len = check_ipv6_addr_prefix(dhcp->prefixes, address);

		__connman_ipconfig_set_local(ipconfig, address);
		__connman_ipconfig_set_prefixlen(ipconfig, prefix_len);

		DBG("new address %s/%d", address, prefix_len);
	}

	return 0;
}

static int dhcpv6_release(struct connman_dhcpv6 *dhcp)
{
	DBG("dhcp %p", dhcp);

	if (dhcp->timeout > 0) {
		g_source_remove(dhcp->timeout);
		dhcp->timeout = 0;
	}

	dhcpv6_free(dhcp);

	if (dhcp->dhcp_client == NULL)
		return 0;

	g_dhcp_client_stop(dhcp->dhcp_client);
	g_dhcp_client_unref(dhcp->dhcp_client);

	dhcp->dhcp_client = NULL;

	return 0;
}

static void remove_network(gpointer user_data)
{
	struct connman_dhcpv6 *dhcp = user_data;

	DBG("dhcp %p", dhcp);

	dhcpv6_release(dhcp);

	g_free(dhcp);
}

static gboolean timeout_info_req(gpointer user_data)
{
	struct connman_dhcpv6 *dhcp = user_data;

	dhcp->RT = calc_delay(dhcp->RT, INF_MAX_RT);

	DBG("info RT timeout %d msec", dhcp->RT);

	dhcp->timeout = g_timeout_add(dhcp->RT, timeout_info_req, dhcp);

	g_dhcp_client_start(dhcp->dhcp_client, NULL);

	return FALSE;
}

static gboolean start_info_req(gpointer user_data)
{
	struct connman_dhcpv6 *dhcp = user_data;

	/* Set the retransmission timeout, RFC 3315 chapter 14 */
	dhcp->RT = INF_TIMEOUT * (1 + get_random());

	DBG("info initial RT timeout %d msec", dhcp->RT);

	dhcp->timeout = g_timeout_add(dhcp->RT, timeout_info_req, dhcp);

	dhcpv6_info_request(dhcp);

	return FALSE;
}

int __connman_dhcpv6_start_info(struct connman_network *network,
				dhcp_cb callback)
{
	struct connman_dhcpv6 *dhcp;
	int delay;

	DBG("");

	dhcp = g_try_new0(struct connman_dhcpv6, 1);
	if (dhcp == NULL)
		return -ENOMEM;

	dhcp->network = network;
	dhcp->callback = callback;

	connman_network_ref(network);

	g_hash_table_replace(network_table, network, dhcp);

	/* Initial timeout, RFC 3315, 18.1.5 */
	delay = rand() % 1000;

	dhcp->timeout = g_timeout_add(delay, start_info_req, dhcp);

	return 0;
}

static void advertise_cb(GDHCPClient *dhcp_client, gpointer user_data)
{
	struct connman_dhcpv6 *dhcp = user_data;

	DBG("dhcpv6 advertise msg %p", dhcp);
}

static void solicitation_cb(GDHCPClient *dhcp_client, gpointer user_data)
{
	/* We get here if server supports rapid commit */
	struct connman_dhcpv6 *dhcp = user_data;

	DBG("dhcpv6 solicitation msg %p", dhcp);

	if (dhcp->timeout > 0) {
		g_source_remove(dhcp->timeout);
		dhcp->timeout = 0;
	}

	set_addresses(dhcp_client, dhcp);
}

static gboolean timeout_solicitation(gpointer user_data)
{
	struct connman_dhcpv6 *dhcp = user_data;

	dhcp->RT = calc_delay(dhcp->RT, SOL_MAX_RT);

	DBG("solicit RT timeout %d msec", dhcp->RT);

	dhcp->timeout = g_timeout_add(dhcp->RT, timeout_solicitation, dhcp);

	g_dhcp_client_start(dhcp->dhcp_client, NULL);

	return FALSE;
}

static int dhcpv6_solicitation(struct connman_dhcpv6 *dhcp)
{
	struct connman_service *service;
	struct connman_ipconfig *ipconfig_ipv6;
	GDHCPClient *dhcp_client;
	GDHCPClientError error;
	int index, ret;

	DBG("dhcp %p", dhcp);

	index = connman_network_get_index(dhcp->network);

	dhcp_client = g_dhcp_client_new(G_DHCP_IPV6, index, &error);
	if (error != G_DHCP_CLIENT_ERROR_NONE)
		return -EINVAL;

	if (getenv("CONNMAN_DHCPV6_DEBUG"))
		g_dhcp_client_set_debug(dhcp_client, dhcpv6_debug, "DHCPv6");

	service = __connman_service_lookup_from_network(dhcp->network);
	if (service == NULL)
		return -EINVAL;

	ret = set_duid(service, dhcp->network, dhcp_client, index);
	if (ret < 0)
		return ret;

	g_dhcp_client_set_request(dhcp_client, G_DHCPV6_CLIENTID);
	g_dhcp_client_set_request(dhcp_client, G_DHCPV6_RAPID_COMMIT);
	g_dhcp_client_set_request(dhcp_client, G_DHCPV6_DNS_SERVERS);
	g_dhcp_client_set_request(dhcp_client, G_DHCPV6_SNTP_SERVERS);

	g_dhcpv6_client_set_oro(dhcp_client, 2, G_DHCPV6_DNS_SERVERS,
				G_DHCPV6_SNTP_SERVERS);

	ipconfig_ipv6 = __connman_service_get_ip6config(service);
	dhcp->use_ta = __connman_ipconfig_ipv6_privacy_enabled(ipconfig_ipv6);

	g_dhcpv6_client_set_ia(dhcp_client, index,
			dhcp->use_ta == TRUE ? G_DHCPV6_IA_TA : G_DHCPV6_IA_NA,
			NULL, NULL, FALSE);

	clear_callbacks(dhcp_client);

	g_dhcp_client_register_event(dhcp_client,
				G_DHCP_CLIENT_EVENT_SOLICITATION,
				solicitation_cb, dhcp);

	g_dhcp_client_register_event(dhcp_client,
				G_DHCP_CLIENT_EVENT_ADVERTISE,
				advertise_cb, dhcp);

	dhcp->dhcp_client = dhcp_client;

	return g_dhcp_client_start(dhcp_client, NULL);
}

static gboolean start_solicitation(gpointer user_data)
{
	struct connman_dhcpv6 *dhcp = user_data;

	/* Set the retransmission timeout, RFC 3315 chapter 14 */
	dhcp->RT = SOL_TIMEOUT * (1 + get_random());

	DBG("solicit initial RT timeout %d msec", dhcp->RT);

	dhcp->timeout = g_timeout_add(dhcp->RT, timeout_solicitation, dhcp);

	dhcpv6_solicitation(dhcp);

	return FALSE;
}

int __connman_dhcpv6_start(struct connman_network *network,
				GSList *prefixes, dhcp_cb callback)
{
	struct connman_dhcpv6 *dhcp;
	int delay;

	DBG("");

	dhcp = g_try_new0(struct connman_dhcpv6, 1);
	if (dhcp == NULL)
		return -ENOMEM;

	dhcp->network = network;
	dhcp->callback = callback;
	dhcp->prefixes = prefixes;

	connman_network_ref(network);

	g_hash_table_replace(network_table, network, dhcp);

	/* Initial timeout, RFC 3315, 17.1.2 */
	delay = rand() % 1000;

	dhcp->timeout = g_timeout_add(delay, start_solicitation, dhcp);

	return 0;
}

void __connman_dhcpv6_stop(struct connman_network *network)
{
	DBG("");

	if (network_table == NULL)
		return;

	if (g_hash_table_remove(network_table, network) == TRUE)
		connman_network_unref(network);
}

int __connman_dhcpv6_init(void)
{
	DBG("");

	srand(time(0));

	network_table = g_hash_table_new_full(g_direct_hash, g_direct_equal,
							NULL, remove_network);

	return 0;
}

void __connman_dhcpv6_cleanup(void)
{
	DBG("");

	g_hash_table_destroy(network_table);
	network_table = NULL;
}
