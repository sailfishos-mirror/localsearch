/*
 * Copyright (C) 2024, Carlos Garnacho

 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 * Author: Carlos Garnacho <carlosg@gnome.org>
 */

#include "config-miners.h"

#include <locale.h>
#include <glib.h>
#include <glib-unix.h>
#include <glib/gi18n.h>

#include <libtracker-miners-common/tracker-common.h>
#include <libtracker-sparql/tracker-sparql.h>

#include "tracker-scraper.h"

#define ABOUT	  \
	"Tracker " PACKAGE_VERSION "\n"

#define LICENSE	  \
	"This program is free software and comes without any warranty.\n" \
	"It is licensed under version 2 or later of the General Public " \
	"License which can be viewed at:\n" \
	"\n" \
	"  http://www.gnu.org/licenses/gpl.txt\n"

static gboolean version = FALSE;

static GOptionEntry entries[] = {
	{ "version", 'V', 0,
	  G_OPTION_ARG_NONE, &version,
	  N_("Displays version information"),
	  NULL },
	{ NULL }
};

static GMainLoop *main_loop = NULL;

static gboolean
signal_handler (gpointer user_data)
{
	int signo = GPOINTER_TO_INT (user_data);

	static gboolean in_loop = FALSE;

	/* Die if we get re-entrant signals handler calls */
	if (in_loop) {
		_exit (EXIT_FAILURE);
	}

	switch (signo) {
	case SIGTERM:
	case SIGINT:
		in_loop = TRUE;
		g_main_loop_quit (main_loop);

		/* Fall through */
	default:
		if (g_strsignal (signo)) {
			g_debug ("Received signal:%d->'%s'",
			         signo,
			         g_strsignal (signo));
		}
		break;
	}

	return G_SOURCE_CONTINUE;
}

static void
initialize_signal_handler (void)
{
#ifndef G_OS_WIN32
	g_unix_signal_add (SIGTERM, signal_handler, GINT_TO_POINTER (SIGTERM));
	g_unix_signal_add (SIGINT, signal_handler, GINT_TO_POINTER (SIGINT));
#endif /* G_OS_WIN32 */
}

int
main (gint argc, gchar *argv[])
{
	TrackerSparqlConnection *sparql_conn;
	TrackerScraper *scraper;
	GDBusConnection *conn;
	GOptionContext *context;
	GError *error = NULL;

	setlocale (LC_ALL, "");

	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	/* Translators: this messagge will apper immediately after the
	 * usage string - Usage: COMMAND <THIS_MESSAGE>
	 */
	context = g_option_context_new (_("— start the tracker scraper"));

	g_option_context_add_main_entries (context, entries, NULL);
	g_option_context_parse (context, &argc, &argv, &error);
	g_option_context_free (context);

	if (error) {
		g_printerr ("%s\n", error->message);
		g_error_free (error);
		return EXIT_FAILURE;
	}

	if (version) {
		g_print ("\n" ABOUT "\n" LICENSE "\n");
		return EXIT_SUCCESS;
	}

	conn = g_bus_get_sync (TRACKER_IPC_BUS, NULL, &error);
	if (error) {
		g_critical ("Could not create DBus connection: %s\n",
		            error->message);
		g_error_free (error);
		return EXIT_FAILURE;
	}

	sparql_conn = tracker_sparql_connection_bus_new ("org.freedesktop.Tracker3.Miner.Files",
	                                                 NULL,
	                                                 conn,
	                                                 &error);
	if (error) {
		g_critical ("Could not create SPARQL connection: %s\n",
		            error->message);
		g_error_free (error);
		return EXIT_FAILURE;
	}

	scraper = tracker_scraper_new (sparql_conn);

	main_loop = g_main_loop_new (NULL, FALSE);
	initialize_signal_handler ();
	g_main_loop_run (main_loop);

	g_main_loop_unref (main_loop);
	g_object_unref (scraper);
	g_object_unref (sparql_conn);
	g_object_unref (conn);

	return EXIT_SUCCESS;
}
