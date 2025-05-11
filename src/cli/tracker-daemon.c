/*
 * Copyright (C) 2010, Nokia <ivan.frade@nokia.com>
 * Copyright (C) 2014, Lanedo <martyn@lanedo.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include "config-miners.h"

#include <errno.h>

#include <glib.h>
#include <glib-unix.h>
#include <glib/gi18n.h>
#include <glib/gprintf.h>
#include <locale.h>

#include <tinysparql.h>

#include <tracker-common.h>

#include "tracker-process.h"
#include "tracker-dbus.h"
#include "tracker-indexer-proxy.h"

static GMainLoop *main_loop;

static gboolean follow;
static gboolean watch;

static gboolean start;
static gboolean terminate;

static gboolean indexer_paused;

static int indeterminate_pos = 0;

#define INDETERMINATE_ROOM ((int) strlen ("100.0%") - 1)

static GOptionEntry entries[] = {
	/* Status */
	{ "follow", 'f', 0, G_OPTION_ARG_NONE, &follow,
	  N_("Follow status changes as they happen"),
	  NULL
	},
	{ "watch", 'w', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &watch,
	  N_("Watch changes to the database in real time (e.g. resources or files being added)"),
	  NULL
	},
	/* Processes */
	{ "terminate", 't', 0, G_OPTION_ARG_NONE, &terminate,
	  N_("Stops the indexer"),
	  NULL },
	{ "start", 's', 0, G_OPTION_ARG_NONE, &start,
	  N_("Starts the indexer"),
	  NULL },
	{ NULL }
};

static gboolean
signal_handler (gpointer user_data)
{
	int signo = GPOINTER_TO_INT (user_data);

	static gboolean in_loop = FALSE;

	/* Die if we get re-entrant signals handler calls */
	if (in_loop) {
		exit (EXIT_FAILURE);
	}

	switch (signo) {
	case SIGTERM:
	case SIGINT:
		in_loop = TRUE;
		g_main_loop_quit (main_loop);
		break;
	default:
		break;
	}

	return G_SOURCE_CONTINUE;
}

static void
initialize_signal_handler (void)
{
	g_unix_signal_add (SIGTERM, signal_handler, GINT_TO_POINTER (SIGTERM));
	g_unix_signal_add (SIGINT, signal_handler, GINT_TO_POINTER (SIGINT));
}

static void
notifier_events_cb (TrackerNotifier         *notifier,
		    const gchar             *service,
		    const gchar             *graph,
		    GPtrArray               *events,
		    TrackerSparqlConnection *conn)
{
	gint i;

	for (i = 0; i < events->len; i++) {
		TrackerNotifierEvent *event;

		event = g_ptr_array_index (events, i);
		g_print ("  '%s' => '%s'\n", graph,
			 tracker_notifier_event_get_urn (event));
	}
}

static void
maybe_reset_line (void)
{
	if (tracker_term_is_tty ()) {
		g_print ("\033[2K");
		g_print ("\r");
	}
}

static void
print_indexer_status (TrackerIndexerMiner *indexer_proxy)
{
	g_auto (GStrv) pause_apps = NULL, pause_reasons = NULL;
	g_autofree char *status = NULL;
	double progress;

	if (!tracker_indexer_miner_call_get_status_sync (indexer_proxy,
	                                                 &status,
	                                                 NULL, NULL))
		return;

	if (!tracker_indexer_miner_call_get_progress_sync (indexer_proxy,
	                                                   &progress,
	                                                   NULL, NULL))
		return;

	maybe_reset_line ();

	if (progress > 0.0) {
		g_print ("[%5.1f%%]", progress * 100);
	} else {
		g_print ("[");
		if (indeterminate_pos > 0)
			g_print ("%*c", indeterminate_pos, ' ');
		g_print ("=");
		if (INDETERMINATE_ROOM - indeterminate_pos > 0)
			g_print ("%*c", INDETERMINATE_ROOM - indeterminate_pos, ' ');
		g_print ("]");

		indeterminate_pos++;
		if (indeterminate_pos > INDETERMINATE_ROOM)
			indeterminate_pos = 0;
	}

	g_print (" %s", status);

	if (!tracker_term_is_tty ())
		g_print ("\n");
}

static void
indexer_progress_cb (TrackerIndexerMiner *indexer_proxy,
                     const char          *status,
                     double               progress,
                     int                  remaining_time)
{
	if (!indexer_paused)
		print_indexer_status (indexer_proxy);
}

