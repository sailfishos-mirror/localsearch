/*
 * Copyright (C) 2011, Nokia <ivan.frade@nokia.com>
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
 * Author: Carlos Garnacho <carlos@lanedo.com>
 */

#include "config-miners.h"

#include "tracker-sparql-buffer.h"

#include <tracker-common.h>

#include "tracker-utils.h"

#define DEFAULT_GRAPH "tracker:FileSystem"

typedef struct _SparqlTaskData SparqlTaskData;
typedef struct _UpdateBatchData UpdateBatchData;

enum {
	PROP_0,
	PROP_CONNECTION,
	PROP_ROOT,
	PROP_LIMIT,
	N_PROPS,
};

static GParamSpec *props[N_PROPS] = { 0, };

struct _TrackerSparqlBuffer
{
	GObject parent_instance;

	TrackerSparqlConnection *connection;
	GPtrArray *tasks;
	gint n_updates;
	unsigned int limit;
	TrackerBatch *batch;

	TrackerSparqlStatement *delete_file;
	TrackerSparqlStatement *delete_file_content;
	TrackerSparqlStatement *delete_content;
	TrackerSparqlStatement *move_file;
	TrackerSparqlStatement *move_content;

	GFile *root;
};

enum {
	TASK_TYPE_RESOURCE,
	TASK_TYPE_STMT,
};

struct _SparqlTaskData
{
	guint type;
	GFile *file;

	union {
		struct {
			gchar *graph;
			TrackerResource *resource;
		} resource;
		struct {
			TrackerSparqlStatement *stmt;
		} stmt;
	} d;
};

struct _UpdateBatchData {
	TrackerSparqlBuffer *buffer;
	GPtrArray *tasks;
	TrackerBatch *batch;
};

static void sparql_task_data_free (SparqlTaskData *data);

G_DEFINE_TYPE (TrackerSparqlBuffer, tracker_sparql_buffer, G_TYPE_OBJECT)

static void
tracker_sparql_buffer_finalize (GObject *object)
{
	TrackerSparqlBuffer *sparql_buffer = TRACKER_SPARQL_BUFFER (object);

	g_object_unref (sparql_buffer->delete_file);
	g_object_unref (sparql_buffer->delete_file_content);
	g_object_unref (sparql_buffer->delete_content);
	g_object_unref (sparql_buffer->move_file);
	g_object_unref (sparql_buffer->move_content);
	g_object_unref (sparql_buffer->connection);
	g_clear_object (&sparql_buffer->root);

	G_OBJECT_CLASS (tracker_sparql_buffer_parent_class)->finalize (object);
}

