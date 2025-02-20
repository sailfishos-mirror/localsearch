/*
 * Copyright (C) 2008, Nokia <ivan.frade@nokia.com>
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

#include <glib/gi18n.h>

#include "utils/tracker-extract.h"

#include <valgrind.h>

#include "tracker-extract.h"
#include "tracker-main.h"

G_DEFINE_QUARK (TrackerExtractError, tracker_extract_error)

#define DEFAULT_DEADLINE_SECONDS 5

#define DEFAULT_MAX_TEXT 1048576

static gint deadline_seconds = -1;

typedef struct {
	GTimer *elapsed;
	gint extracted_count;
	gint failed_count;
} StatisticsData;

struct _TrackerExtract {
	GObject parent_instance;

	GHashTable *statistics_data;

	gint max_text;

	/* used to maintain the running tasks
	 * and stats from different threads
	 */
	GMutex stats_mutex;

	GMainContext *thread_context;
	GMainLoop *thread_loop;

	GThread *task_thread;

	GTimer *total_elapsed;

	gint unhandled_count;
};

typedef struct {
	TrackerExtract *extract;
	GCancellable *cancellable;
	GAsyncResult *res;
	gchar *content_id;
	gchar *file;
	gchar *mimetype;
	const gchar *graph;
	gint max_text;

	TrackerExtractMetadataFunc func;
	GModule *module;

	GSource *deadline;

	guint success : 1;
} TrackerExtractTask;

static void tracker_extract_finalize (GObject *object);
static void log_statistics        (GObject *object);
static gboolean get_metadata         (TrackerExtractTask *task);
static gboolean dispatch_task_cb     (TrackerExtractTask *task);


G_DEFINE_TYPE(TrackerExtract, tracker_extract, G_TYPE_OBJECT)

static void
tracker_extract_class_init (TrackerExtractClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = tracker_extract_finalize;
}

static void
statistics_data_free (StatisticsData *data)
{
	g_timer_destroy (data->elapsed);
	g_slice_free (StatisticsData, data);
}

static void
tracker_extract_init (TrackerExtract *extract)
{
	extract->max_text = DEFAULT_MAX_TEXT;

#ifdef G_ENABLE_DEBUG
	if (TRACKER_DEBUG_CHECK (STATISTICS)) {
		extract->total_elapsed = g_timer_new ();
		g_timer_stop (extract->total_elapsed);
		extract->statistics_data =
			g_hash_table_new_full (NULL, NULL, NULL,
			                       (GDestroyNotify) statistics_data_free);
		g_mutex_init (&extract->stats_mutex);
	}
#endif
}

static void
tracker_extract_finalize (GObject *object)
{
	TrackerExtract *extract = TRACKER_EXTRACT (object);

#ifdef G_ENABLE_DEBUG
	if (TRACKER_DEBUG_CHECK (STATISTICS)) {
		log_statistics (object);
		g_hash_table_destroy (extract->statistics_data);
		g_timer_destroy (extract->total_elapsed);
		g_mutex_clear (&extract->stats_mutex);
	}
#endif

	if (extract->thread_loop) {
		g_main_loop_quit (extract->thread_loop);
		g_main_loop_unref (extract->thread_loop);
	}

	if (extract->task_thread)
		g_thread_join (extract->task_thread);

	if (extract->thread_context)
		g_main_context_unref (extract->thread_context);

	G_OBJECT_CLASS (tracker_extract_parent_class)->finalize (object);
}

