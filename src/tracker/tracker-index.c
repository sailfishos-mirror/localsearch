/*
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

#include <stdlib.h>
#include <errno.h>

#ifdef __sun
#include <procfs.h>
#endif

#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <locale.h>

#include <libtracker-sparql/tracker-sparql.h>
#include <libtracker-miners-common/tracker-common.h>

#include "tracker-color.h"
#include "tracker-dbus.h"
#include "tracker-miner-manager.h"

static gchar **filenames;

static GOptionEntry entries[] = {
	{ G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &filenames,
	  N_("FILE"),
	  N_("FILE") },
	{ NULL }
};

const struct {
	const gchar *symbol;
	GUserDirectory user_dir;
} special_dirs[] = {
	{ "&DESKTOP",      G_USER_DIRECTORY_DESKTOP },
	{ "&DOCUMENTS",    G_USER_DIRECTORY_DOCUMENTS },
	{ "&DOWNLOAD",     G_USER_DIRECTORY_DOWNLOAD },
	{ "&MUSIC",        G_USER_DIRECTORY_MUSIC },
	{ "&PICTURES",     G_USER_DIRECTORY_PICTURES },
	{ "&PUBLIC_SHARE", G_USER_DIRECTORY_PUBLIC_SHARE },
	{ "&TEMPLATES",    G_USER_DIRECTORY_TEMPLATES },
	{ "&VIDEOS",       G_USER_DIRECTORY_VIDEOS }
};

static const gchar *
alias_to_path (const gchar *alias)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS (special_dirs); i++) {
		if (g_strcmp0 (special_dirs[i].symbol, alias) == 0)
			return g_get_user_special_dir (special_dirs[i].user_dir);
	}

	return NULL;
}

static const gchar *
envvar_to_path (const gchar *envvar)
{
	const gchar *path;

	path = g_getenv (&envvar[1]);
	if (g_file_test (path, G_FILE_TEST_EXISTS))
		return path;

	return NULL;
}

static gint
index_or_reindex_file (void)
{
	TrackerMinerManager *manager;
	GError *error = NULL;
	gchar **p;

	/* Auto-start the miners here if we need to */
	manager = tracker_miner_manager_new_full (TRUE, &error);
	if (!manager) {
		g_printerr (_("Could not (re)index file, manager could not be created, %s"),
		            error ? error->message : _("No error given"));
		g_printerr ("\n");
		g_clear_error (&error);
		return EXIT_FAILURE;
	}

	for (p = filenames; *p; p++) {
		GFile *file;

		file = g_file_new_for_commandline_arg (*p);
		tracker_miner_manager_index_location (manager, file, NULL, TRACKER_INDEX_LOCATION_FLAGS_NONE, NULL, &error);

		if (error) {
			g_printerr ("%s: %s\n",
			            _("Could not (re)index file"),
			            error->message);
			g_error_free (error);
			return EXIT_FAILURE;
		}

		g_print ("%s\n", _("(Re)indexing file was successful"));
		g_object_unref (file);
	}

	g_object_unref (manager);

	return EXIT_SUCCESS;
}

static int
index_run (void)
{
	return index_or_reindex_file ();
}

static void
print_list (GStrv    list,
            gint     len,
            gboolean recursive)
{
	guint i;

	for (i = 0; list[i]; i++) {
		const gchar *path;
		gchar *str;

		if (list[i][0] == '&')
			path = alias_to_path (list[i]);
		else if (list[i][0] == '$')
			path = envvar_to_path (list[i]);
		else if (list[i][0] == '/')
			path = list[i];
		else
			continue;

		str = tracker_term_ellipsize (path, len, TRACKER_ELLIPSIZE_START);
		g_print ("%-*s " BOLD_BEGIN "%s" BOLD_END "\n",
		         len, str,
		         recursive ? "*" : "-");
		g_free (str);
	}
}

static int
list_index_roots (void)
{
	GSettings *settings;
	GStrv recursive, non_recursive;
	gint cols, col_len[2];
	gchar *col_header1, *col_header2;

	settings = g_settings_new ("org.freedesktop.Tracker3.Miner.Files");
	recursive = g_settings_get_strv (settings, "index-recursive-directories");
	non_recursive = g_settings_get_strv (settings, "index-single-directories");

	tracker_term_dimensions (&cols, NULL);
	col_len[0] = cols * 3 / 4;
	col_len[1] = cols / 4 - 1;

	col_header1 = tracker_term_ellipsize (_("Path"), col_len[0], TRACKER_ELLIPSIZE_END);
	col_header2 = tracker_term_ellipsize (_("Recursive"), col_len[1], TRACKER_ELLIPSIZE_END);

	g_print (BOLD_BEGIN "%-*s %-*s" BOLD_END "\n",
	         col_len[0], col_header1,
	         col_len[1], col_header2);
	g_free (col_header1);
	g_free (col_header2);

	print_list (recursive, col_len[0], TRUE);
	print_list (non_recursive, col_len[0], FALSE);

	g_strfreev (recursive);
	g_strfreev (non_recursive);
	g_object_unref (settings);

	return EXIT_SUCCESS;
}

int
main (int argc, const char **argv)
{
	GOptionContext *context;
	GError *error = NULL;
	const gchar *failed;

	setlocale (LC_ALL, "");

	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	context = g_option_context_new (NULL);
	g_option_context_add_main_entries (context, entries, NULL);

	argv[0] = "tracker index";

	if (!g_option_context_parse (context, &argc, (char***) &argv, &error)) {
		g_printerr ("%s, %s\n", _("Unrecognized options"), error->message);
		g_error_free (error);
		g_option_context_free (context);
		return EXIT_FAILURE;
	}

	g_option_context_free (context);

	if (!filenames) {
		return list_index_roots ();
	}

	if (g_strv_length (filenames) < 1) {
		failed = _("Please specify one or more locations to index.");
		g_printerr ("%s\n", failed);
		return EXIT_FAILURE;
	}

	return index_run ();
}
