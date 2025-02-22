/*
 * Copyright (C) 2014 Carlos Garnacho  <carlosg@gnome.org>
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

#include <string.h>

#include <tracker-common.h>

#include "tracker-decorator.h"

#ifdef HAVE_POSIX_FADVISE
#include <fcntl.h>
#endif

#define DEFAULT_BATCH_SIZE 200
#define THROTTLED_TIMEOUT_MS 10

/**
 * SECTION:tracker-decorator
 * @short_description: A miner tasked with listening for DB resource changes and extracting metadata
 * @include: libtracker-miner/tracker-miner.h
 * @title: TrackerDecorator
 * @see_also: #TrackerDecoratorFS
 *
 * #TrackerDecorator watches for signal updates based on content changes
 * in the database. When new files are added initially, only simple
 * metadata exists, for example, name, size, mtime, etc. The
 * #TrackerDecorator queues files for extended metadata extraction
 * (i.e. for tracker-extract to fetch metadata specific to the file
 * type) for example 'nmm:whiteBalance' for a picture.
**/

typedef struct _TrackerDecoratorInfo TrackerDecoratorInfo;
struct _TrackerDecoratorInfo {
	GTask *task;
	gchar *url;
	gchar *content_id;
	gchar *mime_type;
	gint id;
	gint ref_count;
};

typedef struct _TrackerDecoratorPrivate TrackerDecoratorPrivate;
struct _TrackerDecoratorPrivate {
	TrackerExtract *extractor;
	TrackerExtractPersistence *persistence;
	TrackerNotifier *notifier;

	gssize n_remaining_items;
	gssize n_processed_items;

	TrackerSparqlStatement *update_hash;
	TrackerSparqlStatement *delete_file;

	TrackerSparqlCursor *cursor; /* Results of remaining_items_query */
	TrackerDecoratorInfo *item; /* Pre-extracted info for the next element */
	TrackerDecoratorInfo *next_item; /* Pre-extracted info for the next element */

	GStrv priority_graphs;

	GPtrArray *sparql_buffer; /* Array of TrackerExtractInfo */
	GPtrArray *commit_buffer; /* Array of TrackerExtractInfo */
	GTimer *timer;

	TrackerBatch *batch;
	TrackerSparqlStatement *remaining_items_query;
	TrackerSparqlStatement *item_count_query;

	GCancellable *cancellable;
	GCancellable *task_cancellable;

	gint batch_size;
	guint throttle_id;

	guint throttled  : 1;
	guint updating   : 1;
	guint processing : 1;
	guint querying   : 1;
	guint extracting : 1;
};

enum {
	PROP_0,
	PROP_COMMIT_BATCH_SIZE,
	PROP_EXTRACTOR,
	PROP_PERSISTENCE,
	N_PROPS,
};

static GParamSpec *props[N_PROPS] = { 0, };

