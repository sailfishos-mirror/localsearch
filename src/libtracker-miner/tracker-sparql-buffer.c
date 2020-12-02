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

#include <libtracker-sparql/tracker-sparql.h>
#include "libtracker-miners-common/tracker-debug.h"

#include "tracker-sparql-buffer.h"

typedef struct _TrackerSparqlBufferPrivate TrackerSparqlBufferPrivate;
typedef struct _SparqlTaskData SparqlTaskData;
typedef struct _UpdateArrayData UpdateArrayData;

enum {
	PROP_0,
	PROP_CONNECTION
};

struct _TrackerSparqlBufferPrivate
{
	TrackerSparqlConnection *connection;
	GPtrArray *tasks;
	gint n_updates;
};

struct _SparqlTaskData
{
	gchar *str;
};

struct _UpdateArrayData {
	TrackerSparqlBuffer *buffer;
	GPtrArray *tasks;
	GArray *sparql_array;
	GTask *async_task;
};

G_DEFINE_TYPE_WITH_PRIVATE (TrackerSparqlBuffer, tracker_sparql_buffer, TRACKER_TYPE_TASK_POOL)

static void
tracker_sparql_buffer_finalize (GObject *object)
{
	TrackerSparqlBufferPrivate *priv;

	priv = tracker_sparql_buffer_get_instance_private (TRACKER_SPARQL_BUFFER (object));

	g_object_unref (priv->connection);

	G_OBJECT_CLASS (tracker_sparql_buffer_parent_class)->finalize (object);
}