static void
log_statistics (GObject *object)
{
#ifdef G_ENABLE_DEBUG
	if (TRACKER_DEBUG_CHECK (STATISTICS)) {
		TrackerExtract *extract = TRACKER_EXTRACT (object);
		GHashTableIter iter;
		gpointer key, value;
		gdouble total_elapsed;

		g_mutex_lock (&extract->stats_mutex);

		g_message ("--------------------------------------------------");
		g_message ("Statistics:");

		g_hash_table_iter_init (&iter, extract->statistics_data);
		total_elapsed = g_timer_elapsed (extract->total_elapsed, NULL);

		while (g_hash_table_iter_next (&iter, &key, &value)) {
			GModule *module = key;
			StatisticsData *data = value;

			if (data->extracted_count > 0 || data->failed_count > 0) {
				const gchar *name, *name_without_path;

				name = g_module_name (module);
				name_without_path = strrchr (name, G_DIR_SEPARATOR) + 1;

				g_message ("    Module:'%s', extracted:%d, failures:%d, elapsed: %.2fs (%.2f%% of total)",
				           name_without_path,
				           data->extracted_count,
				           data->failed_count,
					   g_timer_elapsed (data->elapsed, NULL),
					   (g_timer_elapsed (data->elapsed, NULL) / total_elapsed) * 100);
			}
		}

		g_message ("Unhandled files: %d", extract->unhandled_count);

		if (extract->unhandled_count == 0 &&
		    g_hash_table_size (extract->statistics_data) < 1) {
			g_message ("    No files handled");
		}

		g_message ("--------------------------------------------------");

		g_mutex_unlock (&extract->stats_mutex);
	}
#endif
}

TrackerExtract *
tracker_extract_new (void)
{
	return g_object_new (TRACKER_TYPE_EXTRACT, NULL);
}

static void
notify_task_finish (TrackerExtractTask *task,
                    gboolean            success)
{
	TrackerExtract *extract;
	StatisticsData *stats_data;

	extract = task->extract;

	/* Reports and ongoing tasks may be
	 * accessed from other threads.
	 */
#ifdef G_ENABLE_DEBUG
	if (TRACKER_DEBUG_CHECK (STATISTICS)) {
		g_mutex_lock (&extract->stats_mutex);

		if (task->module) {
			stats_data = g_hash_table_lookup (extract->statistics_data,
			                                  task->module);
			if (stats_data) {
				stats_data->extracted_count++;

				if (!success) {
					stats_data->failed_count++;
				}
			}
		} else {
			extract->unhandled_count++;
		}

		g_mutex_unlock (&extract->stats_mutex);
	}
#endif
}

static gboolean
get_file_metadata (TrackerExtractTask  *task,
                   TrackerExtractInfo **info_out,
                   GError             **error)
{
	TrackerExtractInfo *info;
	GFile *file;

	*info_out = NULL;

	file = g_file_new_for_uri (task->file);
	info = tracker_extract_info_new (file, task->content_id, task->mimetype, task->graph, task->max_text);
	g_object_unref (file);

	if (!task->mimetype || !*task->mimetype) {
		tracker_extract_info_unref (info);
		return FALSE;
	}

	/* Now we have sanity checked everything, actually get the
	 * data we need from the extractors.
	 */
	if (task->func && task->module) {
		g_debug ("Using %s...",
		         g_module_name (task->module));

		task->success = (task->func) (info, error);
	} else {
		g_autoptr (TrackerResource) resource = NULL;

		/* Dummy extractor */
		resource = tracker_resource_new (NULL);
		tracker_extract_info_set_resource (info, resource);
		task->success = TRUE;
	}

	if (!task->success) {
		tracker_extract_info_unref (info);
		info = NULL;
	}

	*info_out = info;

	return task->success;
}

static gboolean
task_deadline_cb (gpointer user_data)
{
	TrackerExtractTask *task = user_data;

	g_warning ("File '%s' took too long to process. Shutting down everything",
	           task->file);

	exit (EXIT_FAILURE);
}

