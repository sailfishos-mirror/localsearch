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

#include "tracker-scraper.h"

#define QUERY_RESOURCE_PATH_PREFIX "/org/freedesktop/Tracker3/Scraper/queries/"
#define BATCH_SIZE_LIMIT 100

typedef struct _TrackerScraper TrackerScraper;

typedef struct _Operation Operation;

/* Function to bind rows in a local resultset to parameters in a wikidata query */
typedef void (* BindFunc) (TrackerScraper         *scraper,
                           TrackerSparqlStatement *remote_stmt,
                           TrackerSparqlCursor    *local_cursor);

struct _Operation {
	const gchar *data_query;
	BindFunc bind;
	const gchar *search_query;
	const gchar *insert_query;
};

struct _TrackerScraper
{
	GObject parent_instance;
	TrackerSparqlConnection *sparql_conn;
	TrackerSparqlCursor *local_cursor;
	GCancellable *cancellable;
	TrackerBatch *batch;
	int cur;
	int batch_size;
};

enum {
	PROP_0,
	PROP_SPARQL_CONNECTION,
	N_PROPS,
};

GParamSpec *props[N_PROPS] = { 0, };

G_DEFINE_TYPE (TrackerScraper, tracker_scraper, G_TYPE_OBJECT)

static void next_item (TrackerScraper *scraper);
static void next_operation (TrackerScraper *scraper);

enum {
	MUSIC_ARTISTS,
	N_OPERATIONS,
};

Operation operations[] = {
	{ "local-music-artists.rq", NULL, "search-music-artist.rq", "complete-music-artist.rq" },
};

G_STATIC_ASSERT (G_N_ELEMENTS (operations) == N_OPERATIONS);

static void
bind_generic (TrackerScraper         *scraper,
              TrackerSparqlStatement *remote_stmt,
              TrackerSparqlCursor    *local_cursor)
{
	gint i;

	/* Map cursor variable names in local query to parameters in the remote statement */
	for (i = 0; i < tracker_sparql_cursor_get_n_columns (local_cursor); i++) {
		const gchar *column_name, *value;

		column_name = tracker_sparql_cursor_get_variable_name (local_cursor, i);
		value = tracker_sparql_cursor_get_string (local_cursor, i, NULL);
		tracker_sparql_statement_bind_string (remote_stmt, column_name, value);
		g_debug ("Bound query statement argument '%s' to '%s'", column_name, value);
	}
}

