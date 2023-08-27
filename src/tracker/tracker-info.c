/*
 * Copyright (C) 2006, Jamie McCracken <jamiemcc@gnome.org>
 * Copyright (C) 2008-2010, Nokia <ivan.frade@nokia.com>
 * Copyright (C) 2014, SoftAtHome <contact@softathome.com>
 * Copyright (C) 2014, Lanedo <martyn@lanedo.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config-miners.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <locale.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gunixoutputstream.h>

#include <libtracker-sparql/tracker-sparql.h>
#include <libtracker-miners-common/tracker-common.h>

#include "tracker-cli-utils.h"
#include "tracker-color.h"

#define INFO_OPTIONS_ENABLED() \
	(filenames && g_strv_length (filenames) > 0);

#define GROUP "Report"
#define KEY_URI "Uri"
#define KEY_MESSAGE "Message"
#define KEY_SPARQL "Sparql"
#define ERROR_MESSAGE "Extraction failed for this file. Some metadata will be missing."

static gboolean inside_build_tree = FALSE;

static gchar **filenames;
static gboolean full_namespaces;
static gboolean plain_text_content;
static gboolean resource_is_iri;
static gboolean turtle;
static gboolean eligible;
static gchar *url_property;

static GOptionEntry entries[] = {
	{ "full-namespaces", 'f', 0, G_OPTION_ARG_NONE, &full_namespaces,
	  N_("Show full namespaces (i.e. don’t use nie:title, use full URLs)"),
	  NULL,
	},
	{ "plain-text-content", 'c', 0, G_OPTION_ARG_NONE, &plain_text_content,
	  N_("Show plain text content if available for resources"),
	  NULL,
	},
	{ "resource-is-iri", 'i', 0, G_OPTION_ARG_NONE, &resource_is_iri,
	  /* To translators:
	   * IRI (International Resource Identifier) is a generalization
	   * of the URI. While URI supports only ASCI encoding, IRI
	   * fully supports international characters. In practice, UTF-8
	   * is the most popular encoding used for IRI.
	   */
	  N_("Instead of looking up a file name, treat the FILE arguments as actual IRIs (e.g. <file:///path/to/some/file.txt>)"),
	  NULL,
	},
	{ "turtle", 't', 0, G_OPTION_ARG_NONE, &turtle,
	  N_("Output results as RDF in Turtle format"),
	  NULL,
	},
	{ "url", 'u', 0, G_OPTION_ARG_STRING, &url_property,
	  N_("RDF property to treat as URL (eg. “nie:url”)"),
	  NULL,
	},
	{ "eligible", 'e', 0, G_OPTION_ARG_NONE, &eligible,
	  N_("Checks if FILE is eligible for being mined based on configuration"),
	  NULL },
	{ G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &filenames,
	  N_("FILE"),
	  N_("FILE")},
	{ NULL }
};

static gboolean
has_valid_uri_scheme (const gchar *uri)
{
	const gchar *s;

	s = uri;

	if (!g_ascii_isalpha (*s)) {
		return FALSE;
	}

	do {
		s++;
	} while (g_ascii_isalnum (*s) || *s == '+' || *s == '.' || *s == '-');

	return (*s == ':');
}

static TrackerSparqlStatement *
describe_statement_for_urns (TrackerSparqlConnection *conn,
                             GList                   *urns)
{
	TrackerSparqlStatement *stmt;
	g_autoptr (GString) str = NULL;
	GList *l;

	str = g_string_new ("DESCRIBE");

	for (l = urns; l; l = l->next)
		g_string_append_printf (str, " <%s>", (gchar*) l->data);

	stmt = tracker_sparql_connection_query_statement (conn,
	                                                  str->str,
	                                                  NULL, NULL);

	return stmt;
}

static inline void
print_key_and_value (TrackerNamespaceManager *namespaces,
                     const gchar             *key,
                     const gchar             *value)
{
	if (G_UNLIKELY (full_namespaces)) {
		g_print ("  '%s' = '%s'\n", key, value);
	} else {
		g_autofree gchar *shorthand = NULL;

		shorthand = tracker_namespace_manager_compress_uri (namespaces, key);
		g_print ("  '%s' = '%s'\n", shorthand, value);
	}
}