static TrackerExtractTask *
extract_task_new (TrackerExtract *extract,
                  const gchar    *uri,
                  const gchar    *content_id,
                  const gchar    *mimetype,
                  const gchar    *graph,
                  GCancellable   *cancellable,
                  GAsyncResult   *res)
{
	TrackerExtractTask *task;

	task = g_slice_new0 (TrackerExtractTask);
	task->cancellable = (cancellable) ? g_object_ref (cancellable) : NULL;
	task->res = (res) ? g_object_ref (res) : NULL;
	task->file = g_strdup (uri);
	task->content_id = g_strdup (content_id);
	task->mimetype = g_strdup (mimetype);
	task->graph = graph;
	task->extract = extract;
	task->max_text = extract->max_text;

	task->module = tracker_extract_module_manager_get_module (task->mimetype,
	                                                          NULL,
	                                                          &task->func);

	if (task->res && !RUNNING_ON_VALGRIND) {
		if (deadline_seconds < 0) {
			const gchar *deadline_envvar;

			deadline_envvar = g_getenv ("TRACKER_EXTRACT_DEADLINE");
			if (deadline_envvar)
				deadline_seconds = atoi (deadline_envvar);
			else
				deadline_seconds = DEFAULT_DEADLINE_SECONDS;
		}

		task->deadline =
			g_timeout_source_new_seconds (deadline_seconds);
		g_source_set_callback (task->deadline, task_deadline_cb, task, NULL);
		g_source_attach (task->deadline,
		                 g_task_get_context (G_TASK (task->res)));
	}

	return task;
}

static void
extract_task_free (TrackerExtractTask *task)
{
	notify_task_finish (task, task->success);

	if (task->deadline) {
		g_source_destroy (task->deadline);
		g_source_unref (task->deadline);
	}

	if (task->res) {
		g_object_unref (task->res);
	}

	if (task->cancellable) {
		g_object_unref (task->cancellable);
	}

	g_free (task->mimetype);
	g_free (task->file);
	g_free (task->content_id);

	g_slice_free (TrackerExtractTask, task);
}

static gboolean
get_metadata (TrackerExtractTask *task)
{
	TrackerExtract *extract = task->extract;
	TrackerExtractInfo *info;
	GError *error = NULL;

	if (g_task_return_error_if_cancelled (G_TASK (task->res))) {
		extract_task_free (task);
		return FALSE;
	}

#ifdef G_ENABLE_DEBUG
	if (TRACKER_DEBUG_CHECK (STATISTICS)) {
		StatisticsData *stats_data;

		stats_data = g_hash_table_lookup (extract->statistics_data,
						  task->module);
		if (!stats_data) {
			stats_data = g_slice_new0 (StatisticsData);
			stats_data->elapsed = g_timer_new ();
			g_hash_table_insert (extract->statistics_data,
					     task->module,
					     stats_data);
		} else {
			g_timer_continue (stats_data->elapsed);
		}
	}
#endif

	if (get_file_metadata (task, &info, &error)) {
		g_task_return_pointer (G_TASK (task->res), info,
		                       (GDestroyNotify) tracker_extract_info_unref);
	} else {
		if (error) {
			g_task_return_error (G_TASK (task->res), error);
		} else {
			g_task_return_new_error (G_TASK (task->res),
			                         tracker_extract_error_quark (),
			                         TRACKER_EXTRACT_ERROR_NO_EXTRACTOR,
			                         "Could not get any metadata for uri:'%s' and mime:'%s'",
			                         task->file, task->mimetype);
		}
	}

#ifdef G_ENABLE_DEBUG
	if (TRACKER_DEBUG_CHECK (STATISTICS)) {
		StatisticsData *stats_data;

		stats_data = g_hash_table_lookup (extract->statistics_data,
						  task->module);
		g_timer_stop (stats_data->elapsed);
	}
#endif

	extract_task_free (task);

	return FALSE;
}

static gboolean
handle_task_in_thread (gpointer user_data)
{
	TrackerExtractTask *task = user_data;

	get_metadata (task);

	return G_SOURCE_REMOVE;
}


static gpointer
metadata_thread_func (GMainLoop *loop)
{
	GMainContext *context;

	context = g_main_loop_get_context (loop);
	g_main_context_push_thread_default (context);

	g_main_loop_run (loop);

	g_main_context_pop_thread_default (context);
	g_main_loop_unref (loop);

	return NULL;
}

/* This function is executed in the main thread, decides the
 * module that's going to be run for a given task, and dispatches
 * the task according to the threading strategy of that module.
 */