static void
tracker_sparql_buffer_set_property (GObject      *object,
                                    guint         param_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
	TrackerSparqlBufferPrivate *priv;

	priv = tracker_sparql_buffer_get_instance_private (TRACKER_SPARQL_BUFFER (object));

	switch (param_id) {
	case PROP_CONNECTION:
		priv->connection = g_value_dup_object (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
		break;
	}
}

static void
tracker_sparql_buffer_get_property (GObject    *object,
                                    guint       param_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
	TrackerSparqlBufferPrivate *priv;

	priv = tracker_sparql_buffer_get_instance_private (TRACKER_SPARQL_BUFFER (object));

	switch (param_id) {
	case PROP_CONNECTION:
		g_value_set_object (value,
		                    priv->connection);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
		break;
	}
}

static void
tracker_sparql_buffer_class_init (TrackerSparqlBufferClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = tracker_sparql_buffer_finalize;
	object_class->set_property = tracker_sparql_buffer_set_property;
	object_class->get_property = tracker_sparql_buffer_get_property;

	g_object_class_install_property (object_class,
	                                 PROP_CONNECTION,
	                                 g_param_spec_object ("connection",
	                                                      "sparql connection",
	                                                      "Sparql Connection",
	                                                      TRACKER_SPARQL_TYPE_CONNECTION,
	                                                      G_PARAM_READWRITE |
	                                                      G_PARAM_CONSTRUCT_ONLY |
	                                                      G_PARAM_STATIC_STRINGS));
}

static void
tracker_sparql_buffer_init (TrackerSparqlBuffer *buffer)
{
}

TrackerSparqlBuffer *
tracker_sparql_buffer_new (TrackerSparqlConnection *connection,
                           guint                    limit)
{
	return g_object_new (TRACKER_TYPE_SPARQL_BUFFER,
	                     "connection", connection,
	                     "limit", limit,
	                     NULL);
}

static void
remove_task_foreach (TrackerTask     *task,
                     TrackerTaskPool *pool)
{
	tracker_task_pool_remove (pool, task);
}

static void
update_array_data_free (UpdateArrayData *update_data)
{
	if (!update_data)
		return;

	if (update_data->sparql_array) {
		/* The array contains pointers to strings in the tasks, so no need to
		 * deallocate its pointed contents, just the array itself. */
		g_array_free (update_data->sparql_array, TRUE);
	}

	g_ptr_array_foreach (update_data->tasks,
	                     (GFunc) remove_task_foreach,
	                     update_data->buffer);
	g_ptr_array_unref (update_data->tasks);

	g_slice_free (UpdateArrayData, update_data);
}

static void
tracker_sparql_buffer_update_array_cb (GObject      *object,
                                       GAsyncResult *result,
                                       gpointer      user_data)
{
	TrackerSparqlBufferPrivate *priv;
	TrackerSparqlBuffer *buffer;
	GError *error = NULL;
	UpdateArrayData *update_data;

	update_data = user_data;
	buffer = TRACKER_SPARQL_BUFFER (update_data->buffer);
	priv = tracker_sparql_buffer_get_instance_private (buffer);
	priv->n_updates--;

	TRACKER_NOTE (MINER_FS_EVENTS,
	              g_message ("(Sparql buffer) Finished array-update with %u tasks",
	                         update_data->tasks->len));

	if (!tracker_sparql_connection_update_array_finish (priv->connection,
							    result,
							    &error)) {
		g_critical ("  (Sparql buffer) Error in array-update: %s",
		            error->message);
	}

	if (error) {
		g_task_return_error (update_data->async_task, error);
	} else {
		g_task_return_pointer (update_data->async_task,
		                       g_ptr_array_ref (update_data->tasks),
		                       (GDestroyNotify) g_ptr_array_unref);
	}

	/* Note that tasks are actually deallocated here */
	update_array_data_free (update_data);
}

gboolean
tracker_sparql_buffer_flush (TrackerSparqlBuffer *buffer,
                             const gchar         *reason,
                             GAsyncReadyCallback  cb,
                             gpointer             user_data)
{
	TrackerSparqlBufferPrivate *priv;
	GArray *sparql_array;
	UpdateArrayData *update_data;
	gint i;

	priv = tracker_sparql_buffer_get_instance_private (buffer);

	if (priv->n_updates > 0) {
		return FALSE;
	}

	if (!priv->tasks ||
	    priv->tasks->len == 0) {
		return FALSE;
	}

	TRACKER_NOTE (MINER_FS_EVENTS, g_message ("Flushing SPARQL buffer, reason: %s", reason));

	/* Loop buffer and construct array of strings */
	sparql_array = g_array_new (FALSE, TRUE, sizeof (gchar *));

	for (i = 0; i < priv->tasks->len; i++) {
		SparqlTaskData *task_data;
		TrackerTask *task;

		task = g_ptr_array_index (priv->tasks, i);
		task_data = tracker_task_get_data (task);
		g_array_append_val (sparql_array, task_data->str);
	}

	update_data = g_slice_new0 (UpdateArrayData);
	update_data->buffer = buffer;
	update_data->tasks = g_ptr_array_ref (priv->tasks);
	update_data->sparql_array = sparql_array;
	update_data->async_task = g_task_new (buffer, NULL, cb, user_data);

	/* Empty pool, update_data will keep
	 * references to the tasks to keep
	 * these alive.
	 */
	g_ptr_array_unref (priv->tasks);
	priv->tasks = NULL;
	priv->n_updates++;

	/* Start the update */
	tracker_sparql_connection_update_array_async (priv->connection,
	                                              (gchar **) update_data->sparql_array->data,
	                                              update_data->sparql_array->len,
	                                              NULL,
	                                              tracker_sparql_buffer_update_array_cb,
	                                              update_data);
	return TRUE;
}

static void
sparql_buffer_push_to_pool (TrackerSparqlBuffer *buffer,
                            TrackerTask         *task)
{
	TrackerSparqlBufferPrivate *priv;

	priv = tracker_sparql_buffer_get_instance_private (buffer);

	/* Task pool addition increments reference */
	tracker_task_pool_add (TRACKER_TASK_POOL (buffer), task);

	if (!priv->tasks) {
		priv->tasks = g_ptr_array_new_with_free_func ((GDestroyNotify) tracker_task_unref);
	}

	/* We add a reference here because we unref when removed from
	 * the GPtrArray. */
	g_ptr_array_add (priv->tasks, tracker_task_ref (task));
}

void
tracker_sparql_buffer_push (TrackerSparqlBuffer *buffer,
                            TrackerTask         *task)
{
	g_return_if_fail (TRACKER_IS_SPARQL_BUFFER (buffer));
	g_return_if_fail (task != NULL);

	sparql_buffer_push_to_pool (buffer, task);
}

static SparqlTaskData *
sparql_task_data_new (gchar *data,
                      guint  flags)
{
	SparqlTaskData *task_data;

	task_data = g_slice_new0 (SparqlTaskData);
	task_data->str = data;

	return task_data;
}

static void
sparql_task_data_free (SparqlTaskData *data)
{
	g_free (data->str);
	g_slice_free (SparqlTaskData, data);
}

TrackerTask *
tracker_sparql_task_new_take_sparql_str (GFile *file,
                                         gchar *sparql_str)
{
	SparqlTaskData *data;

	data = sparql_task_data_new (sparql_str, 0);
	return tracker_task_new (file, data,
	                         (GDestroyNotify) sparql_task_data_free);
}

TrackerTask *
tracker_sparql_task_new_with_sparql_str (GFile       *file,
                                         const gchar *sparql_str)
{
	SparqlTaskData *data;

	data = sparql_task_data_new (g_strdup (sparql_str), 0);
	return tracker_task_new (file, data,
	                         (GDestroyNotify) sparql_task_data_free);
}

const gchar *
tracker_sparql_task_get_sparql (TrackerTask *task)
{
	SparqlTaskData *task_data;

	task_data = tracker_task_get_data (task);

	return task_data->str;
}

GPtrArray *
tracker_sparql_buffer_flush_finish (TrackerSparqlBuffer  *buffer,
                                    GAsyncResult         *res,
                                    GError              **error)
{
	g_return_val_if_fail (TRACKER_IS_SPARQL_BUFFER (buffer), NULL);
	g_return_val_if_fail (G_IS_ASYNC_RESULT (res), NULL);
	g_return_val_if_fail (!error || !*error, NULL);

	return g_task_propagate_pointer (G_TASK (res), error);
}

static gboolean
task_has_file (TrackerTask *task,
               GFile       *file)
{
	GFile *task_file;

	task_file = tracker_task_get_file (task);

	return g_file_equal (task_file, file);
}

TrackerSparqlBufferState
tracker_sparql_buffer_get_state (TrackerSparqlBuffer *buffer,
                                 GFile               *file)
{
	TrackerSparqlBufferPrivate *priv;

	g_return_val_if_fail (TRACKER_IS_SPARQL_BUFFER (buffer), TRACKER_BUFFER_STATE_UNKNOWN);
	g_return_val_if_fail (G_IS_FILE (file), TRACKER_BUFFER_STATE_UNKNOWN);

	priv = tracker_sparql_buffer_get_instance_private (TRACKER_SPARQL_BUFFER (buffer));

	if (!tracker_task_pool_find (TRACKER_TASK_POOL (buffer), file))
		return TRACKER_BUFFER_STATE_UNKNOWN;

	if (priv->tasks &&
	    g_ptr_array_find_with_equal_func (priv->tasks, file,
	                                      (GEqualFunc) task_has_file, NULL))
		return TRACKER_BUFFER_STATE_QUEUED;

	return TRACKER_BUFFER_STATE_FLUSHING;
}