static void
tracker_scraper_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
	TrackerScraper *scraper = TRACKER_SCRAPER (object);

	switch (prop_id) {
	case PROP_SPARQL_CONNECTION:
		scraper->sparql_conn = g_value_dup_object (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
tracker_scraper_finalize (GObject *object)
{
	TrackerScraper *scraper = TRACKER_SCRAPER (object);

	g_cancellable_cancel (scraper->cancellable);
	g_clear_object (&scraper->cancellable);
	g_clear_object (&scraper->sparql_conn);
	g_clear_object (&scraper->local_cursor);
	g_clear_object (&scraper->batch);

	G_OBJECT_CLASS (tracker_scraper_parent_class)->finalize (object);
}

static TrackerSparqlStatement *
load_statement (TrackerScraper  *scraper,
                const gchar     *query_file,
                GError         **error)
{
	g_autofree gchar *resource_path;

	resource_path = g_strconcat (QUERY_RESOURCE_PATH_PREFIX, query_file, NULL);

	return tracker_sparql_connection_load_statement_from_gresource (scraper->sparql_conn,
	                                                                resource_path,
	                                                                scraper->cancellable,
	                                                                error);
}

static void
flush_batch_cb (GObject      *object,
                GAsyncResult *res,
                gpointer      user_data)
{
	g_autoptr (GError) error = NULL;

	g_debug ("Batch executed");
	tracker_batch_execute_finish (TRACKER_BATCH (object), res, &error);

	if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		g_warning ("Could not update local elements: %s", error->message);
}

static void
flush_batch (TrackerScraper *scraper)
{
	g_autoptr (TrackerBatch) batch = NULL;

	if (!scraper->batch)
		return;

	g_debug ("Executing batch");
	batch = g_steal_pointer (&scraper->batch);
	tracker_batch_execute_async (batch, scraper->cancellable,
	                             flush_batch_cb, scraper);
}

static void
push_to_batch (TrackerScraper         *scraper,
               TrackerSparqlStatement *stmt,
               GPtrArray              *names,
               GArray                 *values)
{
	if (!scraper->batch) {
		scraper->batch = tracker_sparql_connection_create_batch (scraper->sparql_conn);
		scraper->batch_size = 0;
	}

	g_assert (names->len == values->len);
	tracker_batch_add_statementv (scraper->batch,
	                              stmt,
	                              names->len,
	                              (const gchar **) names->pdata,
	                              (const GValue *) values->data);
	scraper->batch_size++;

	if (scraper->batch_size > BATCH_SIZE_LIMIT)
		flush_batch (scraper);
}

static void
collect_update_statement_bindings_from_cursor (GPtrArray           *names,
                                               GArray              *values,
                                               TrackerSparqlCursor *cursor)
{
	gint i;

	for (i = 0; i < tracker_sparql_cursor_get_n_columns (cursor); i++) {
		GValue value = G_VALUE_INIT;

		switch (tracker_sparql_cursor_get_value_type (cursor, i)) {
		case TRACKER_SPARQL_VALUE_TYPE_URI:
		case TRACKER_SPARQL_VALUE_TYPE_STRING:
			g_value_init (&value, G_TYPE_STRING);
			g_value_set_string (&value,
			                    tracker_sparql_cursor_get_string (cursor, i, NULL));
			break;
		case TRACKER_SPARQL_VALUE_TYPE_INTEGER:
			g_value_init (&value, G_TYPE_INT64);
			g_value_set_int64 (&value,
			                   tracker_sparql_cursor_get_integer (cursor, i));
			break;
		case TRACKER_SPARQL_VALUE_TYPE_DOUBLE:
			g_value_init (&value, G_TYPE_DOUBLE);
			g_value_set_double (&value,
			                    tracker_sparql_cursor_get_double (cursor, i));
			break;
		case TRACKER_SPARQL_VALUE_TYPE_DATETIME:
			g_value_init (&value, G_TYPE_DATE_TIME);
			g_value_take_boxed (&value,
			                    tracker_sparql_cursor_get_datetime (cursor, i));
			break;
		case TRACKER_SPARQL_VALUE_TYPE_BOOLEAN:
			g_value_init (&value, G_TYPE_BOOLEAN);
			g_value_set_boolean (&value,
			                     tracker_sparql_cursor_get_boolean (cursor, i));
			break;
		case TRACKER_SPARQL_VALUE_TYPE_UNBOUND:
		case TRACKER_SPARQL_VALUE_TYPE_BLANK_NODE:
			break;
		}

		if (G_VALUE_TYPE (&value) != G_TYPE_INVALID) {
			g_debug ("Bound update statement argument '%s' to %s (%s)",
			         tracker_sparql_cursor_get_variable_name (cursor, i),
			         tracker_sparql_cursor_get_string (cursor, i, NULL),
			         G_VALUE_TYPE_NAME (&value));

			g_ptr_array_add (names,
			                 (gpointer) tracker_sparql_cursor_get_variable_name (cursor, i));
			g_array_append_val (values, value);
		}
	}
}

static void
search_query_cb (GObject      *object,
                 GAsyncResult *res,
                 gpointer      user_data)
{
	TrackerScraper *scraper = user_data;
	g_autoptr (TrackerSparqlCursor) cursor = NULL;
	g_autoptr (GError) error = NULL;

	cursor = tracker_sparql_statement_execute_finish (TRACKER_SPARQL_STATEMENT (object),
	                                                  res, &error);

	if (cursor && tracker_sparql_cursor_next (cursor, scraper->cancellable, &error)) {
		Operation *operation = &operations[scraper->cur];
		g_autoptr (TrackerSparqlStatement) insert_stmt = NULL;
		g_autoptr (TrackerResource) resource = NULL;

		g_debug ("Found remote match for resource '%s'",
		         tracker_sparql_cursor_get_string (scraper->local_cursor, 0, NULL));

		insert_stmt = load_statement (scraper, operation->insert_query, &error);

		if (insert_stmt) {
			g_autoptr (GPtrArray) names = NULL;
			g_autoptr (GArray) values = NULL;

			names = g_ptr_array_new ();
			values = g_array_new (FALSE, FALSE, sizeof (GValue));
			g_array_set_clear_func (values, (GDestroyNotify) g_value_unset);

			collect_update_statement_bindings_from_cursor (names, values, scraper->local_cursor);
			collect_update_statement_bindings_from_cursor (names, values, cursor);
			push_to_batch (scraper, insert_stmt, names, values);
		}
	}

	if (error)
		g_warning ("Could not match with remote database: %s", error->message);

	next_item (scraper);
}

static void
next_item (TrackerScraper *scraper)
{
	Operation *operation = &operations[scraper->cur];
	g_autoptr (TrackerSparqlStatement) stmt = NULL;
	g_autoptr (GError) error = NULL;

	if (tracker_sparql_cursor_next (scraper->local_cursor, scraper->cancellable, &error))
		stmt = load_statement (scraper, operation->search_query, &error);

	if (stmt) {
		BindFunc bind_func;

		g_debug ("Scraping data for resource '%s'",
		         tracker_sparql_cursor_get_string (scraper->local_cursor, 0, NULL));

		bind_func = operation->bind ? operation->bind : bind_generic;
		bind_func (scraper, stmt, scraper->local_cursor);

		tracker_sparql_statement_execute_async (stmt,
		                                        scraper->cancellable,
		                                        search_query_cb,
		                                        scraper);
	} else if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		if (error)
			g_warning ("Could not get next local item: %s", error->message);

		next_operation (scraper);
	}
}

static void
data_query_cb (GObject      *object,
               GAsyncResult *res,
               gpointer      user_data)
{
	TrackerScraper *scraper = user_data;
	g_autoptr (GError) error = NULL;

	scraper->local_cursor =
		tracker_sparql_statement_execute_finish (TRACKER_SPARQL_STATEMENT (object),
		                                         res,
		                                         &error);
	if (scraper->local_cursor)
		next_item (scraper);

	if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		g_warning ("Could not query: %s", error->message);
		next_operation (scraper);
	}
}