static gboolean
print_plain (const gchar             *urn_or_filename,
             const gchar             *urn,
             TrackerSparqlCursor     *cursor,
             TrackerNamespaceManager *namespaces,
             gboolean                 full_namespaces)
{
	gchar *fts_key = NULL;
	gchar *fts_value = NULL;
	gboolean has_output = FALSE;

	while (tracker_sparql_cursor_next (cursor, NULL, NULL)) {
		const gchar *key = tracker_sparql_cursor_get_string (cursor, 0, NULL);
		const gchar *value = tracker_sparql_cursor_get_string (cursor, 1, NULL);

		if (!key || !value) {
			continue;
		}

		if (!has_output) {
			g_print ("%s:\n", _("Results"));
			has_output = TRUE;
		}

		/* Don't display nie:plainTextContent */
		if (strcmp (key, "http://tracker.api.gnome.org/ontology/v3/nie#plainTextContent") == 0) {
			if (plain_text_content) {
				fts_key = g_strdup (key);
				fts_value = g_strdup (value);
			}

			/* Always print FTS data at the end because of it's length */
			continue;
		}

		print_key_and_value (namespaces, key, value);
	}

	if (fts_key && fts_value) {
		print_key_and_value (namespaces, fts_key, fts_value);
	}

	g_free (fts_key);
	g_free (fts_value);

	return has_output;
}

static void
serialize_cb (GObject *object,
              GAsyncResult *res,
              gpointer      user_data)
{
	GMainLoop *main_loop = user_data;
	g_autoptr (GInputStream) istream = NULL;
	g_autoptr (GOutputStream) ostream = NULL;
	g_autoptr (GError) error = NULL;

	istream = tracker_sparql_statement_serialize_finish (TRACKER_SPARQL_STATEMENT (object),
	                                                     res, &error);
	if (istream) {
		tracker_term_pipe_to_pager ();
		ostream = g_unix_output_stream_new (STDOUT_FILENO, FALSE);
		g_output_stream_splice (ostream, istream, G_OUTPUT_STREAM_SPLICE_NONE, NULL, &error);
		g_output_stream_close (ostream, NULL, NULL);
		tracker_term_pager_close ();
	}

	if (error)
		g_printerr ("%s\n", error->message);

	g_main_loop_quit (main_loop);
}

static TrackerSparqlConnection *
create_connection (GError **error)
{
	return tracker_sparql_connection_bus_new ("org.freedesktop.Tracker3.Miner.Files",
	                                          NULL, NULL, error);
}

static gboolean
output_eligible_status_for_file (gchar   *path,
                                 GError **error)
{
	g_autofree char *tracker_miner_fs_path = NULL;

	if (inside_build_tree) {
		/* Developer convienence - use uninstalled version if running from build tree */
		tracker_miner_fs_path = g_build_filename (BUILDROOT, "src", "miners", "fs", "tracker-miner-fs-3", NULL);
	} else {
		tracker_miner_fs_path = g_build_filename (LIBEXECDIR, "tracker-miner-fs-3", NULL);
	}

	{
		char *argv[] = {tracker_miner_fs_path, "--eligible", path, NULL };

		return g_spawn_sync (NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL, NULL, error);
	}
}

static void
print_errors (GList       *keyfiles,
              const gchar *file_uri)
{
	GList *l;
	GKeyFile *keyfile;
	GFile *file;

	file = g_file_new_for_uri (file_uri);


	for (l = keyfiles; l; l = l->next) {
		gchar *uri;
		GFile *error_file;

		keyfile = l->data;
		uri = g_key_file_get_string (keyfile, GROUP, KEY_URI, NULL);
		error_file = g_file_new_for_uri (uri);

		if (g_file_equal (file, error_file)) {
			gchar *message = g_key_file_get_string (keyfile, GROUP, KEY_MESSAGE, NULL);
			gchar *sparql = g_key_file_get_string (keyfile, GROUP, KEY_SPARQL, NULL);

			if (message)
				g_print (CRIT_BEGIN "%s\n%s: %s" CRIT_END "\n",
					 ERROR_MESSAGE,
					 _("Error message"),
					 message);
			if (sparql)
				g_print ("SPARQL: %s\n", sparql);
			g_print ("\n");

			g_free (message);
			g_free (sparql);
		}

		g_free (uri);
		g_object_unref (error_file);
	}

	g_object_unref (file);

}