static gboolean
dispatch_task_cb (TrackerExtractTask *task)
{
	TrackerExtract *extract = task->extract;
	GError *error = NULL;

	if (!extract->task_thread) {
		extract->thread_context = g_main_context_new ();
		extract->thread_loop = g_main_loop_new (extract->thread_context, FALSE);

		extract->task_thread =
			g_thread_try_new ("single",
					  (GThreadFunc) metadata_thread_func,
					  g_main_loop_ref (extract->thread_loop),
					  &error);
		if (!extract->task_thread) {
			g_task_return_error (G_TASK (task->res), error);
			extract_task_free (task);
			return FALSE;
		}

	}

	g_main_context_invoke (extract->thread_context,
			       handle_task_in_thread,
			       task);
	return FALSE;
}

/* This function can be called in any thread */
void
tracker_extract_file (TrackerExtract      *extract,
                      const gchar         *file,
                      const gchar         *content_id,
                      const gchar         *mimetype,
                      GCancellable        *cancellable,
                      GAsyncReadyCallback  cb,
                      gpointer             user_data)
{
	g_autoptr (GTask) async_task = NULL;
	g_autoptr (GError) error = NULL;
	TrackerExtractTask *task;
	const char *graph;

	g_return_if_fail (TRACKER_IS_EXTRACT (extract));
	g_return_if_fail (file != NULL);
	g_return_if_fail (cb != NULL);

	if (!mimetype) {
		g_task_report_new_error (extract, cb, user_data, NULL,
		                         TRACKER_EXTRACT_ERROR,
		                         TRACKER_EXTRACT_ERROR_NO_MIMETYPE,
		                         "No mimetype for '%s'",
		                         file);
		return;
	}

	graph = tracker_extract_module_manager_get_graph (mimetype);

	if (!graph) {
		g_task_report_new_error (extract, cb, user_data, NULL,
		                         TRACKER_EXTRACT_ERROR,
		                         TRACKER_EXTRACT_ERROR_NO_EXTRACTOR,
		                         "Unknown target graph for uri:'%s' and mime:'%s'",
		                         file, mimetype);
		return;
	}

	async_task = g_task_new (extract, cancellable, cb, user_data);

	task = extract_task_new (extract, file, content_id, mimetype, graph, cancellable,
	                         G_ASYNC_RESULT (async_task));

#ifdef G_ENABLE_DEBUG
	if (TRACKER_DEBUG_CHECK (STATISTICS)) {
		g_timer_continue (extract->total_elapsed);
	}
#endif

	g_idle_add ((GSourceFunc) dispatch_task_cb, task);
}

TrackerExtractInfo *
tracker_extract_file_sync (TrackerExtract  *object,
                           const gchar     *uri,
                           const gchar     *content_id,
                           const gchar     *mimetype,
                           GError         **error)
{
	TrackerExtractTask *task;
	TrackerExtractInfo *info;
	const char *graph = NULL;

	g_return_val_if_fail (uri != NULL, NULL);
	g_return_val_if_fail (content_id != NULL, NULL);

	if (mimetype)
		graph = tracker_extract_module_manager_get_graph (mimetype);

	if (!graph) {
		g_set_error (error,
		             TRACKER_EXTRACT_ERROR,
		             TRACKER_EXTRACT_ERROR_NO_EXTRACTOR,
		             "Unknown target graph for uri:'%s' and mime:'%s'",
		             uri, mimetype);
		return NULL;
	}

	task = extract_task_new (object, uri, content_id, mimetype, graph,
	                         NULL, NULL);

	if (!get_file_metadata (task, &info, error))
		return NULL;

	extract_task_free (task);
	return info;
}

TrackerExtractInfo *
tracker_extract_file_finish (TrackerExtract  *extract,
                             GAsyncResult    *res,
                             GError         **error)
{
	g_return_val_if_fail (TRACKER_IS_EXTRACT (extract), NULL);
	g_return_val_if_fail (G_IS_ASYNC_RESULT (res), NULL);
	g_return_val_if_fail (!error || !*error, NULL);

#ifdef G_ENABLE_DEBUG
	if (TRACKER_DEBUG_CHECK (STATISTICS)) {
		g_timer_stop (extract->total_elapsed);
	}
#endif

	return g_task_propagate_pointer (G_TASK (res), error);
}

void
tracker_extract_set_max_text (TrackerExtract *extract,
                              gint            max_text)
{
	extract->max_text = max_text;
}
