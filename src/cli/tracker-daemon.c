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

#ifdef __sun
#include <procfs.h>
#endif

#include <glib.h>
#include <glib-unix.h>
#include <glib/gi18n.h>
#include <glib/gprintf.h>
#include <locale.h>

#include <tinysparql.h>

#include <tracker-common.h>

#include "tracker-process.h"
#include "tracker-dbus.h"
#include "tracker-miner-manager.h"

static GMainLoop *main_loop;
static GHashTable *miners_progress;
static GHashTable *miners_status;
static gint longest_miner_name_length = 0;
static gint paused_length = 0;

static gboolean status;
static gboolean follow;
static gboolean watch;

static gboolean start;
static gboolean terminate;

#define DAEMON_OPTIONS_ENABLED() \
	((status || follow || watch) || \
	 (start || terminate));

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

static gboolean
miner_get_details (TrackerMinerManager  *manager,
                   const gchar          *miner,
                   gchar               **status,
                   gdouble              *progress,
                   gint                 *remaining_time,
                   GStrv                *pause_applications,
                   GStrv                *pause_reasons)
{
	if ((status || progress || remaining_time) &&
	    !tracker_miner_manager_get_status (manager,
	                                       miner,
	                                       status,
	                                       progress,
	                                       remaining_time)) {
		g_printerr (_("Could not get status from miner: %s"), miner);
		return FALSE;
	}

	tracker_miner_manager_is_paused (manager, miner,
	                                 pause_applications,
	                                 pause_reasons);

	if (!(*pause_applications) || !(*pause_reasons)) {
		/* unable to get pause details,
		   already logged by tracker_miner_manager_is_paused */
		return FALSE;
	}

	return TRUE;
}

static void
miner_print_state (TrackerMinerManager *manager,
                   const gchar         *miner_name,
                   const gchar         *status,
                   gdouble              progress,
                   gint                 remaining_time,
                   gboolean             is_running,
                   gboolean             is_paused)
{
	const gchar *name;
	time_t now;
	gchar time_str[64];
	size_t len;
	struct tm *local_time;

	now = time ((time_t *) NULL);
	local_time = localtime (&now);
	len = strftime (time_str,
	                sizeof (time_str) - 1,
	                "%d %b %Y, %H:%M:%S:",
	                local_time);
	time_str[len] = '\0';

	name = tracker_miner_manager_get_display_name (manager, miner_name);

	if (is_running) {
		gchar *progress_str = NULL;
		gchar *remaining_time_str = NULL;

		if (progress >= 0.0 && progress < 1.0) {
			progress_str = g_strdup_printf ("%3u%%", (guint)(progress * 100));
		}

		/* Progress > 0.01 here because we want to avoid any message
		 * during crawling, as we don't have the remaining time in that
		 * case and it would just print "unknown time left" */
		if (progress > 0.01 &&
		    progress < 1.0 &&
		    remaining_time >= 0) {
			/* 0 means that we couldn't properly compute the remaining
			 * time. */
			if (remaining_time > 0) {
				gchar *seconds_str = tracker_seconds_to_string (remaining_time, TRUE);

				/* Translators: %s is a time string */
				remaining_time_str = g_strdup_printf (_("%s remaining"), seconds_str);
				g_free (seconds_str);
			} else {
				remaining_time_str = g_strdup (_("unknown time left"));
			}
		}

		g_print ("%s  %s  %-*.*s %s%-*.*s%s %s %s %s\n",
		         time_str,
		         progress_str ? progress_str : "✓   ",
		         longest_miner_name_length,
		         longest_miner_name_length,
		         name,
		         is_paused ? "(" : " ",
		         paused_length,
		         paused_length,
		         is_paused ? _("PAUSED") : " ",
		         is_paused ? ")" : " ",
		         status ? "-" : "",
		         status ? _(status) : "",
		         remaining_time_str ? remaining_time_str : "");

		g_free (progress_str);
		g_free (remaining_time_str);
	} else {
		g_print ("%s  ✗     %-*.*s  %-*.*s  - %s\n",
		         time_str,
		         longest_miner_name_length,
		         longest_miner_name_length,
		         name,
		         paused_length,
		         paused_length,
		         " ",
		         _("Not running or is a disabled plugin"));
	}
}

static void
manager_miner_progress_cb (TrackerMinerManager *manager,
                           const gchar         *miner_name,
                           const gchar         *status,
                           gdouble              progress,
                           gint                 remaining_time)
{
	GValue *gvalue;

	gvalue = g_slice_new0 (GValue);

	g_value_init (gvalue, G_TYPE_DOUBLE);
	g_value_set_double (gvalue, progress);

	miner_print_state (manager, miner_name, status, progress, remaining_time, TRUE, FALSE);

	g_hash_table_replace (miners_status,
	                      g_strdup (miner_name),
	                      g_strdup (status));
	g_hash_table_replace (miners_progress,
	                      g_strdup (miner_name),
	                      gvalue);
}

static void
manager_miner_paused_cb (TrackerMinerManager *manager,
                         const gchar         *miner_name)
{
	GValue *gvalue;

	gvalue = g_hash_table_lookup (miners_progress, miner_name);

	miner_print_state (manager, miner_name,
	                   g_hash_table_lookup (miners_status, miner_name),
	                   gvalue ? g_value_get_double (gvalue) : 0.0,
	                   -1,
	                   TRUE,
	                   TRUE);
}