static int
info_run (void)
{
	TrackerSparqlConnection *connection;
	GError *error = NULL;
	GList *urns = NULL, *l;
	gchar **p;

	connection = create_connection (&error);

	if (!connection) {
		g_printerr ("%s: %s\n",
		            _("Could not establish a connection to Tracker"),
		            error ? error->message : _("No error given"));
		g_clear_error (&error);
		return EXIT_FAILURE;
	}

	for (p = filenames; *p; p++) {
		g_autoptr (TrackerSparqlStatement) stmt = NULL;
		g_autoptr (TrackerSparqlCursor) cursor = NULL;
		gchar *uri = NULL;
		gchar *query;
		GList *keyfiles;
		gboolean found = FALSE;

		if (!turtle && !resource_is_iri) {
			g_print ("%s: '%s'\n", _("Querying information for entity"), *p);
		}

		/* support both, URIs and local file paths */
		if (has_valid_uri_scheme (*p)) {
			uri = g_strdup (*p);
		} else if (resource_is_iri) {
			uri = g_strdup (*p);
		} else {
			GFile *file;

			file = g_file_new_for_commandline_arg (*p);
			uri = g_file_get_uri (file);
			g_object_unref (file);
		}

		if (url_property) {
			g_autoptr (TrackerSparqlStatement) stmt = NULL;
			g_autoptr (TrackerSparqlCursor) cursor = NULL;

			/* First check whether there's some entity with nie:url like this */
			query = g_strdup_printf ("SELECT ?urn { ?urn %s ~value }", url_property);
			stmt = tracker_sparql_connection_query_statement (connection, query,
			                                                  NULL, &error);
			g_free (query);

			if (stmt) {
				tracker_sparql_statement_bind_string (stmt, "prop", url_property);
				tracker_sparql_statement_bind_string (stmt, "value", uri);
				cursor = tracker_sparql_statement_execute (stmt, NULL, &error);
			}

			if (cursor && tracker_sparql_cursor_next (cursor, NULL, &error)) {
				g_clear_pointer (&uri, g_free);
				uri = g_strdup (tracker_sparql_cursor_get_string (cursor, 0, NULL));
			}

			if (error) {
				g_printerr ("  %s, %s\n",
				            _("Unable to retrieve URN for URI"),
				            error->message);
				g_clear_error (&error);
				continue;
			}
		}

		stmt = tracker_sparql_connection_query_statement (connection,
		                                                  "SELECT DISTINCT ?urn {"
		                                                  "  {"
		                                                  "    BIND (~uri AS ?urn) . "
		                                                  "    ?urn a rdfs:Resource . "
		                                                  "  } UNION {"
		                                                  "    ~uri nie:interpretedAs ?urn ."
		                                                  "  }"
		                                                  "}",
		                                                  NULL, &error);
		if (stmt) {
			tracker_sparql_statement_bind_string (stmt, "uri", uri);
			cursor = tracker_sparql_statement_execute (stmt, NULL, &error);
		}

		if (cursor) {
			while (tracker_sparql_cursor_next (cursor, NULL, NULL)) {
				const gchar *str;

				str = tracker_sparql_cursor_get_string (cursor, 0, NULL);
				urns = g_list_prepend (urns, g_strdup (str));
				found = TRUE;
			}
		}

		if (error) {
			g_printerr ("  %s, %s\n",
			            _("Unable to retrieve data for URI"),
			            error->message);
			g_clear_error (&error);
		}

		if (!found) {
			if (turtle) {
				g_print ("# No metadata available for <%s>\n", uri);
			} else {
				g_print ("  %s\n",
				         _("No metadata available for that URI"));
				output_eligible_status_for_file (*p, &error);

				if (error) {
					g_printerr ("%s: %s\n",
					            _("Could not get eligible status: "),
					            error->message);
					g_clear_error (&error);
				}

				keyfiles = tracker_cli_get_error_keyfiles ();
				if (keyfiles)
					print_errors (keyfiles, uri);
			}
		}
	}

	if (!urns)
		goto out;

	if (turtle) {
		g_autoptr (TrackerSparqlStatement) stmt = NULL;
		g_autoptr (GMainLoop) main_loop = NULL;

		stmt = describe_statement_for_urns (connection, urns);
		main_loop = g_main_loop_new (NULL, FALSE);
		tracker_sparql_statement_serialize_async (stmt,
		                                          TRACKER_SERIALIZE_FLAGS_NONE,
		                                          TRACKER_RDF_FORMAT_TURTLE,
		                                          NULL,
		                                          serialize_cb,
		                                          main_loop);
		g_main_loop_run (main_loop);

		g_list_free_full (urns, g_free);
		g_object_unref (connection);

		return EXIT_SUCCESS;
	}

	for (l = urns; l; l = l->next) {
		TrackerSparqlCursor *cursor;
		TrackerNamespaceManager *namespaces;
		const gchar *urn = l->data;
		gchar *query;

		query = g_strdup_printf ("SELECT DISTINCT ?predicate ?object ?x"
		                         "  ( EXISTS { ?predicate rdfs:range [ rdfs:subClassOf rdfs:Resource ] } )"
		                         "WHERE {"
		                         "  <%s> nie:interpretedAs? ?x . "
					 "  ?x ?predicate ?object . "
		                         "} ORDER BY ?x",
					 urn);

		namespaces = tracker_sparql_connection_get_namespace_manager (connection);
		cursor = tracker_sparql_connection_query (connection, query, NULL, &error);

		g_free (query);

		if (error) {
			g_printerr ("  %s, %s\n",
			            _("Unable to retrieve data for URI"),
			            error->message);

			g_clear_error (&error);
			continue;
		}

		if (cursor) {
			g_print ("  '%s'\n", urn);
			print_plain (*p, urn, cursor, namespaces, full_namespaces);
			g_object_unref (cursor);
		}

		g_print ("\n");
	}

 out:
	g_list_free_full (urns, g_free);
	g_object_unref (connection);

	return EXIT_SUCCESS;
}