enum {
	ITEMS_AVAILABLE,
	FINISHED,
	RAISE_ERROR,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void decorator_maybe_restart_query (TrackerDecorator *decorator);
static gboolean decorator_check_commit (TrackerDecorator *decorator);
static void decorator_get_next_file (TrackerDecorator *decorator);

typedef enum {
	TRACKER_DECORATOR_ERROR_PAUSED,
	TRACKER_DECORATOR_ERROR_INVALID_FILE,
} TrackerDecoratorError;

G_DEFINE_QUARK (TrackerDecoratorError, tracker_decorator_error)
#define TRACKER_DECORATOR_ERROR (tracker_decorator_error_quark ())

G_DEFINE_TYPE_WITH_PRIVATE (TrackerDecorator, tracker_decorator, TRACKER_TYPE_MINER)

static TrackerDecoratorInfo *
tracker_decorator_info_new (TrackerDecorator    *decorator,
                            TrackerSparqlCursor *cursor)
{
	TrackerDecoratorInfo *info;

	info = g_new0 (TrackerDecoratorInfo, 1);
	info->url = g_strdup (tracker_sparql_cursor_get_string (cursor, 0, NULL));
	info->id = tracker_sparql_cursor_get_integer (cursor, 1);
	info->content_id = g_strdup (tracker_sparql_cursor_get_string (cursor, 2, NULL));
	info->mime_type = g_strdup (tracker_sparql_cursor_get_string (cursor, 3, NULL));
	info->ref_count = 1;

	return info;
}

/**
 * tracker_decorator_info_ref:
 * @info: a #TrackerDecoratorInfo
 *
 * Increases the reference count of @info by 1.
 *
 * Returns: the same @info passed in, or %NULL on error.
 *
 * Since: 0.18
 **/
TrackerDecoratorInfo *
tracker_decorator_info_ref (TrackerDecoratorInfo *info)
{
	g_atomic_int_inc (&info->ref_count);
	return info;
}

/**
 * tracker_decorator_info_unref:
 * @info: a #TrackerDecoratorInfo
 *
 * Decreases the reference count of @info by 1 and frees it when the
 * reference count reaches 0.
 *
 * Since: 0.18
 **/
void
tracker_decorator_info_unref (TrackerDecoratorInfo *info)
{
	if (!g_atomic_int_dec_and_test (&info->ref_count))
		return;

	g_clear_object (&info->task);
	g_free (info->url);
	g_free (info->content_id);
	g_free (info->mime_type);
	g_free (info);
}

static void
tracker_decorator_info_complete (TrackerDecoratorInfo *info,
                                 TrackerExtractInfo   *extract_info)
{
	TRACKER_NOTE (DECORATOR, g_message ("[Decorator] Task for %s completed successfully", info->url));
	g_task_return_pointer (info->task,
	                       tracker_extract_info_ref (extract_info),
	                       (GDestroyNotify) tracker_extract_info_unref);
}

static void
tracker_decorator_info_complete_error (TrackerDecoratorInfo *info,
                                       GError               *error)
{
	TRACKER_NOTE (DECORATOR, g_message ("[Decorator] Task for %s failed: %s", info->url, error->message));
	g_task_return_error (info->task, error);
}

static void
hint_file_needed (GFile    *file,
                  gboolean  needed)
{
#ifdef HAVE_POSIX_FADVISE
	g_autofree gchar *path = NULL;
	int fd;

	path = g_file_get_path (file);
	if (!path)
		return;

	fd = tracker_file_open_fd (path);
	posix_fadvise (fd, 0, 0,
	               needed ? POSIX_FADV_WILLNEED : POSIX_FADV_DONTNEED);
	close (fd);
#endif /* HAVE_POSIX_FADVISE */
}

static void
tracker_decorator_info_hint_needed (TrackerDecoratorInfo *info,
                                    gboolean              needed)
{
	g_autoptr (GFile) file = NULL;

	file = g_file_new_for_uri (info->url);
	hint_file_needed (file, needed);
}

static void
decorator_update_state (TrackerDecorator *decorator,
                        const gchar      *message,
                        gboolean          estimate_time)
{
	TrackerDecoratorPrivate *priv;
	gint remaining_time = -1;
	gdouble progress = 1;
	gsize total_items;

	priv = tracker_decorator_get_instance_private (decorator);
	remaining_time = 0;
	total_items = priv->n_remaining_items + priv->n_processed_items;

	if (priv->n_remaining_items > 0)
		progress = ((gdouble) priv->n_processed_items / total_items);

	if (priv->timer && estimate_time &&
	    !tracker_miner_is_paused (TRACKER_MINER (decorator))) {
		gdouble elapsed;

		/* FIXME: Quite naive calculation */
		elapsed = g_timer_elapsed (priv->timer, NULL);

		if (priv->n_processed_items > 0)
			remaining_time = (priv->n_remaining_items * elapsed) / priv->n_processed_items;
	}

	g_object_set (decorator,
	              "progress", progress,
	              "remaining-time", remaining_time,
	              NULL);

	if (message)
		g_object_set (decorator, "status", message, NULL);
}

static TrackerBatch *
tracker_decorator_get_batch (TrackerDecorator *decorator)
{
	TrackerDecoratorPrivate *priv =
		tracker_decorator_get_instance_private (decorator);

	if (!priv->batch) {
		TrackerSparqlConnection *conn;

		conn = tracker_miner_get_connection (TRACKER_MINER (decorator));
		priv->batch = tracker_sparql_connection_create_batch (conn);
	}

	return priv->batch;
}

static void
tracker_decorator_items_available (TrackerDecorator *decorator)
{
	TrackerDecoratorPrivate *priv =
		tracker_decorator_get_instance_private (decorator);

	g_debug ("Starting to process %ld items", priv->n_remaining_items);
	decorator_get_next_file (decorator);
}

static void
tracker_decorator_update (TrackerDecorator   *decorator,
                          TrackerExtractInfo *info)
{
	TrackerDecoratorPrivate *priv =
		tracker_decorator_get_instance_private (decorator);
	TrackerResource *resource;
	const gchar *graph, *mime_type, *hash;
	g_autofree gchar *uri = NULL;
	TrackerBatch *batch;
	GFile *file;

	batch = tracker_decorator_get_batch (decorator);

	mime_type = tracker_extract_info_get_mimetype (info);
	hash = tracker_extract_module_manager_get_hash (mime_type);
	graph = tracker_extract_info_get_graph (info);
	resource = tracker_extract_info_get_resource (info);
	file = tracker_extract_info_get_file (info);
	uri = g_file_get_uri (file);

	tracker_batch_add_statement (batch,
	                             priv->update_hash,
	                             "file", G_TYPE_STRING, uri,
	                             "hash", G_TYPE_STRING, hash,
	                             NULL);

	if (resource)
		tracker_batch_add_resource (batch, graph, resource);
}

static void
tracker_decorator_raise_error (TrackerDecorator *decorator,
                               GFile            *file,
                               const char       *message,
                               const char       *extra_info)
{
	g_signal_emit (decorator, signals[RAISE_ERROR], 0,
	               file, message, extra_info);
}

static void
tracker_decorator_error (TrackerDecorator   *decorator,
                         GFile              *file,
                         const gchar        *error_message,
                         const gchar        *extra_info)
{
	TrackerDecoratorPrivate *priv =
		tracker_decorator_get_instance_private (decorator);
	g_autofree gchar *uri = NULL;
	g_autoptr (GFileInfo) info = NULL;
	TrackerBatch *batch;
	const gchar *hash = NULL;

	uri = g_file_get_uri (file);
	g_debug ("Extraction on file '%s' failed in previous execution, ignoring", uri);

	info = g_file_query_info (file,
	                          G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
	                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
	                          NULL, NULL);

	if (info) {
		const gchar *mimetype;

		mimetype = g_file_info_get_attribute_string (info,
		                                             G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE);
		hash = tracker_extract_module_manager_get_hash (mimetype);
	}

	batch = tracker_decorator_get_batch (decorator);

	if (hash) {
		tracker_batch_add_statement (batch,
		                             priv->update_hash,
		                             "file", G_TYPE_STRING, uri,
		                             "hash", G_TYPE_STRING, hash,
		                             NULL);
	} else {
		tracker_batch_add_statement (batch,
		                             priv->delete_file,
		                             "file", G_TYPE_STRING, uri,
		                             NULL);
	}
}

static void
retry_synchronously (TrackerDecorator *decorator,
                     GPtrArray        *commit_buffer)
{
	TrackerDecoratorPrivate *priv =
		tracker_decorator_get_instance_private (decorator);
	guint i;

	for (i = 0; i < commit_buffer->len; i++) {
		g_autoptr (TrackerBatch) batch = NULL;
		TrackerExtractInfo *info;
		g_autoptr (GError) error = NULL;

		info = g_ptr_array_index (commit_buffer, i);

		TRACKER_DECORATOR_GET_CLASS (decorator)->update (decorator, info);

		batch = g_steal_pointer (&priv->batch);

		if (batch)
			tracker_batch_execute (batch, NULL, &error);

		if (error) {
			g_autofree gchar *sparql = NULL;
			g_autoptr (GError) inner_error = NULL;
			TrackerResource *resource;
			const gchar *graph;
			GFile *file;

			/* This is a SPARQL/ontology error, set the SPARQL query
			 * as the the extra information.
			 */
			file = tracker_extract_info_get_file (info);
			graph = tracker_extract_info_get_graph (info);
			resource = tracker_extract_info_get_resource (info);

			sparql = tracker_resource_print_sparql_update (resource,
			                                               NULL,
			                                               graph);

			tracker_decorator_raise_error (decorator, file,
			                               error->message, sparql);

			batch = g_steal_pointer (&priv->batch);
			if (batch)
				tracker_batch_execute (batch, NULL, &inner_error);

			if (inner_error) {
				g_autofree char *uri = NULL;

				uri = g_file_get_uri (file);
				g_warning ("Could not handle error on '%s': %s",
				           uri, inner_error->message);
			}
		}
	}
}

static void
decorator_commit_cb (GObject      *object,
                     GAsyncResult *result,
                     gpointer      user_data)
{
	TrackerBatch *batch = TRACKER_BATCH (object);
	TrackerDecorator *decorator = user_data;
	TrackerDecoratorPrivate *priv;
	g_autoptr (GError) error = NULL;

	tracker_batch_execute_finish (batch, result, &error);

	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		return;

	priv = tracker_decorator_get_instance_private (decorator);
	priv->updating = FALSE;

	if (error) {
		g_debug ("SPARQL error detected in batch, retrying one by one");
		retry_synchronously (decorator, priv->commit_buffer);
	}

	g_clear_pointer (&priv->commit_buffer, g_ptr_array_unref);
	decorator_check_commit (decorator);
}

static gboolean
decorator_commit_info (TrackerDecorator *decorator)
{
	TrackerDecoratorPrivate *priv;
	g_autoptr (TrackerBatch) batch = NULL;

	priv = tracker_decorator_get_instance_private (decorator);

	if (!priv->batch)
		return FALSE;
	if (priv->commit_buffer)
		return FALSE;

	/* Move sparql buffer to commit buffer */
	priv->commit_buffer = priv->sparql_buffer;
	priv->sparql_buffer = NULL;
	priv->updating = TRUE;

	batch = g_steal_pointer (&priv->batch);

	if (batch) {
		tracker_batch_execute_async (batch,
		                             priv->cancellable,
		                             decorator_commit_cb,
		                             decorator);
		decorator_update_state (decorator, NULL, TRUE);
	}

	return TRUE;
}

static gboolean
decorator_check_commit (TrackerDecorator *decorator)
{
	TrackerDecoratorPrivate *priv;

	priv = tracker_decorator_get_instance_private (decorator);

	if (!priv->sparql_buffer ||
	    (priv->n_remaining_items > 0 &&
	     priv->sparql_buffer->len < (guint) priv->batch_size))
		return FALSE;

	return decorator_commit_info (decorator);
}

static void
decorator_start (TrackerDecorator *decorator)
{
	TrackerDecoratorPrivate *priv;

	priv = tracker_decorator_get_instance_private (decorator);

	if (priv->processing)
		return;

	priv->processing = TRUE;
	g_signal_emit (decorator, signals[ITEMS_AVAILABLE], 0);
	decorator_update_state (decorator, "Extracting metadata", TRUE);
}

static void
decorator_finish (TrackerDecorator *decorator)
{
	TrackerDecoratorPrivate *priv;

	priv = tracker_decorator_get_instance_private (decorator);

	priv->processing = FALSE;
	priv->n_remaining_items = priv->n_processed_items = 0;
	g_signal_emit (decorator, signals[FINISHED], 0);
	decorator_commit_info (decorator);
	decorator_update_state (decorator, "Idle", FALSE);
}

static void
decorator_clear_cache (TrackerDecorator *decorator)
{
	TrackerDecoratorPrivate *priv =
		tracker_decorator_get_instance_private (decorator);

	priv->n_remaining_items = 0;
	g_clear_pointer (&priv->next_item, tracker_decorator_info_unref);

	if (priv->cursor) {
		tracker_sparql_cursor_close (priv->cursor);
		g_clear_object (&priv->cursor);
	}
}

static void
decorator_rebuild_cache (TrackerDecorator *decorator)
{
	decorator_clear_cache (decorator);
	decorator_maybe_restart_query (decorator);
}

/* This function is called after the caller has completed the
 * GTask given on the TrackerDecoratorInfo, this definitely removes
 * the element being processed from queues.
 */
static void
decorator_task_done (GObject      *object,
                     GAsyncResult *result,
                     gpointer      user_data)
{
	TrackerDecorator *decorator = TRACKER_DECORATOR (object);
	TrackerDecoratorInfo *info = user_data;
	TrackerDecoratorPrivate *priv;
	TrackerExtractInfo *extract_info;
	g_autoptr (GError) error = NULL;

	extract_info = g_task_propagate_pointer (G_TASK (result), &error);

	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		return;

	priv = tracker_decorator_get_instance_private (decorator);
	g_assert (info == priv->item);
	tracker_decorator_info_hint_needed (info, FALSE);

	if (error) {
		g_autoptr (GFile) file = NULL;

		g_warning ("Task for '%s' finished with error: %s\n",
		           info->url, error->message);

		file = g_file_new_for_uri (info->url);
		tracker_decorator_raise_error (decorator, file,
		                               error->message, NULL);
	} else {
		if (!priv->sparql_buffer) {
			priv->sparql_buffer =
				g_ptr_array_new_with_free_func ((GDestroyNotify) tracker_extract_info_unref);
		}

		g_ptr_array_add (priv->sparql_buffer, extract_info);

		TRACKER_DECORATOR_GET_CLASS (decorator)->update (decorator,
		                                                 extract_info);
	}

	g_clear_pointer (&priv->item, tracker_decorator_info_unref);

	if (priv->n_remaining_items > 0)
		priv->n_remaining_items--;
	priv->n_processed_items++;

	decorator_check_commit (decorator);

	if (!priv->next_item) {
		decorator_finish (decorator);
		if (!priv->updating)
			decorator_rebuild_cache (decorator);
	}
}

static TrackerSparqlStatement *
load_statement (TrackerDecorator *decorator,
                const gchar      *query_filename)
{
	g_autofree gchar *resource_path = NULL;
	TrackerSparqlConnection *conn;

	resource_path =
		g_strconcat ("/org/freedesktop/Tracker3/Extract/queries/",
		             query_filename, NULL);

	conn = tracker_miner_get_connection (TRACKER_MINER (decorator));

	return tracker_sparql_connection_load_statement_from_gresource (conn,
	                                                                resource_path,
	                                                                NULL,
	                                                                NULL);
}

static void
decorator_cache_next_item (TrackerDecorator *decorator)
{
	TrackerDecoratorPrivate *priv =
		tracker_decorator_get_instance_private (decorator);

	if (!priv->cursor)
		return;

	if (tracker_sparql_cursor_next (priv->cursor, NULL, NULL)) {
		priv->next_item = tracker_decorator_info_new (decorator,
		                                              priv->cursor);
		tracker_decorator_info_hint_needed (priv->next_item, TRUE);
	} else {
		decorator_clear_cache (decorator);
	}
}

static void
query_items_cb (GObject      *object,
                GAsyncResult *result,
                gpointer      user_data)
{
	TrackerDecorator *decorator = user_data;
	TrackerDecoratorPrivate *priv =
		tracker_decorator_get_instance_private (decorator);
	g_autoptr (TrackerSparqlCursor) cursor = NULL;
	g_autoptr (GError) error = NULL;
	gboolean had_cursor;

	priv->querying = FALSE;
	had_cursor = priv->cursor != NULL;
	g_clear_object (&priv->cursor);

	cursor = tracker_sparql_statement_execute_finish (TRACKER_SPARQL_STATEMENT (object),
	                                                  result, &error);

	if (error) {
		g_warning ("Could not get unextracted files: %s", error->message);
		return;
	}

	g_set_object (&priv->cursor, cursor);
	decorator_cache_next_item (decorator);

	if (priv->next_item && !priv->processing) {
		decorator_start (decorator);
	} else if (!priv->next_item) {
		decorator_finish (decorator);
	} else if (!had_cursor) {
		g_signal_emit (decorator, signals[ITEMS_AVAILABLE], 0);
	}
}

static void
bind_graph_limits (TrackerDecorator *decorator,
                   gboolean          priority)
{
	TrackerDecoratorPrivate *priv;
	const gchar *graphs[][3] = {
		{ "tracker:Audio", "audioHigh", "audioLow" },
		{ "tracker:Pictures", "picturesHigh", "picturesLow" },
		{ "tracker:Video", "videoHigh", "videoLow" },
		{ "tracker:Software", "softwareHigh", "softwareLow" },
		{ "tracker:Documents", "documentsHigh", "documentsLow" },
	};
	guint i;

	priv = tracker_decorator_get_instance_private (decorator);

	for (i = 0; i < G_N_ELEMENTS (graphs); i++) {
		const gchar *graph = graphs[i][0];
		const gchar *high_limit = graphs[i][1];
		const gchar *low_limit = graphs[i][2];
		gboolean is_priority;

		is_priority = priv->priority_graphs &&
			g_strv_contains ((const gchar * const *) priv->priority_graphs, graph);

		/* Graphs with high priority get unbound high limit and 0 low limit,
		 * graphs with regular priority get the opposite.
		 */
		tracker_sparql_statement_bind_int (priv->remaining_items_query,
		                                   high_limit,
		                                   is_priority ? -1 : 0);
		tracker_sparql_statement_bind_int (priv->remaining_items_query,
		                                   low_limit,
		                                   is_priority ? 0 : -1);
	}
}

static void
decorator_query_items (TrackerDecorator *decorator)
{
	TrackerDecoratorPrivate *priv;

	priv = tracker_decorator_get_instance_private (decorator);

	if (!priv->remaining_items_query)
		priv->remaining_items_query = load_statement (decorator, "get-items.rq");

	priv->querying = TRUE;
	bind_graph_limits (decorator, TRUE);
	tracker_sparql_statement_execute_async (priv->remaining_items_query,
	                                        priv->cancellable,
	                                        query_items_cb,
	                                        decorator);
}

static void
count_remaining_items_cb (GObject      *object,
                          GAsyncResult *result,
                          gpointer      user_data)
{
	TrackerDecorator *decorator = user_data;
	TrackerDecoratorPrivate *priv =
		tracker_decorator_get_instance_private (decorator);
	g_autoptr (TrackerSparqlCursor) cursor = NULL;
	g_autoptr (GError) error = NULL;

	priv->querying = FALSE;
	cursor = tracker_sparql_statement_execute_finish (TRACKER_SPARQL_STATEMENT (object),
	                                                  result, &error);

	if (error) {
		g_warning ("Could not get remaining item count: %s", error->message);
		return;
	}

	if (!tracker_sparql_cursor_next (cursor, NULL, NULL))
		return;

	priv->n_remaining_items = tracker_sparql_cursor_get_integer (cursor, 0);

	TRACKER_NOTE (DECORATOR, g_message ("[Decorator] Found %" G_GSIZE_FORMAT " items to extract", priv->n_remaining_items));

	if (priv->n_remaining_items > 0) {
		decorator_query_items (decorator);
	} else {
		decorator_finish (decorator);
	}
}

static void
decorator_maybe_restart_query (TrackerDecorator *decorator)
{
	TrackerDecoratorPrivate *priv;

	priv = tracker_decorator_get_instance_private (decorator);

	if (priv->querying ||
	    priv->updating ||
	    priv->next_item)
		return;

	priv->querying = TRUE;

	TRACKER_NOTE (DECORATOR, g_message ("[Decorator] Counting items which still need processing"));

	if (!priv->item_count_query)
		priv->item_count_query = load_statement (decorator, "get-item-count.rq");

	tracker_sparql_statement_execute_async (priv->item_count_query,
	                                        priv->cancellable,
	                                        count_remaining_items_cb,
	                                        decorator);
}

static void
ensure_data (TrackerExtractInfo *info)
{
	TrackerResource *resource, *dataobject;
	GStrv rdf_types;
	const gchar *mimetype;
	g_autofree gchar *uri = NULL;
	GFile *file;
	gint i;

	resource = tracker_extract_info_get_resource (info);
	if (!resource)
		return;

	mimetype = tracker_extract_info_get_mimetype (info);
	file = tracker_extract_info_get_file (info);
	uri = g_file_get_uri (file);

	dataobject = tracker_resource_new (uri);
	tracker_resource_set_string (resource, "nie:mimeType", mimetype);
	tracker_resource_add_take_relation (resource, "nie:isStoredAs", dataobject);
	tracker_resource_add_uri (dataobject, "nie:interpretedAs",
	                          tracker_resource_get_identifier (resource));

	rdf_types = tracker_extract_module_manager_get_rdf_types (mimetype);

	for (i = 0; rdf_types[i] != NULL; i++)
		tracker_resource_add_uri (resource, "rdf:type", rdf_types[i]);

	g_strfreev (rdf_types);
}

static gboolean
throttle_next_item_cb (gpointer user_data)
{
	TrackerDecorator *decorator = user_data;
	TrackerDecoratorPrivate *priv =
		tracker_decorator_get_instance_private (decorator);

	priv->throttle_id = 0;
	decorator_get_next_file (decorator);

	return G_SOURCE_REMOVE;
}

static void
throttle_next_item (TrackerDecorator *decorator)
{
	TrackerDecoratorPrivate *priv =
		tracker_decorator_get_instance_private (decorator);

	if (priv->throttled) {
		priv->throttle_id =
			g_timeout_add (THROTTLED_TIMEOUT_MS,
				       throttle_next_item_cb,
				       decorator);
	} else {
		priv->throttle_id =
			g_idle_add (throttle_next_item_cb,
				    decorator);
	}
}

static void
get_metadata_cb (TrackerExtract   *extract,
                 GAsyncResult     *result,
                 TrackerDecorator *decorator)
{
	TrackerDecoratorPrivate *priv =
		tracker_decorator_get_instance_private (decorator);
	g_autoptr (TrackerExtractInfo) info = NULL;
	GError *error = NULL;

	info = tracker_extract_file_finish (extract, result, &error);

	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		return;

	tracker_extract_persistence_set_file (priv->persistence, NULL);

	if (error) {
		tracker_decorator_info_complete_error (priv->item, error);
	} else {
		ensure_data (info);
		tracker_decorator_info_complete (priv->item, info);
	}

	priv->extracting = FALSE;

	throttle_next_item (decorator);
}

static TrackerDecoratorInfo *
tracker_decorator_next (TrackerDecorator  *decorator,
                        GError           **error)
{
	TrackerDecoratorPrivate *priv;

	g_return_val_if_fail (TRACKER_IS_DECORATOR (decorator), NULL);

	priv = tracker_decorator_get_instance_private (decorator);

	if (tracker_miner_is_paused (TRACKER_MINER (decorator))) {
		g_set_error (error,
		             tracker_decorator_error_quark (),
		             TRACKER_DECORATOR_ERROR_PAUSED,
		             "Decorator is paused");
		return NULL;
	}

	priv->item = g_steal_pointer (&priv->next_item);

	if (priv->item) {
		TRACKER_NOTE (DECORATOR, g_message ("[Decorator] Next item %s",
		                                    priv->item->url));
		priv->item->task =
			g_task_new (decorator,
			            priv->task_cancellable,
			            decorator_task_done,
			            priv->item);
	}

	/* Preempt next item */
	decorator_cache_next_item (decorator);

	return priv->item;
}

static void
decorator_get_next_file (TrackerDecorator *decorator)
{
	TrackerDecoratorPrivate *priv =
		tracker_decorator_get_instance_private (decorator);
	TrackerDecoratorInfo *info;
	g_autoptr (GError) error = NULL;
	g_autoptr (GFile) file = NULL;

	if (!tracker_miner_is_started (TRACKER_MINER (decorator)) ||
	    tracker_miner_is_paused (TRACKER_MINER (decorator)))
		return;

	if (priv->extracting)
		return;

	info = tracker_decorator_next (decorator, &error);

	if (!info) {
		if (error &&
		    g_error_matches (error,
		                     TRACKER_DECORATOR_ERROR,
		                     TRACKER_DECORATOR_ERROR_PAUSED)) {
			g_debug ("Next item is on hold because miner is paused");
		} else if (error) {
			g_warning ("Next item could not be processed, %s", error->message);
		}

		g_clear_error (&error);
		return;
	}

	file = g_file_new_for_uri (info->url);

	if (!g_file_is_native (file)) {
		error = g_error_new (TRACKER_DECORATOR_ERROR,
		                     TRACKER_DECORATOR_ERROR_INVALID_FILE,
		                     "URI '%s' is not native",
		                     info->url);
		tracker_decorator_info_complete_error (info, error);
		decorator_get_next_file (decorator);
		return;
	}

	priv->extracting = TRUE;

	TRACKER_NOTE (DECORATOR,
	              g_message ("[Decorator] Extracting metadata for '%s'",
	                         info->url));

	tracker_extract_persistence_set_file (priv->persistence, file);

	tracker_extract_file (priv->extractor,
	                      info->url,
	                      info->content_id,
	                      info->mime_type,
	                      priv->task_cancellable,
	                      (GAsyncReadyCallback) get_metadata_cb,
	                      decorator);
}

static void
tracker_decorator_set_property (GObject      *object,
                                guint         param_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
	TrackerDecorator *decorator  = TRACKER_DECORATOR (object);
	TrackerDecoratorPrivate *priv =
		tracker_decorator_get_instance_private (decorator);

	switch (param_id) {
	case PROP_COMMIT_BATCH_SIZE:
		priv->batch_size = g_value_get_int (value);
		break;
	case PROP_EXTRACTOR:
		priv->extractor = g_value_dup_object (value);
		break;
	case PROP_PERSISTENCE:
		priv->persistence = g_value_dup_object (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
	}
}

static void
notifier_events_cb (TrackerDecorator *decorator,
                    const gchar      *service,
                    const gchar      *graph,
                    GPtrArray        *events,
                    TrackerNotifier  *notifier)
{
	gboolean added = FALSE, deleted = FALSE;
	gint i;

	for (i = 0; i < events->len; i++) {
		TrackerNotifierEvent *event;

		event = g_ptr_array_index (events, i);

		switch (tracker_notifier_event_get_event_type (event)) {
		case TRACKER_NOTIFIER_EVENT_CREATE:
		case TRACKER_NOTIFIER_EVENT_UPDATE:
			added = TRUE;
			break;
		case TRACKER_NOTIFIER_EVENT_DELETE:
			deleted = TRUE;
			break;
		}
	}

	if (deleted)
		decorator_rebuild_cache (decorator);
	else if (added)
		decorator_maybe_restart_query (decorator);
}

static void
tracker_decorator_constructed (GObject *object)
{
	TrackerDecorator *decorator;
	TrackerDecoratorPrivate *priv;
	TrackerSparqlConnection *conn;

	G_OBJECT_CLASS (tracker_decorator_parent_class)->constructed (object);

	decorator = TRACKER_DECORATOR (object);
	priv = tracker_decorator_get_instance_private (decorator);

	conn = tracker_miner_get_connection (TRACKER_MINER (decorator));
	priv->notifier = tracker_sparql_connection_create_notifier (conn);
	g_signal_connect_swapped (priv->notifier, "events",
	                          G_CALLBACK (notifier_events_cb),
	                          decorator);

	priv->update_hash = load_statement (decorator, "update-hash.rq");
	priv->delete_file = load_statement (decorator, "delete-file.rq");

	decorator_update_state (decorator, "Idle", FALSE);
}

static void
tracker_decorator_finalize (GObject *object)
{
	TrackerDecoratorPrivate *priv;
	TrackerDecorator *decorator;

	decorator = TRACKER_DECORATOR (object);
	priv = tracker_decorator_get_instance_private (decorator);

	g_clear_object (&priv->remaining_items_query);
	g_clear_object (&priv->item_count_query);
	g_clear_object (&priv->update_hash);
	g_clear_object (&priv->delete_file);
	g_strfreev (priv->priority_graphs);

	g_clear_handle_id (&priv->throttle_id, g_source_remove);
	g_clear_object (&priv->extractor);
	g_clear_object (&priv->persistence);

	g_cancellable_cancel (priv->cancellable);
	g_clear_object (&priv->cancellable);

	g_cancellable_cancel (priv->task_cancellable);
	g_clear_object (&priv->task_cancellable);

	g_clear_object (&priv->notifier);

	g_clear_object (&priv->cursor);
	g_clear_pointer (&priv->item, tracker_decorator_info_unref);
	g_clear_pointer (&priv->next_item, tracker_decorator_info_unref);

	g_clear_object (&priv->batch);

	g_clear_pointer (&priv->sparql_buffer, g_ptr_array_unref);
	g_clear_pointer (&priv->commit_buffer, g_ptr_array_unref);
	g_timer_destroy (priv->timer);

	G_OBJECT_CLASS (tracker_decorator_parent_class)->finalize (object);
}

static void
tracker_decorator_paused (TrackerMiner *miner)
{
	TrackerDecorator *decorator = TRACKER_DECORATOR (miner);
	TrackerDecoratorPrivate *priv =
		tracker_decorator_get_instance_private (decorator);

	TRACKER_NOTE (DECORATOR, g_message ("[Decorator] Paused"));
	g_cancellable_cancel (priv->task_cancellable);
	g_set_object (&priv->task_cancellable, g_cancellable_new ());
	g_clear_handle_id (&priv->throttle_id, g_source_remove);
	decorator_clear_cache (decorator);
	g_timer_stop (priv->timer);
}

static void
tracker_decorator_resumed (TrackerMiner *miner)
{
	TrackerDecorator *decorator = TRACKER_DECORATOR (miner);
	TrackerDecoratorPrivate *priv =
		tracker_decorator_get_instance_private (decorator);

	TRACKER_NOTE (DECORATOR, g_message ("[Decorator] Resumed"));
	decorator_rebuild_cache (decorator);
	g_timer_continue (priv->timer);
}

static void
tracker_decorator_stopped (TrackerMiner *miner)
{
	TrackerDecorator *decorator = TRACKER_DECORATOR (miner);
	TrackerDecoratorPrivate *priv =
		tracker_decorator_get_instance_private (decorator);

	TRACKER_NOTE (DECORATOR, g_message ("[Decorator] Stopped"));
	decorator_clear_cache (decorator);
	g_timer_stop (priv->timer);
}

static void
tracker_decorator_started (TrackerMiner *miner)
{
	TrackerDecorator *decorator = TRACKER_DECORATOR (miner);
	TrackerDecoratorPrivate *priv =
		tracker_decorator_get_instance_private (decorator);
	GFile *file;

	TRACKER_NOTE (DECORATOR, g_message ("[Decorator] Started"));

	file = tracker_extract_persistence_get_file (priv->persistence);

	if (file) {
		tracker_decorator_raise_error (TRACKER_DECORATOR (miner), file,
		                               "Crash/hang handling file", NULL);
		decorator_commit_info (decorator);
	}

	decorator_rebuild_cache (decorator);
	g_timer_start (priv->timer);
}

static void
tracker_decorator_class_init (TrackerDecoratorClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	TrackerMinerClass *miner_class = TRACKER_MINER_CLASS (klass);

	object_class->constructed = tracker_decorator_constructed;
	object_class->set_property = tracker_decorator_set_property;
	object_class->finalize = tracker_decorator_finalize;

	miner_class->paused = tracker_decorator_paused;
	miner_class->resumed = tracker_decorator_resumed;
	miner_class->started = tracker_decorator_started;
	miner_class->stopped = tracker_decorator_stopped;

	klass->items_available = tracker_decorator_items_available;
	klass->update = tracker_decorator_update;
	klass->error = tracker_decorator_error;

	props[PROP_COMMIT_BATCH_SIZE] =
		g_param_spec_int ("commit-batch-size",
		                  "Commit batch size",
		                  "Number of items per update batch",
		                  0, G_MAXINT, DEFAULT_BATCH_SIZE,
		                  G_PARAM_WRITABLE |
		                  G_PARAM_CONSTRUCT_ONLY |
		                  G_PARAM_STATIC_STRINGS);
	props[PROP_EXTRACTOR] =
		g_param_spec_object ("extractor", NULL, NULL,
		                     TRACKER_TYPE_EXTRACT,
		                     G_PARAM_WRITABLE |
		                     G_PARAM_CONSTRUCT_ONLY |
		                     G_PARAM_STATIC_STRINGS);
	props[PROP_PERSISTENCE] =
		g_param_spec_object ("persistence",
		                     NULL, NULL,
		                     TRACKER_TYPE_EXTRACT_PERSISTENCE,
		                     G_PARAM_WRITABLE |
		                     G_PARAM_CONSTRUCT_ONLY |
		                     G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties (object_class, N_PROPS, props);

	/**
	 * TrackerDecorator::items-available:
	 * @decorator: the #TrackerDecorator
	 *
	 * The ::items-available signal will be emitted whenever the
	 * #TrackerDecorator sees resources that are available for
	 * extended metadata extraction.
	 *
	 * Since: 0.18
	 **/
	signals[ITEMS_AVAILABLE] =
		g_signal_new ("items-available",
		              G_OBJECT_CLASS_TYPE (object_class),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (TrackerDecoratorClass,
		                               items_available),
		              NULL, NULL, NULL,
		              G_TYPE_NONE, 0);
	/**
	 * TrackerDecorator::finished:
	 * @decorator: the #TrackerDecorator
	 *
	 * The ::finished signal will be emitted whenever the
	 * #TrackerDecorator has finished extracted extended metadata
	 * for resources in the database.
	 *
	 * Since: 0.18
	 **/
	signals[FINISHED] =
		g_signal_new ("finished",
		              G_OBJECT_CLASS_TYPE (object_class),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (TrackerDecoratorClass, finished),
		              NULL, NULL, NULL,
		              G_TYPE_NONE, 0);

	signals[RAISE_ERROR] =
		g_signal_new ("raise-error",
		              G_OBJECT_CLASS_TYPE (object_class),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (TrackerDecoratorClass,
		                               error),
		              NULL, NULL, NULL,
		              G_TYPE_NONE, 3,
			      G_TYPE_FILE,
			      G_TYPE_STRING,
			      G_TYPE_STRING);
}

static void
tracker_decorator_init (TrackerDecorator *decorator)
{
	TrackerDecoratorPrivate *priv;

	priv = tracker_decorator_get_instance_private (decorator);
	priv->batch_size = DEFAULT_BATCH_SIZE;
	priv->timer = g_timer_new ();
	priv->cancellable = g_cancellable_new ();
	priv->task_cancellable = g_cancellable_new ();
}

TrackerDecorator *
tracker_decorator_new (TrackerSparqlConnection   *connection,
                       TrackerExtract            *extract,
                       TrackerExtractPersistence *persistence)
{
	return g_object_new (TRACKER_TYPE_DECORATOR,
			     "connection", connection,
			     "extractor", extract,
	                     "persistence", persistence,
			     NULL);
}

void
tracker_decorator_set_priority_graphs (TrackerDecorator    *decorator,
                                       const gchar * const *graphs)
{
	TrackerDecoratorPrivate *priv;

	priv = tracker_decorator_get_instance_private (decorator);

	g_strfreev (priv->priority_graphs);
	priv->priority_graphs = g_strdupv ((gchar **) graphs);
	decorator_rebuild_cache (decorator);
}

void
tracker_decorator_set_throttled (TrackerDecorator *decorator,
                                 gboolean          throttled)
{
	TrackerDecoratorPrivate *priv =
		tracker_decorator_get_instance_private (decorator);

	priv->throttled = !!throttled;
}