static void
tracker_sparql_buffer_set_property (GObject      *object,
                                    guint         param_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
	TrackerSparqlBuffer *sparql_buffer = TRACKER_SPARQL_BUFFER (object);

	switch (param_id) {
	case PROP_CONNECTION:
		sparql_buffer->connection = g_value_dup_object (value);
		break;
	case PROP_ROOT:
		sparql_buffer->root = g_value_dup_object (value);
		break;
	case PROP_LIMIT:
		sparql_buffer->limit = g_value_get_uint (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
		break;
	}
}

static void
tracker_sparql_buffer_constructed (GObject *object)
{
	TrackerSparqlBuffer *sparql_buffer = TRACKER_SPARQL_BUFFER (object);

	sparql_buffer->delete_file =
		tracker_load_statement (sparql_buffer->connection, "delete-file.rq", NULL);
	sparql_buffer->delete_file_content =
		tracker_load_statement (sparql_buffer->connection, "delete-file-content.rq", NULL);
	sparql_buffer->delete_content =
		tracker_load_statement (sparql_buffer->connection, "delete-folder-contents.rq", NULL);
	sparql_buffer->move_file =
		tracker_load_statement (sparql_buffer->connection, "move-file.rq", NULL);
	sparql_buffer->move_content =
		tracker_load_statement (sparql_buffer->connection, "move-folder-contents.rq", NULL);

	G_OBJECT_CLASS (tracker_sparql_buffer_parent_class)->constructed (object);
}

static void
tracker_sparql_buffer_class_init (TrackerSparqlBufferClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = tracker_sparql_buffer_finalize;
	object_class->set_property = tracker_sparql_buffer_set_property;
	object_class->constructed = tracker_sparql_buffer_constructed;

	props[PROP_CONNECTION] =
		g_param_spec_object ("connection", NULL, NULL,
		                     TRACKER_SPARQL_TYPE_CONNECTION,
		                     G_PARAM_WRITABLE |
		                     G_PARAM_CONSTRUCT_ONLY |
		                     G_PARAM_STATIC_STRINGS);
	props[PROP_ROOT] =
		g_param_spec_object ("root", NULL, NULL,
		                     G_TYPE_FILE,
		                     G_PARAM_WRITABLE |
		                     G_PARAM_CONSTRUCT_ONLY |
		                     G_PARAM_STATIC_STRINGS);
	props[PROP_LIMIT] =
		g_param_spec_uint ("limit", NULL, NULL,
		                   1, G_MAXUINT, 1,
		                   G_PARAM_WRITABLE |
		                   G_PARAM_CONSTRUCT_ONLY |
		                   G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
tracker_sparql_buffer_init (TrackerSparqlBuffer *buffer)
{
}

TrackerSparqlBuffer *
tracker_sparql_buffer_new (TrackerSparqlConnection *connection,
                           guint                    limit,
                           GFile                   *root)
{
	return g_object_new (TRACKER_TYPE_SPARQL_BUFFER,
	                     "connection", connection,
	                     "limit", limit,
	                     "root", root,
	                     NULL);
}

static void
update_batch_data_free (UpdateBatchData *batch_data)
{
	g_object_unref (batch_data->batch);

	g_ptr_array_unref (batch_data->tasks);

	g_slice_free (UpdateBatchData, batch_data);
}

static void
batch_execute_cb (GObject      *object,
                  GAsyncResult *result,
                  gpointer      user_data)
{
	TrackerSparqlBuffer *buffer;
	GError *error = NULL;
	UpdateBatchData *update_data;
	g_autoptr (GTask) task = NULL;

	task = user_data;
	update_data = g_task_get_task_data (task);
	buffer = TRACKER_SPARQL_BUFFER (update_data->buffer);
	buffer->n_updates--;

	TRACKER_NOTE (MINER_FS_EVENTS,
	              g_message ("(Sparql buffer) Finished array-update with %u tasks",
	                         update_data->tasks->len));

	if (!tracker_batch_execute_finish (TRACKER_BATCH (object),
	                                   result,
	                                   &error)) {
		g_task_return_error (task, error);
	} else {
		g_task_return_boolean (task, TRUE);
	}
}

gboolean
tracker_sparql_buffer_flush (TrackerSparqlBuffer *buffer,
                             const gchar         *reason,
                             GAsyncReadyCallback  cb,
                             gpointer             user_data)
{
	UpdateBatchData *update_data;
	GTask *task;

	if (buffer->n_updates > 0) {
		return FALSE;
	}

	if (!buffer->tasks ||
	    buffer->tasks->len == 0) {
		return FALSE;
	}

	TRACKER_NOTE (MINER_FS_EVENTS, g_message ("Flushing SPARQL buffer, reason: %s", reason));

	update_data = g_slice_new0 (UpdateBatchData);
	update_data->buffer = buffer;
	update_data->tasks = g_steal_pointer (&buffer->tasks);
	update_data->batch = g_steal_pointer (&buffer->batch);

	task = g_task_new (buffer, NULL, cb, user_data);
	g_task_set_task_data (task, update_data, (GDestroyNotify) update_batch_data_free);

	buffer->n_updates++;

	tracker_batch_execute_async (update_data->batch,
	                             NULL,
	                             batch_execute_cb,
	                             task);
	return TRUE;
}

static void
sparql_buffer_push_task (TrackerSparqlBuffer *buffer,
                         SparqlTaskData      *task)
{
	if (!buffer->tasks)
		buffer->tasks = g_ptr_array_new_with_free_func ((GDestroyNotify) sparql_task_data_free);

	g_ptr_array_add (buffer->tasks, task);
}

static TrackerBatch *
tracker_sparql_buffer_get_current_batch (TrackerSparqlBuffer *buffer)
{
	g_return_val_if_fail (TRACKER_IS_SPARQL_BUFFER (buffer), NULL);

	if (!buffer->batch)
		buffer->batch = tracker_sparql_connection_create_batch (buffer->connection);

	return buffer->batch;
}

static SparqlTaskData *
sparql_task_data_new_resource (GFile           *file,
                               const gchar     *graph,
                               TrackerResource *resource)
{
	SparqlTaskData *task_data;

	task_data = g_slice_new0 (SparqlTaskData);
	g_set_object (&task_data->file, file);
	task_data->type = TASK_TYPE_RESOURCE;
	task_data->d.resource.resource = g_object_ref (resource);
	task_data->d.resource.graph = g_strdup (graph);

	return task_data;
}

static SparqlTaskData *
sparql_task_data_new_stmt (GFile                  *file,
                           TrackerSparqlStatement *stmt)
{
	SparqlTaskData *task_data;

	task_data = g_slice_new0 (SparqlTaskData);
	g_set_object (&task_data->file, file);
	task_data->type = TASK_TYPE_STMT;
	task_data->d.stmt.stmt = stmt;

	return task_data;
}

static void
sparql_task_data_free (SparqlTaskData *data)
{
	if (data->type == TASK_TYPE_RESOURCE) {
		g_clear_object (&data->d.resource.resource);
		g_free (data->d.resource.graph);
	}

	g_clear_object (&data->file);

	g_slice_free (SparqlTaskData, data);
}

static void
tracker_sparql_buffer_push (TrackerSparqlBuffer *buffer,
                            GFile               *file,
                            const gchar         *graph,
                            TrackerResource     *resource)
{
	TrackerBatch *batch;
	SparqlTaskData *task;

	g_return_if_fail (TRACKER_IS_SPARQL_BUFFER (buffer));
	g_return_if_fail (G_IS_FILE (file));
	g_return_if_fail (TRACKER_IS_RESOURCE (resource));

	batch = tracker_sparql_buffer_get_current_batch (buffer);
	tracker_batch_add_resource (batch, graph, resource);

	task = sparql_task_data_new_resource (file, graph, resource);
	sparql_buffer_push_task (buffer, task);
}

static gchar *
sparql_task_get_sparql (SparqlTaskData *task_data)
{
	if (task_data->type == TASK_TYPE_RESOURCE) {
		return tracker_resource_print_sparql_update (task_data->d.resource.resource,
		                                             NULL,
		                                             task_data->d.resource.graph);
	} else if (task_data->type == TASK_TYPE_STMT) {
		return g_strdup (tracker_sparql_statement_get_sparql (task_data->d.stmt.stmt));
	}

	return NULL;
}

gboolean
tracker_sparql_buffer_flush_finish (TrackerSparqlBuffer  *buffer,
                                    GAsyncResult         *res,
                                    GError              **error)
{
	UpdateBatchData *update_data;
	GError *inner_error = NULL;
	gboolean retval;

	g_return_val_if_fail (TRACKER_IS_SPARQL_BUFFER (buffer), FALSE);
	g_return_val_if_fail (G_IS_ASYNC_RESULT (res), FALSE);
	g_return_val_if_fail (!error || !*error, FALSE);

	retval = g_task_propagate_boolean (G_TASK (res), &inner_error);

	if (!retval) {
		if (!g_error_matches (inner_error, TRACKER_SPARQL_ERROR,
		                      TRACKER_SPARQL_ERROR_NO_SPACE)) {
			SparqlTaskData *task;
			GString *str;
			unsigned int i;

			str = g_string_new ("Unexpected error with batch, elements are:\n");
			update_data = g_task_get_task_data (G_TASK (res));

			for (i = 0; i < update_data->tasks->len; i++) {
				g_autofree char *sparql = NULL, *uri = NULL;
				GFile *task_file;

				task = g_ptr_array_index (update_data->tasks, i);
				task_file = task->file;
				sparql = sparql_task_get_sparql (task);
				uri = g_file_get_uri (task_file);

				g_string_append_printf (str, "URI: %s\nSPARQL: %s\n",
				                        uri, sparql);
			}

			g_string_append (str, "Error obtained was: ");

			g_propagate_prefixed_error (error, inner_error,
			                            "%s", str->str);
			g_string_free (str, TRUE);
		} else {
			g_propagate_error (error, inner_error);
		}
	}

	return retval;
}

static void
push_stmt_task (TrackerSparqlBuffer    *buffer,
                TrackerSparqlStatement *stmt,
                GFile                  *file)
{
	SparqlTaskData *task;

	task = sparql_task_data_new_stmt (file, stmt);
	sparql_buffer_push_task (buffer, task);
}

static char *
resolve_file_uri (TrackerSparqlBuffer *buffer,
                  GFile               *file)
{
	if (buffer->root)
		return tracker_file_get_relative_uri (file, buffer->root);
	else
		return g_file_get_uri (file);
}

void
tracker_sparql_buffer_log_delete (TrackerSparqlBuffer *buffer,
                                  GFile               *file)
{
	TrackerBatch *batch;
	g_autofree gchar *uri = NULL;

	g_return_if_fail (TRACKER_IS_SPARQL_BUFFER (buffer));
	g_return_if_fail (G_IS_FILE (file));

	uri = resolve_file_uri (buffer, file);
	batch = tracker_sparql_buffer_get_current_batch (buffer);
	tracker_batch_add_statement (batch, buffer->delete_file,
	                             "uri", G_TYPE_STRING, uri,
	                             NULL);
	push_stmt_task (buffer, buffer->delete_file, file);
}

void
tracker_sparql_buffer_log_delete_content (TrackerSparqlBuffer *buffer,
                                          GFile               *file)
{
	TrackerBatch *batch;
	g_autofree gchar *uri = NULL;

	g_return_if_fail (TRACKER_IS_SPARQL_BUFFER (buffer));
	g_return_if_fail (G_IS_FILE (file));

	uri = resolve_file_uri (buffer, file);
	batch = tracker_sparql_buffer_get_current_batch (buffer);
	tracker_batch_add_statement (batch, buffer->delete_content,
	                             "uri", G_TYPE_STRING, uri,
	                             NULL);
	push_stmt_task (buffer, buffer->delete_content, file);
}

void
tracker_sparql_buffer_log_move (TrackerSparqlBuffer *buffer,
                                GFile               *source,
                                GFile               *dest,
                                const gchar         *dest_data_source)
{
	TrackerBatch *batch;
	g_autofree gchar *source_uri = NULL, *dest_uri = NULL, *new_parent_uri = NULL;
	g_autofree gchar *basename = NULL, *path = NULL;
	g_autoptr (GFile) new_parent = NULL;

	g_return_if_fail (TRACKER_IS_SPARQL_BUFFER (buffer));
	g_return_if_fail (G_IS_FILE (source));
	g_return_if_fail (G_IS_FILE (dest));

	source_uri = resolve_file_uri (buffer, source);
	dest_uri = resolve_file_uri (buffer, dest);
	path = g_file_get_path (dest);
	new_parent = g_file_get_parent (dest);
	new_parent_uri = resolve_file_uri (buffer, new_parent);
	basename = g_filename_display_basename (path);

	batch = tracker_sparql_buffer_get_current_batch (buffer);
	tracker_batch_add_statement (batch, buffer->move_file,
	                             "sourceUri", G_TYPE_STRING, source_uri,
	                             "destUri", G_TYPE_STRING, dest_uri,
	                             "newFilename", G_TYPE_STRING, basename,
	                             "newParent", G_TYPE_STRING, new_parent_uri,
	                             "newDataSource", G_TYPE_STRING, dest_data_source,
	                             NULL);

	push_stmt_task (buffer, buffer->move_file, dest);
}

void
tracker_sparql_buffer_log_move_content (TrackerSparqlBuffer *buffer,
                                        GFile               *source,
                                        GFile               *dest)
{
	TrackerBatch *batch;
	g_autofree gchar *source_uri = NULL, *dest_uri = NULL;

	g_return_if_fail (TRACKER_IS_SPARQL_BUFFER (buffer));
	g_return_if_fail (G_IS_FILE (source));
	g_return_if_fail (G_IS_FILE (dest));

	source_uri = resolve_file_uri (buffer, source);
	dest_uri = resolve_file_uri (buffer, dest);

	batch = tracker_sparql_buffer_get_current_batch (buffer);
	tracker_batch_add_statement (batch, buffer->move_content,
	                             "sourceUri", G_TYPE_STRING, source_uri,
	                             "destUri", G_TYPE_STRING, dest_uri,
	                             NULL);

	push_stmt_task (buffer, buffer->move_content, dest);
}

void
tracker_sparql_buffer_log_clear_content (TrackerSparqlBuffer *buffer,
                                         GFile               *file)
{
	TrackerBatch *batch;
	g_autofree gchar *uri = NULL;

	g_return_if_fail (TRACKER_IS_SPARQL_BUFFER (buffer));
	g_return_if_fail (G_IS_FILE (file));

	batch = tracker_sparql_buffer_get_current_batch (buffer);
	uri = resolve_file_uri (buffer, file);
	tracker_batch_add_statement (batch, buffer->delete_file_content,
	                             "uri", G_TYPE_STRING, uri,
	                             NULL);

	push_stmt_task (buffer, buffer->delete_file_content, file);
}

void
tracker_sparql_buffer_log_file (TrackerSparqlBuffer *buffer,
                                GFile               *file,
                                const gchar         *content_graph,
                                TrackerResource     *file_resource,
                                TrackerResource     *graph_resource)
{
	g_autofree gchar *uri = NULL;

	g_return_if_fail (TRACKER_IS_SPARQL_BUFFER (buffer));
	g_return_if_fail (G_IS_FILE (file));
	g_return_if_fail (TRACKER_IS_RESOURCE (file_resource));
	g_return_if_fail (!graph_resource || TRACKER_IS_RESOURCE (graph_resource));

	tracker_sparql_buffer_push (buffer, file, DEFAULT_GRAPH, file_resource);

	if (content_graph && graph_resource)
		tracker_sparql_buffer_push (buffer, file, content_graph, graph_resource);
}

void
tracker_sparql_buffer_log_folder (TrackerSparqlBuffer *buffer,
                                  GFile               *file,
                                  gboolean             is_root,
                                  TrackerResource     *file_resource,
                                  TrackerResource     *folder_resource)
{
	g_autofree gchar *uri = NULL;

	g_return_if_fail (TRACKER_IS_SPARQL_BUFFER (buffer));
	g_return_if_fail (G_IS_FILE (file));
	g_return_if_fail (TRACKER_IS_RESOURCE (file_resource));
	g_return_if_fail (TRACKER_IS_RESOURCE (folder_resource));

	/* Add indexing roots also to content specific graphs to provide the availability information */
	if (is_root) {
		const gchar *special_graphs[] = {
			"tracker:Audio",
			"tracker:Documents",
			"tracker:Pictures",
			"tracker:Software",
			"tracker:Video"
		};
		gint i;

		for (i = 0; i < G_N_ELEMENTS (special_graphs); i++) {
			tracker_sparql_buffer_push (buffer, file, special_graphs[i], folder_resource);
		}
	}

	tracker_sparql_buffer_push (buffer, file, DEFAULT_GRAPH, file_resource);
	tracker_sparql_buffer_push (buffer, file, DEFAULT_GRAPH, folder_resource);
}

void
tracker_sparql_buffer_log_attributes_update (TrackerSparqlBuffer *buffer,
                                             GFile               *file,
                                             const gchar         *content_graph,
                                             TrackerResource     *file_resource,
                                             TrackerResource     *graph_resource)
{
	g_return_if_fail (TRACKER_IS_SPARQL_BUFFER (buffer));
	g_return_if_fail (G_IS_FILE (file));
	g_return_if_fail (TRACKER_IS_RESOURCE (file_resource));
	g_return_if_fail (!graph_resource || TRACKER_IS_RESOURCE (graph_resource));

	if (content_graph && graph_resource)
		tracker_sparql_buffer_push (buffer, file, content_graph, graph_resource);

	tracker_sparql_buffer_push (buffer, file, DEFAULT_GRAPH, file_resource);
}

gboolean
tracker_sparql_buffer_limit_reached (TrackerSparqlBuffer *buffer)
{
	if (!buffer->tasks)
		return FALSE;

	return buffer->tasks->len >= buffer->limit;
}

unsigned int
tracker_sparql_buffer_get_size (TrackerSparqlBuffer *buffer)
{
	if (!buffer->tasks)
		return 0;

	return buffer->tasks->len;
}