static void
start_operation (TrackerScraper *scraper)
{
	Operation *operation = &operations[scraper->cur];
	g_autoptr (TrackerSparqlStatement) stmt = NULL;
	g_autoptr (GError) error = NULL;

	stmt = load_statement (scraper, operation->data_query, &error);

	if (stmt) {
		tracker_sparql_statement_execute_async (stmt, scraper->cancellable,
		                                        data_query_cb, scraper);
	}

	if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		g_warning ("Could load statement: %s", error->message);
		next_operation (scraper);
	}
}

static void
next_operation (TrackerScraper *scraper)
{
	scraper->cur++;

	if (scraper->cur < N_OPERATIONS) {
		g_debug ("Next operation...");
		start_operation (scraper);
	} else {
		g_debug ("Finished...");
		flush_batch (scraper);
	}
}

static void
tracker_scraper_constructed (GObject *object)
{
	TrackerScraper *scraper = TRACKER_SCRAPER (object);

	G_OBJECT_CLASS (tracker_scraper_parent_class)->constructed (object);

	g_debug ("Starting...");
	start_operation (scraper);
}

static void
tracker_scraper_class_init (TrackerScraperClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->set_property = tracker_scraper_set_property;
	object_class->finalize = tracker_scraper_finalize;
	object_class->constructed = tracker_scraper_constructed;

	props[PROP_SPARQL_CONNECTION] =
		g_param_spec_object ("sparql-connection", NULL, NULL,
		                     TRACKER_TYPE_SPARQL_CONNECTION,
		                     G_PARAM_WRITABLE |
		                     G_PARAM_CONSTRUCT_ONLY |
		                     G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
tracker_scraper_init (TrackerScraper *scraper)
{
	scraper->cancellable = g_cancellable_new ();
}

TrackerScraper *
tracker_scraper_new (TrackerSparqlConnection *sparql_conn)
{
	return g_object_new (TRACKER_TYPE_SCRAPER,
	                     "sparql-connection", sparql_conn,
	                     NULL);
}
