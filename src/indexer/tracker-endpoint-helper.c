/*
 * Copyright (C) 2025, Red Hat Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config-miners.h"

#include <locale.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <glib/gi18n.h>
#include <tinysparql.h>

#include <tracker-common.h>

static char *location = NULL;
static int socket_fd = 0;

static GOptionEntry entries[] = {
	{ "location", 0, 0,
	  G_OPTION_ARG_FILENAME, &location,
	  N_("Database location"),
	  N_("DIR") },
	{ "socket-fd", 's', 0,
	  G_OPTION_ARG_INT, &socket_fd,
	  N_("Socket file descriptor for peer-to-peer communication"),
	  N_("FD") },
	{ NULL }
};

static gboolean
on_term_signal (gpointer user_data)
{
	GMainLoop *main_loop = user_data;

	g_main_loop_quit (main_loop);

	return G_SOURCE_REMOVE;
}

int
main (int   argc,
      char *argv[])
{
	g_autoptr (TrackerSparqlConnection) sparql_conn = NULL;
	g_autoptr (TrackerEndpointDBus) endpoint = NULL;
	g_autoptr (GFile) ontology = NULL, store = NULL;
	g_autoptr (GError) error = NULL;
	g_autoptr (GMainLoop) main_loop = NULL;
	g_autoptr (GOptionContext) context = NULL;
	g_autoptr (GSocket) socket = NULL;
	g_autoptr (GIOStream) stream = NULL;
	g_autoptr (GDBusConnection) dbus_conn = NULL;
	TrackerSparqlConnectionFlags flags =
		TRACKER_SPARQL_CONNECTION_FLAGS_FTS_ENABLE_STEMMER |
		TRACKER_SPARQL_CONNECTION_FLAGS_FTS_ENABLE_UNACCENT;

	setlocale (LC_ALL, "");
	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	context = g_option_context_new (_("— start the tracker index proxy"));
	g_option_context_add_main_entries (context, entries, NULL);
	g_option_context_parse (context, &argc, &argv, &error);

	if (error) {
		g_warning ("Error parsing options: %s", error->message);
		return EXIT_FAILURE;
	}

	if (!location) {
		g_warning ("No database location");
		return EXIT_FAILURE;
	}

#ifdef HAVE_LANDLOCK
	if (!tracker_landlock_init ("localsearch-endpoint-3", NULL,
	                            (const char *[]) { location, "/var/tmp", g_get_tmp_dir (), NULL }))
		return EXIT_FAILURE;
#endif

#ifndef HAVE_LIBSECCOMP
	if (!tracker_seccomp_init (FALSE))
		return EXIT_FAILURE;
#endif

	if (socket_fd == 0) {
		g_warning ("The --socket-fd argument is mandatory");
		return EXIT_FAILURE;
	}

	socket = g_socket_new_from_fd (socket_fd, &error);
	if (!socket) {
		g_warning ("Error creating D-Bus socket: %s", error->message);
		return EXIT_FAILURE;
	}

	stream = G_IO_STREAM (g_socket_connection_factory_create_connection (socket));
	dbus_conn = g_dbus_connection_new_sync (stream,
	                                        NULL,
	                                        G_DBUS_CONNECTION_FLAGS_DELAY_MESSAGE_PROCESSING |
	                                        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
	                                        NULL, NULL, &error);
	if (!dbus_conn) {
		g_warning ("Error creating D-Bus connection: %s", error->message);
		return EXIT_FAILURE;
	}

	store = g_file_new_for_commandline_arg (location);
	ontology = tracker_sparql_get_ontology_nepomuk ();

	sparql_conn = tracker_sparql_connection_new (flags,
	                                             store,
	                                             ontology,
	                                             NULL,
	                                             &error);
	if (!sparql_conn) {
		g_warning ("Could not open database: %s", error->message);
		return EXIT_FAILURE;
	}

	endpoint = tracker_endpoint_dbus_new (sparql_conn,
	                                      dbus_conn,
	                                      NULL,
	                                      NULL,
	                                      &error);
	if (!endpoint) {
		g_warning ("Failed to create D-Bus endpoint: %s", error->message);
		return EXIT_FAILURE;
	}

	main_loop = g_main_loop_new (NULL, FALSE);
	g_unix_signal_add (SIGTERM, on_term_signal, main_loop);
	g_unix_signal_add (SIGINT, on_term_signal, main_loop);
	g_dbus_connection_start_message_processing (dbus_conn);

	g_main_loop_run (main_loop);

	tracker_sparql_connection_close (sparql_conn);
	g_dbus_connection_close_sync (dbus_conn, NULL, NULL);

	return EXIT_SUCCESS;
}