static void
manager_miner_resumed_cb (TrackerMinerManager *manager,
                          const gchar         *miner_name)
{
	GValue *gvalue;

	gvalue = g_hash_table_lookup (miners_progress, miner_name);

	miner_print_state (manager, miner_name,
	                   g_hash_table_lookup (miners_status, miner_name),
	                   gvalue ? g_value_get_double (gvalue) : 0.0,
	                   0,
	                   TRUE,
	                   FALSE);
}

static void
miners_progress_destroy_notify (gpointer data)
{
	GValue *value;

	value = data;
	g_value_unset (value);
	g_slice_free (GValue, value);
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

static gint
daemon_run (void)
{
	TrackerMinerManager *manager;

	/* --follow implies --status */
	if (follow) {
		status = TRUE;
	}

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

	if (status) {
		GError *error = NULL;
		GSList *miners_available;
		GSList *miners_running;
		GSList *l;

		/* Don't auto-start the miners here */
		manager = tracker_miner_manager_new_full (FALSE, &error);
		if (!manager) {
			g_printerr (_("Could not get status, manager could not be created, %s"),
			            error ? error->message : _("No error given"));
			g_printerr ("\n");
			g_clear_error (&error);
			return EXIT_FAILURE;
		}

		miners_available = tracker_miner_manager_get_available (manager);
		miners_running = tracker_miner_manager_get_running (manager);

		/* Work out lengths for output spacing */
		paused_length = strlen (_("PAUSED"));

		for (l = miners_available; l; l = l->next) {
			const gchar *name;

			name = tracker_miner_manager_get_display_name (manager, l->data);
			longest_miner_name_length = MAX (longest_miner_name_length, strlen (name));
		}

		/* Display states */
		g_print ("%s:\n", _("Miners"));

		for (l = miners_available; l; l = l->next) {
			const gchar *name;
			gboolean is_running;

			name = tracker_miner_manager_get_display_name (manager, l->data);
			if (!name) {
				g_critical (_("Could not get display name for miner “%s”"),
				            (const gchar*) l->data);
				continue;
			}

			is_running = tracker_string_in_gslist (l->data, miners_running);

			if (is_running) {
				GStrv pause_applications, pause_reasons;
				gchar *status = NULL;
				gdouble progress;
				gint remaining_time;
				gboolean is_paused;

				if (!miner_get_details (manager,
				                        l->data,
				                        &status,
				                        &progress,
				                        &remaining_time,
				                        &pause_applications,
				                        &pause_reasons)) {
					continue;
				}

				is_paused = *pause_applications || *pause_reasons;

				miner_print_state (manager,
				                   l->data,
				                   status,
				                   progress,
				                   remaining_time,
				                   TRUE,
				                   is_paused);

				g_strfreev (pause_applications);
				g_strfreev (pause_reasons);
				g_free (status);
			} else {
				miner_print_state (manager, l->data, NULL, 0.0, -1, FALSE, FALSE);
			}
		}

		g_slist_foreach (miners_available, (GFunc) g_free, NULL);
		g_slist_free (miners_available);

		g_slist_foreach (miners_running, (GFunc) g_free, NULL);
		g_slist_free (miners_running);

		if (!follow) {
			/* Do nothing further */
			g_print ("\n");
			return EXIT_SUCCESS;
		}

		g_print ("%s\n", _("Press Ctrl+C to stop"));

		g_signal_connect (manager, "miner-progress",
		                  G_CALLBACK (manager_miner_progress_cb), NULL);
		g_signal_connect (manager, "miner-paused",
		                  G_CALLBACK (manager_miner_paused_cb), NULL);
		g_signal_connect (manager, "miner-resumed",
		                  G_CALLBACK (manager_miner_resumed_cb), NULL);

		initialize_signal_handler ();

		miners_progress = g_hash_table_new_full (g_str_hash,
		                                         g_str_equal,
		                                         (GDestroyNotify) g_free,
		                                         (GDestroyNotify) miners_progress_destroy_notify);
		miners_status = g_hash_table_new_full (g_str_hash,
		                                       g_str_equal,
		                                       (GDestroyNotify) g_free,
		                                       (GDestroyNotify) g_free);

		main_loop = g_main_loop_new (NULL, FALSE);
		g_main_loop_run (main_loop);
		g_main_loop_unref (main_loop);

		/* Carriage return, so we paper over the ^C */
		g_print ("\r");

		g_hash_table_unref (miners_progress);
		g_hash_table_unref (miners_status);

		if (manager) {
			g_object_unref (manager);
		}

		return EXIT_SUCCESS;
	}

	/* Processes */
	GError *error = NULL;

	/* Constraints */

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

	/* All known options have their own exit points */
	g_warn_if_reached ();

	return EXIT_FAILURE;
}

static int
daemon_run_default (void)
{
	/* Enable status output in the default run */
	status = TRUE;

	return daemon_run ();
}

static gboolean
daemon_options_enabled (void)
{
	return DAEMON_OPTIONS_ENABLED ();
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

	if (daemon_options_enabled ()) {
		return daemon_run ();
	}

	return daemon_run_default ();
}