static int
info_run_eligible (void)
{
	char **p;
	g_autoptr (GError) error = NULL;

	for (p = filenames; *p; p++) {
		output_eligible_status_for_file (*p, &error);

		if (error) {
			g_printerr ("%s: %s\n",
			            _("Could not get eligible status: "),
			            error->message);
			return EXIT_FAILURE;
		}
	}

	return EXIT_SUCCESS;
}


static int
info_run_default (void)
{
	GOptionContext *context;
	gchar *help;

	context = g_option_context_new (NULL);
	g_option_context_add_main_entries (context, entries, NULL);
	help = g_option_context_get_help (context, TRUE, NULL);
	g_option_context_free (context);
	g_printerr ("%s\n", help);
	g_free (help);

	return EXIT_FAILURE;
}

static gboolean
info_options_enabled (void)
{
	return INFO_OPTIONS_ENABLED ();
}

int
main (int argc, const char **argv)
{
	GOptionContext *context;
	GError *error = NULL;

	context = g_option_context_new (NULL);
	g_option_context_add_main_entries (context, entries, NULL);

	inside_build_tree = tracker_cli_check_inside_build_tree (argv[0]);

	argv[0] = "tracker info";

	if (!g_option_context_parse (context, &argc, (char***) &argv, &error)) {
		g_printerr ("%s, %s\n", _("Unrecognized options"), error->message);
		g_error_free (error);
		g_option_context_free (context);
		return EXIT_FAILURE;
	}

	g_option_context_free (context);

	if (info_options_enabled ()) {
		if (eligible)
			return info_run_eligible ();

		return info_run ();
	}

	return info_run_default ();
}