static void
indexer_paused_cb (TrackerIndexerMiner *indexer_proxy)
{
	maybe_reset_line ();
	g_print ("%s", _("Indexer is paused"));
	indexer_paused = TRUE;
}

static void
indexer_resumed_cb (TrackerIndexerMiner *indexer_proxy)
{
	indexer_paused = FALSE;
}

static int
daemon_status (void)
{
	g_autoptr (TrackerIndexerMiner) indexer_proxy = NULL;

	indexer_proxy =
		tracker_indexer_miner_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
		                                              G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
		                                              "org.freedesktop.LocalSearch3",
		                                              "/org/freedesktop/Tracker3/Miner/Files",
		                                              NULL, NULL);
	if (!indexer_proxy)
		return EXIT_FAILURE;

	print_indexer_status (indexer_proxy);

	if (follow) {
		g_signal_connect (indexer_proxy, "progress",
		                  G_CALLBACK (indexer_progress_cb), NULL);
		g_signal_connect (indexer_proxy, "paused",
		                  G_CALLBACK (indexer_paused_cb), NULL);
		g_signal_connect (indexer_proxy, "resumed",
		                  G_CALLBACK (indexer_resumed_cb), NULL);

		initialize_signal_handler ();

		main_loop = g_main_loop_new (NULL, FALSE);
		g_main_loop_run (main_loop);
		g_main_loop_unref (main_loop);

		if (tracker_term_is_tty ()) {
			/* Print the status line a last time, papering over the ^C */
			print_indexer_status (indexer_proxy);
			g_print ("\n");
		}
	}

	return EXIT_SUCCESS;
}

static gint
daemon_run (void)
{
	g_autoptr (GError) error = NULL;

	if (watch) {
		TrackerSparqlConnection *sparql_connection;
		TrackerNotifier *notifier;
		GError *error = NULL;

		sparql_connection = tracker_sparql_connection_bus_new ("org.freedesktop.Tracker3.Miner.Files",
		                                                       NULL, NULL, &error);

		if (!sparql_connection) {
			g_critical ("%s, %s",
			            _("Could not get SPARQL connection"),
			            error ? error->message : _("No error given"));
			g_clear_error (&error);
			return EXIT_FAILURE;
		}

		notifier = tracker_sparql_connection_create_notifier (sparql_connection);
		g_signal_connect (notifier, "events",
				  G_CALLBACK (notifier_events_cb), sparql_connection);
		g_object_unref (sparql_connection);

		g_print ("%s\n", _("Now listening for resource updates to the database"));
		g_print ("%s\n\n", _("All nie:plainTextContent properties are omitted"));
		g_print ("%s\n", _("Press Ctrl+C to stop"));

		main_loop = g_main_loop_new (NULL, FALSE);
		g_main_loop_run (main_loop);
		g_main_loop_unref (main_loop);
		g_object_unref (notifier);

		/* Carriage return, so we paper over the ^C */
		g_print ("\r");

		return EXIT_SUCCESS;
	}

	if (terminate) {
		return tracker_process_stop (SIGTERM);
	}

	if (start) {
		g_autoptr (TrackerSparqlConnection) sparql_conn = NULL;

		g_print ("%s\n", _("Starting indexer…"));

		sparql_conn = tracker_sparql_connection_bus_new ("org.freedesktop.Tracker3.Miner.Files",
		                                                 NULL, NULL, &error);
		if (error) {
			g_printerr ("%s: %s\n", _("Could not start indexer"), error->message);
			return EXIT_FAILURE;
		}

		tracker_sparql_connection_close (sparql_conn);
		return EXIT_SUCCESS;
	}

	return daemon_status ();
}

int
tracker_daemon (int          argc,
		const char **argv)
{
	GOptionContext *context;
	GError *error = NULL;

	setlocale (LC_ALL, "");

	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	context = g_option_context_new (NULL);
	g_option_context_add_main_entries (context, entries, NULL);
	g_option_context_set_summary (context, _("If no arguments are given, the status of the data miners is shown"));

	argv[0] = "tracker daemon";

	if (!g_option_context_parse (context, &argc, (char***) &argv, &error)) {
		g_printerr ("%s, %s\n", _("Unrecognized options"), error->message);
		g_error_free (error);
		g_option_context_free (context);
		return EXIT_FAILURE;
	}

	g_option_context_free (context);

	return daemon_run ();
}
