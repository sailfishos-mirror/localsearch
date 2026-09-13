/*
 * Copyright (C) 2020, Red Hat Inc

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
 */

#include "config-miners.h"

#include <string.h>
#include <stdlib.h>
#include <locale.h>
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib-unix.h>
#include <glib-object.h>
#include <glib/gi18n.h>

#include <tracker-common.h>

#include "tracker-miner-files-index.h"

#define ABOUT	  \
	"Tracker " PACKAGE_VERSION "\n"

#define LICENSE	  \
	"This program is free software and comes without any warranty.\n" \
	"It is licensed under version 2 or later of the General Public " \
	"License which can be viewed at:\n" \
	"\n" \
	"  http://www.gnu.org/licenses/gpl.txt\n"

#define DBUS_NAME_PREFIX "org.freedesktop.LocalSearch3"
#define DBUS_NAME_LEGACY_PREFIX "org.freedesktop.Tracker3.Miner.Files"

static gboolean version;

static GOptionEntry entries[] = {
	{ "version", 'V', 0,
	  G_OPTION_ARG_NONE, &version,
	  N_("Displays version information"),
	  NULL },
	{ NULL }
};

static gboolean
signal_handler (gpointer user_data)
{
	GMainLoop *loop = user_data;

	g_main_loop_quit (loop);
	return G_SOURCE_REMOVE;
}

static void
initialize_signal_handler (GMainLoop *main_loop)
{
#ifndef G_OS_WIN32
	g_unix_signal_add (SIGTERM, signal_handler, main_loop);
	g_unix_signal_add (SIGINT, signal_handler, main_loop);
#endif /* G_OS_WIN32 */
}

static void
files_index_close_cb (TrackerMinerFilesIndex *index,
                      GMainLoop              *main_loop)
{
	g_debug ("No further watched folders, closing");
	g_main_loop_quit (main_loop);
}

static void
name_lost_cb (GDBusConnection *connection,
              const char      *name,
              gpointer         user_data)
{
	g_main_loop_quit (user_data);
}

int
main (gint argc, gchar *argv[])
{
	g_autoptr (GOptionContext) context = NULL;
	g_autoptr (GError) error = NULL;
	g_autoptr (GDBusConnection) connection = NULL;
	g_autoptr (TrackerMinerFilesIndex) index = NULL;
	g_autoptr (GMainLoop) main_loop = NULL;

	setlocale (LC_ALL, "");

	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	/* Set timezone info */
	tzset ();

	/* Translators: this messagge will apper immediately after the
	 * usage string - Usage: COMMAND <THIS_MESSAGE>
	 */
	context = g_option_context_new (_("— start the tracker index proxy"));

	g_option_context_add_main_entries (context, entries, NULL);
	g_option_context_parse (context, &argc, &argv, &error);

	if (error)
		goto error;

	if (version) {
		g_print ("\n" ABOUT "\n" LICENSE "\n");
		return EXIT_SUCCESS;
	}

	connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
	if (error)
		goto error;

	main_loop = g_main_loop_new (NULL, FALSE);

	index = tracker_miner_files_index_new (&error);
	if (!index)
		goto error;

	g_signal_connect (index, "close",
	                  G_CALLBACK (files_index_close_cb), main_loop);

	/* Request DBus names */
	g_bus_own_name_on_connection (connection,
	                              DBUS_NAME_PREFIX ".Control",
	                              G_BUS_NAME_OWNER_FLAGS_NONE,
	                              NULL,
	                              name_lost_cb,
	                              main_loop,
	                              NULL);

	g_bus_own_name_on_connection (connection,
	                              DBUS_NAME_LEGACY_PREFIX ".Control",
	                              G_BUS_NAME_OWNER_FLAGS_NONE,
	                              NULL,
	                              name_lost_cb,
	                              main_loop,
	                              NULL);

	initialize_signal_handler (main_loop);

	g_main_loop_run (main_loop);

	g_debug ("Shutdown started");

	return EXIT_SUCCESS;
 error:
	g_printerr ("%s\n", error->message);
	return EXIT_FAILURE;
}
