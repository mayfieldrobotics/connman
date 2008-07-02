/*
 *
 *  Connection Manager
 *
 *  Copyright (C) 2007-2008  Intel Corporation. All rights reserved.
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
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>
#include <sys/stat.h>
#include <net/if.h>

#include <gdbus.h>

#include "connman.h"

static GMainLoop *main_loop = NULL;

static void sig_term(int sig)
{
	g_main_loop_quit(main_loop);
}

static gchar *option_interface = NULL;
static gboolean option_detach = TRUE;
static gboolean option_compat = FALSE;
static gboolean option_debug = FALSE;

static GOptionEntry options[] = {
	{ "interface", 'i', 0, G_OPTION_ARG_STRING, &option_interface,
				"Specify network interface", "IFACE" },
	{ "nodaemon", 'n', G_OPTION_FLAG_REVERSE,
				G_OPTION_ARG_NONE, &option_detach,
				"Don't fork daemon to background" },
	{ "compat", 'c', 0, G_OPTION_ARG_NONE, &option_compat,
				"Enable Network Manager compatibility" },
	{ "debug", 'd', 0, G_OPTION_ARG_NONE, &option_debug,
				"Enable debug information output" },
	{ NULL },
};

int main(int argc, char *argv[])
{
	GOptionContext *context;
	GError *error = NULL;
	DBusConnection *conn;
	DBusError err;
	struct sigaction sa;

	if (g_thread_supported() == FALSE)
		g_thread_init(NULL);

	context = g_option_context_new(NULL);
	g_option_context_add_main_entries(context, options, NULL);

	if (g_option_context_parse(context, &argc, &argv, &error) == FALSE) {
		if (error != NULL) {
			g_printerr("%s\n", error->message);
			g_error_free(error);
		} else
			g_printerr("An unknown error occurred\n");
		exit(1);
	}

	g_option_context_free(context);

	if (option_detach == TRUE) {
		if (daemon(0, 0)) {
			perror("Can't start daemon");
			exit(1);
		}
	}

	mkdir(STATEDIR, S_IRUSR | S_IWUSR | S_IXUSR |
			S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);

	mkdir(STORAGEDIR, S_IRUSR | S_IWUSR | S_IXUSR |
			S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);

	main_loop = g_main_loop_new(NULL, FALSE);

	if (dbus_threads_init_default() == FALSE) {
		fprintf(stderr, "Can't init usage of threads\n");
		exit(1);
	}

	dbus_error_init(&err);

	conn = g_dbus_setup_bus(DBUS_BUS_SYSTEM, CONNMAN_SERVICE, &err);
	if (conn == NULL) {
		if (dbus_error_is_set(&err) == TRUE) {
			fprintf(stderr, "%s\n", err.message);
			dbus_error_free(&err);
		} else
			fprintf(stderr, "Can't register with system bus\n");
		exit(1);
	}

	if (option_compat == TRUE) {
		if (g_dbus_request_name(conn, NM_SERVICE, NULL) == FALSE) {
			fprintf(stderr, "Can't register compat service\n");
			option_compat = FALSE;
		}
	}

	__connman_log_init(option_detach, option_debug);

	__connman_storage_init();

	__connman_element_init(conn);

	__connman_agent_init(conn);

	__connman_manager_init(conn, option_compat);

	__connman_plugin_init();

	g_free(option_interface);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sig_term;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	g_main_loop_run(main_loop);

	__connman_plugin_cleanup();

	__connman_manager_cleanup();

	__connman_agent_cleanup();

	__connman_element_cleanup();

	__connman_storage_cleanup();

	__connman_log_cleanup();

	g_dbus_cleanup_connection(conn);

	g_main_loop_unref(main_loop);

	rmdir(STORAGEDIR);

	rmdir(STATEDIR);

	return 0;
}
