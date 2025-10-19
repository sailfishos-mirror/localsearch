/*
 *
 * Copyright (C) 2021, Nishit Patel <nishitlimbani130@gmail.com>
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
 *
 */

#include "config-miners.h"

#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#include "tracker-cli-utils.h"

#include "tracker-common.h"
#include "tracker-color.h"

#define GROUP "Report"
#define KEY_URI "Uri"
#define KEY_MESSAGE "Message"
#define KEY_SPARQL "Sparql"

static gint
sort_by_date (gconstpointer a,
              gconstpointer b)
{
	GFileInfo *info_a = (GFileInfo *) a, *info_b = (GFileInfo *) b;
	gint64 time_a, time_b;

	time_a = g_file_info_get_attribute_uint64 (info_a, G_FILE_ATTRIBUTE_TIME_CREATED);
	time_b = g_file_info_get_attribute_uint64 (info_b, G_FILE_ATTRIBUTE_TIME_CREATED);

	if (time_a < time_b)
		return -1;
	else if (time_a > time_b)
		return 1;
	return 0;
}


GList *
tracker_cli_get_error_keyfiles (void)
{
	GFile *file;
	GFileEnumerator *enumerator;
	GList *infos = NULL, *keyfiles = NULL, *l;
	gchar *path;

	path = g_build_filename (g_get_user_cache_dir (),
	                         "tracker3",
	                         "files",
	                         "errors",
	                         NULL);
	file = g_file_new_for_path (path);
	g_free (path);

	enumerator = g_file_enumerate_children (file,
	                                        G_FILE_ATTRIBUTE_STANDARD_NAME ","
	                                        G_FILE_ATTRIBUTE_TIME_CHANGED,
	                                        G_FILE_QUERY_INFO_NONE,
	                                        NULL,
	                                        NULL);
	while (TRUE) {
		GFileInfo *info;

		if (!g_file_enumerator_iterate (enumerator, &info, NULL, NULL, NULL))
			break;
		if (!info)
			break;

		infos = g_list_prepend (infos, g_object_ref (info));
	}

	infos = g_list_sort (infos, sort_by_date);

	for (l = infos; l; l = l->next) {
		GKeyFile *keyfile;
		GFile *child;
		GError *error = NULL;

		child = g_file_get_child (file, g_file_info_get_name (l->data));
		path = g_file_get_path (child);
		keyfile = g_key_file_new ();

		if (g_key_file_load_from_file (keyfile, path, 0, &error)) {
			keyfiles = g_list_prepend (keyfiles, keyfile);

		} else {
			g_warning ("Error retrieving keyfiles: %s", error->message);
			g_error_free (error);
			g_key_file_free (keyfile);
		}

		g_object_unref (child);
	}

	g_object_unref (enumerator);
	g_list_free_full (infos, g_object_unref);

	return keyfiles;
}

static gboolean
file_matches (GFile *file,
              GStrv  terms)
{
	g_autofree char *uri = NULL, *path = NULL;
	int i;

	uri = g_file_get_uri (file);
	path = g_file_get_path (file);

	if (!terms)
		return TRUE;

	for (i = 0; terms[i]; i++) {
		if (strstr (path, terms[i]))
			return TRUE;
		if (strstr (uri, terms[i]))
			return TRUE;
	}

	return FALSE;
}

gboolean
tracker_cli_print_errors (GList    *keyfiles,
                          GStrv     terms,
                          gboolean  piped)
{
	GList *l;
	GKeyFile *keyfile;
	gboolean found = FALSE;

	for (l = keyfiles; l; l = l->next) {
		g_autoptr(GFile) file = NULL;
		g_autofree gchar *uri = NULL, *path = NULL;

		keyfile = l->data;
		uri = g_key_file_get_string (keyfile, GROUP, KEY_URI, NULL);
		file = g_file_new_for_uri (uri);

		if (!g_file_query_exists (file, NULL)) {
			tracker_error_report_delete (file);
			continue;
		}

		if (file_matches (file, terms)) {
			gchar *sparql = g_key_file_get_string (keyfile, GROUP, KEY_SPARQL, NULL);
			gchar *message = g_key_file_get_string (keyfile, GROUP, KEY_MESSAGE, NULL);

			found = TRUE;
			g_print (!piped ?
			         BOLD_BEGIN "URI:" BOLD_END " %s\n" :
			         "URI: %s\n", uri);

			if (message) {
				g_print (!piped ?
				         BOLD_BEGIN "%s:" BOLD_END " %s\n" :
				         "%s: %s\n",
				         _("Message"), message);
			}

			if (sparql) {
				g_print (!piped ?
				         BOLD_BEGIN "SPARQL:" BOLD_END " %s\n" :
				         "SPARQL: %s\n",
				         sparql);
			}

			g_print ("\n");

			g_free (sparql);
			g_free (message);
		}
	}

	return found;
}

gboolean
tracker_cli_check_inside_build_tree (const gchar* argv0)
{
	g_autoptr (GFile) build_root = NULL;
	g_autoptr (GFile) path = NULL;

	build_root = g_file_new_for_path (BUILDROOT);
	path = g_file_new_for_path (argv0);

	return g_file_has_prefix (path, build_root);
}
