/*
 * Copyright (C) 2009, Nokia <ivan.frade@nokia.com>
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

#include <tracker-common.h>

#include "tracker-error-report.h"
#include "tracker-extract-watchdog.h"
#include "tracker-indexer.h"
#include "tracker-indexer-methods.h"
#include "tracker-monitor.h"
#include "tracker-utils.h"
#include "tracker-priority-queue.h"
#include "tracker-task-pool.h"
#include "tracker-sparql-buffer.h"
#include "tracker-file-notifier.h"
#include "tracker-lru.h"

#define BUFFER_POOL_LIMIT 800
#define DEFAULT_URN_LRU_SIZE 100

/* Put tasks processing at a lower priority so other events
 * (timeouts, monitor events, etc...) are guaranteed to be
 * dispatched promptly.
 */
#define TRACKER_TASK_PRIORITY G_PRIORITY_DEFAULT_IDLE + 10

#define MAX_SIMULTANEOUS_ITEMS 64

#define TRACKER_CRAWLER_MAX_TIMEOUT_INTERVAL 1000

#define RETRY_AFTER_DISK_FULL (60 * 15)

typedef struct {
	guint16 type;
	guint attributes_update : 1;
	guint is_dir : 1;
	GFile *file;
	GFile *dest_file;
	GFileInfo *info;
	GList *queue_node;
} QueueEvent;

typedef struct {
	GFile *file;
	gchar *urn;
	gint priority;
	GCancellable *cancellable;
	TrackerMiner *miner;
	TrackerTask *task;
} UpdateProcessingTaskContext;

struct _TrackerIndexer {
	TrackerMiner parent_instance;

	TrackerPriorityQueue *items;
	GHashTable *items_by_file;

	guint item_queues_handler_id;

	TrackerMonitor *monitor;
	TrackerIndexingTree *indexing_tree;
	TrackerFileNotifier *file_notifier;
	TrackerErrorReport *error_reports;
	TrackerExtractWatchdog *extract_watchdog;

#ifdef HAVE_POWER
	TrackerPower *power;
#endif

	/* Root for relative URIs */
	GFile *root;

	/* Sparql insertion tasks */
	TrackerSparqlBuffer *sparql_buffer;

	/* Folder URN cache */
	TrackerLRU *urn_lru;

	/* Properties */
	gdouble throttle;

	/* Status */
	GTimer *timer;
	GTimer *extraction_timer;

	guint low_battery_pause : 1;

	guint active : 1;
	guint is_paused : 1;        /* TRUE if miner is paused */
	guint flushing : 1;         /* TRUE if flushing SPARQL */

	guint timer_stopped : 1;    /* TRUE if main timer is stopped */
	guint extraction_timer_stopped : 1; /* TRUE if the extraction
	                                     * timer is stopped */

	guint status_idle_id;
	guint grace_period_timeout_id;
	guint resume_after_disk_full_id;
};

typedef enum {
	QUEUE_ACTION_NONE           = 0,
	QUEUE_ACTION_DELETE_FIRST   = 1 << 0,
	QUEUE_ACTION_DELETE_SECOND  = 1 << 1,
} QueueCoalesceAction;

typedef enum {
	TRACKER_INDEXER_EVENT_CREATED,
	TRACKER_INDEXER_EVENT_UPDATED,
	TRACKER_INDEXER_EVENT_DELETED,
	TRACKER_INDEXER_EVENT_MOVED,
	TRACKER_INDEXER_EVENT_FINISH_DIRECTORY,
} TrackerIndexerEventType;

enum {
	FINISHED,
	CORRUPT,
	NO_SPACE,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

enum {
	PROP_0,
	PROP_INDEXING_TREE,
	PROP_MONITOR,
	PROP_ROOT,
	PROP_ERROR_REPORTS,
	PROP_ACTIVE,
	N_PROPS,
};

static GParamSpec *props[N_PROPS] = { 0, };

static void           fs_finalize                         (GObject              *object);
static void           fs_constructed                      (GObject              *object);
static void           fs_set_property                     (GObject              *object,
                                                           guint                 prop_id,
                                                           const GValue         *value,
                                                           GParamSpec           *pspec);
static void           fs_get_property                     (GObject              *object,
                                                           guint                 prop_id,
                                                           GValue               *value,
                                                           GParamSpec           *pspec);

static void           miner_started                       (TrackerMiner         *miner);
static void           miner_stopped                       (TrackerMiner         *miner);
static void           miner_paused                        (TrackerMiner         *miner);
static void           miner_resumed                       (TrackerMiner         *miner);

static void           indexing_tree_directory_added       (TrackerIndexingTree  *indexing_tree,
                                                           GFile                *directory,
                                                           gpointer              user_data);
static void           indexing_tree_directory_removed     (TrackerIndexingTree  *indexing_tree,
                                                           GFile                *directory,
                                                           gpointer              user_data);
static void           file_notifier_file_created          (TrackerFileNotifier  *notifier,
                                                           GFile                *file,
                                                           GFileInfo            *info,
                                                           gpointer              user_data);
static void           file_notifier_file_deleted          (TrackerFileNotifier  *notifier,
                                                           GFile                *file,
                                                           gboolean              is_dir,
                                                           gpointer              user_data);
static void           file_notifier_file_updated          (TrackerFileNotifier  *notifier,
                                                           GFile                *file,
                                                           GFileInfo            *info,
                                                           gboolean              attributes_only,
                                                           gpointer              user_data);
static void           file_notifier_file_moved            (TrackerFileNotifier  *notifier,
                                                           GFile                *source,
                                                           GFile                *dest,
                                                           gboolean              is_dir,
                                                           gpointer              user_data);

static void           file_notifier_directory_finished (TrackerFileNotifier *notifier,
                                                        GFile               *directory,
                                                        gpointer             user_data);

static void           file_notifier_finished              (TrackerFileNotifier *notifier,
                                                           gpointer             user_data);

static void           queue_handler_maybe_set_up          (TrackerIndexer *indexer);

static void           task_pool_limit_reached_notify_cb       (GObject        *object,
                                                               GParamSpec     *pspec,
                                                               gpointer        user_data);

G_DEFINE_TYPE (TrackerIndexer, tracker_indexer, TRACKER_TYPE_MINER)

#define EVENT_QUEUE_LOG_PREFIX "[Event Queue] "

G_GNUC_UNUSED static void
debug_print_event (QueueEvent *event)
{
	const gchar *event_type_name[] = { "CREATED", "UPDATED", "DELETED", "MOVED", "FINISH_DIRECTORY" };
	g_autofree char *uri1 = NULL, *uri2 = NULL;

	uri1 = g_file_get_uri (event->file);
	if (event->dest_file)
		uri2 = g_file_get_uri (event->dest_file);

	g_message ("%s New %s event: %s%s%s%s",
	            EVENT_QUEUE_LOG_PREFIX,
	            event_type_name[event->type],
	            event->attributes_update ? "(attributes only) " : "",
	            uri1,
	            uri2 ? "->" : "",
	            uri2 ? uri2 : "");
}

static void
tracker_indexer_class_init (TrackerIndexerClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	TrackerMinerClass *miner_class = TRACKER_MINER_CLASS (klass);

	object_class->finalize = fs_finalize;
	object_class->constructed = fs_constructed;
	object_class->set_property = fs_set_property;
	object_class->get_property = fs_get_property;

	miner_class->started = miner_started;
	miner_class->stopped = miner_stopped;
	miner_class->paused  = miner_paused;
	miner_class->resumed = miner_resumed;

	props[PROP_INDEXING_TREE] =
		g_param_spec_object ("indexing-tree", NULL, NULL,
		                     TRACKER_TYPE_INDEXING_TREE,
		                     G_PARAM_WRITABLE |
		                     G_PARAM_CONSTRUCT_ONLY |
		                     G_PARAM_STATIC_STRINGS);
	props[PROP_MONITOR] =
		g_param_spec_object ("monitor", NULL, NULL,
		                     TRACKER_TYPE_MONITOR,
		                     G_PARAM_WRITABLE |
		                     G_PARAM_CONSTRUCT_ONLY |
		                     G_PARAM_STATIC_STRINGS);
	props[PROP_ROOT] =
		g_param_spec_object ("root", NULL, NULL,
		                     G_TYPE_FILE,
		                     G_PARAM_WRITABLE |
		                     G_PARAM_CONSTRUCT_ONLY |
		                     G_PARAM_STATIC_STRINGS);
	props[PROP_ERROR_REPORTS] =
		g_param_spec_object ("error-reports", NULL, NULL,
		                     TRACKER_TYPE_ERROR_REPORT,
		                     G_PARAM_WRITABLE |
		                     G_PARAM_CONSTRUCT_ONLY |
		                     G_PARAM_STATIC_STRINGS);
	props[PROP_ACTIVE] =
		g_param_spec_boolean ("active", NULL, NULL,
		                      FALSE,
		                      G_PARAM_READABLE |
		                      G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties (object_class, N_PROPS, props);

	signals[FINISHED] =
		g_signal_new ("finished",
		              G_TYPE_FROM_CLASS (object_class),
		              G_SIGNAL_RUN_LAST,
		              0, NULL, NULL, NULL,
		              G_TYPE_NONE, 0);

	signals[CORRUPT] =
		g_signal_new ("corrupt",
		              G_TYPE_FROM_CLASS (object_class),
		              G_SIGNAL_RUN_LAST, 0,
		              NULL, NULL,
		              g_cclosure_marshal_VOID__VOID,
		              G_TYPE_NONE, 0);
	signals[NO_SPACE] =
		g_signal_new ("no-space",
		              G_TYPE_FROM_CLASS (object_class),
		              G_SIGNAL_RUN_LAST, 0,
		              NULL, NULL,
		              g_cclosure_marshal_VOID__VOID,
		              G_TYPE_NONE, 0);
}

static void
tracker_indexer_init (TrackerIndexer *indexer)
{
	indexer->timer = g_timer_new ();
	indexer->extraction_timer = g_timer_new ();

	g_timer_stop (indexer->timer);
	g_timer_stop (indexer->extraction_timer);

	indexer->timer_stopped = TRUE;
	indexer->extraction_timer_stopped = TRUE;

	indexer->items = tracker_priority_queue_new ();
	indexer->items_by_file = g_hash_table_new_full (g_file_hash,
	                                                (GEqualFunc) g_file_equal,
	                                                g_object_unref, NULL);

	indexer->urn_lru = tracker_lru_new (DEFAULT_URN_LRU_SIZE,
	                                    g_file_hash,
	                                    (GEqualFunc) g_file_equal,
	                                    g_object_unref,
	                                    g_free);
}

static QueueEvent *
queue_event_new (TrackerIndexerEventType  type,
                 GFile                   *file,
                 GFileInfo               *info)
{
	QueueEvent *event;

	g_assert (type != TRACKER_INDEXER_EVENT_MOVED);

	event = g_new0 (QueueEvent, 1);
	event->type = type;
	g_set_object (&event->file, file);
	g_set_object (&event->info, info);

	return event;
}

static QueueEvent *
queue_event_moved_new (GFile    *source,
                       GFile    *dest,
                       gboolean  is_dir)
{
	QueueEvent *event;

	event = g_new0 (QueueEvent, 1);
	event->type = TRACKER_INDEXER_EVENT_MOVED;
	event->is_dir = !!is_dir;
	g_set_object (&event->dest_file, dest);
	g_set_object (&event->file, source);

	return event;
}

static void
queue_event_free (QueueEvent *event)
{
	g_clear_object (&event->dest_file);
	g_clear_object (&event->file);
	g_clear_object (&event->info);
	g_free (event);
}

static QueueCoalesceAction
queue_event_coalesce (const QueueEvent  *first,
		      const QueueEvent  *second,
		      QueueEvent       **replacement)
{
	if (!g_file_equal (first->file, second->file))
		return QUEUE_ACTION_NONE;

	*replacement = NULL;

	if (first->type == TRACKER_INDEXER_EVENT_CREATED) {
		if (second->type == TRACKER_INDEXER_EVENT_CREATED ||
		    (second->type == TRACKER_INDEXER_EVENT_UPDATED &&
		     !second->attributes_update)) {
			return QUEUE_ACTION_DELETE_FIRST;
		} else if (second->type == TRACKER_INDEXER_EVENT_MOVED) {
			*replacement = queue_event_new (TRACKER_INDEXER_EVENT_CREATED,
			                                second->dest_file,
			                                NULL);
			return (QUEUE_ACTION_DELETE_FIRST |
				QUEUE_ACTION_DELETE_SECOND);
		} else if (second->type == TRACKER_INDEXER_EVENT_DELETED) {
			/* We can't be sure that "create" is replacing a file
			 * here. Preserve the second event just in case.
			 */
			return QUEUE_ACTION_DELETE_FIRST;
		}
	} else if (first->type == TRACKER_INDEXER_EVENT_UPDATED) {
		if (second->type == TRACKER_INDEXER_EVENT_UPDATED) {
			if (first->attributes_update && !second->attributes_update)
				return QUEUE_ACTION_DELETE_FIRST;
			else
				return QUEUE_ACTION_DELETE_SECOND;
		} else if (second->type == TRACKER_INDEXER_EVENT_DELETED) {
			return QUEUE_ACTION_DELETE_FIRST;
		}
	} else if (first->type == TRACKER_INDEXER_EVENT_MOVED) {
		if (second->type == TRACKER_INDEXER_EVENT_MOVED) {
			if (first->file != second->dest_file) {
				*replacement = queue_event_moved_new (first->file,
				                                      second->dest_file,
				                                      first->is_dir);
			}

			return (QUEUE_ACTION_DELETE_FIRST |
				QUEUE_ACTION_DELETE_SECOND);
		} else if (second->type == TRACKER_INDEXER_EVENT_DELETED) {
			*replacement = queue_event_new (TRACKER_INDEXER_EVENT_DELETED,
			                                first->file,
			                                NULL);
			return (QUEUE_ACTION_DELETE_FIRST |
				QUEUE_ACTION_DELETE_SECOND);
		}
	} else if (first->type == TRACKER_INDEXER_EVENT_DELETED &&
		   second->type == TRACKER_INDEXER_EVENT_DELETED) {
		return QUEUE_ACTION_DELETE_SECOND;
	}

	return QUEUE_ACTION_NONE;
}

static gboolean
queue_event_is_equal_or_descendant (QueueEvent *event,
				    GFile      *prefix)
{
	return (g_file_equal (event->file, prefix) ||
		g_file_has_prefix (event->file, prefix));
}

static void
set_up_mount_point (TrackerIndexer *indexer,
                    GFile          *mount_point,
                    gboolean        mounted,
                    TrackerBatch   *batch)
{
	TrackerSparqlConnection *conn = tracker_batch_get_connection (batch);
	g_autoptr (TrackerSparqlStatement) stmt = NULL;
	g_autofree gchar *uri = NULL;

	uri = g_file_get_uri (mount_point);

	g_debug ("Mount point state (%s) being set in DB for mount_point '%s'",
	         mounted ? "MOUNTED" : "UNMOUNTED",
	         uri);

	stmt = tracker_load_statement (conn, "update-mountpoint.rq", NULL);

	tracker_batch_add_statement (batch, stmt,
	                             "mountPoint", G_TYPE_STRING, uri,
	                             "mounted", G_TYPE_BOOLEAN, mounted,
	                             NULL);
}

static void
delete_index_root (TrackerIndexer *indexer,
                   GFile          *mount_point,
                   TrackerBatch   *batch)
{
	g_autofree gchar *uri = NULL;
	TrackerSparqlConnection *conn = tracker_batch_get_connection (batch);
	g_autoptr (TrackerSparqlStatement) stmt = NULL;

	uri = g_file_get_uri (mount_point);
	stmt = tracker_load_statement (conn, "delete-index-root.rq", NULL);
	tracker_batch_add_statement (batch, stmt,
	                             "rootFolder", G_TYPE_STRING, uri,
	                             NULL);
}

static void
init_index_roots (TrackerIndexer *indexer)
{
	TrackerSparqlConnection *conn;
	g_autoptr (TrackerSparqlStatement) stmt = NULL;
	g_autoptr (GList) roots = NULL;
	g_autoptr (GHashTable) handled = NULL;
	g_autoptr (TrackerBatch) batch = NULL;
	g_autoptr (GError) error = NULL;
	g_autoptr (TrackerSparqlCursor) cursor = NULL;
	GList *l;

	g_debug ("Initializing mount points...");

	conn = tracker_miner_get_connection (TRACKER_MINER (indexer));
	stmt = tracker_load_statement (conn, "get-index-roots.rq", &error);

	if (stmt) {
		/* First, get all mounted volumes, according to tracker-store (SYNC!) */
		cursor = tracker_sparql_statement_execute (stmt, NULL, &error);
	}

	if (error) {
		g_critical ("Could not obtain the mounted volumes: %s", error->message);
		return;
	}

	batch = tracker_sparql_connection_create_batch (conn);
	handled = g_hash_table_new_full (g_file_hash, (GEqualFunc) g_file_equal,
	                                 g_object_unref, NULL);

	while (tracker_sparql_cursor_next (cursor, NULL, NULL)) {
		const gchar *uri;
		gboolean is_removable;
		GFile *file;

		uri = tracker_sparql_cursor_get_string (cursor, 0, NULL);
		is_removable = tracker_sparql_cursor_get_boolean (cursor, 1);

		file = g_file_new_for_uri (uri);
		g_hash_table_add (handled, file);

		if (tracker_indexing_tree_file_is_root (indexer->indexing_tree, file)) {
			/* Directory is indexed and configured */
			if (is_removable) {
				set_up_mount_point (indexer, file, TRUE, batch);
			}
		} else {
			/* Directory is indexed, but no longer configured */
			if (is_removable) {
				/* Preserve */
				set_up_mount_point (indexer, file, FALSE, batch);
			} else {
				/* Not a removable device to preserve, or a no
				 * longer configured folder.
				 */
				delete_index_root (indexer, file, batch);
			}
		}
	}

	roots = tracker_indexing_tree_list_roots (indexer->indexing_tree);

	for (l = roots; l; l = l->next) {
		TrackerDirectoryFlags flags;
		GFile *file = l->data;

		if (g_hash_table_contains (handled, file))
			continue;

		tracker_indexing_tree_get_root (indexer->indexing_tree, file, NULL, &flags);

		if (!!(flags & TRACKER_DIRECTORY_FLAG_IS_VOLUME))
			set_up_mount_point (indexer, file, TRUE, batch);
	}

	if (!tracker_batch_execute (batch, NULL, &error)) {
		g_critical ("Could not initialize currently active mount points: %s",
		            error->message);
	}
}

static void
check_unextracted (TrackerIndexer *indexer)
{
	g_debug ("Starting extractor");
	tracker_extract_watchdog_ensure_started (indexer->extract_watchdog);
}

static gboolean
extractor_lost_timeout_cb (gpointer user_data)
{
	TrackerIndexer *indexer = user_data;

	check_unextracted (indexer);
	indexer->grace_period_timeout_id = 0;
	return G_SOURCE_REMOVE;
}

static void
set_active (TrackerIndexer *indexer,
            gboolean        active)
{
	if (indexer->active == !!active)
		return;

	indexer->active = !!active;
	g_object_notify (G_OBJECT (indexer), "active");
}

static void
on_extractor_lost (TrackerExtractWatchdog *watchdog,
                   TrackerIndexer         *indexer)
{
	g_debug ("tracker-extract vanished, maybe restarting.");

	/* Give a period of grace before restarting, so we allow replacing
	 * from eg. a terminal.
	 */
	indexer->grace_period_timeout_id =
		g_timeout_add_seconds (1, extractor_lost_timeout_cb, indexer);
}

static void
on_extractor_status (TrackerExtractWatchdog *watchdog,
                     const gchar            *status,
                     gdouble                 progress,
                     gint                    remaining,
                     TrackerIndexer         *indexer)
{
	if (!tracker_miner_is_paused (TRACKER_MINER (indexer))) {
		set_active (indexer, g_strcmp0 (status, "Idle") != 0);
		g_object_set (indexer,
		              "status", status,
		              "progress", progress,
		              "remaining-time", remaining,
		              NULL);
	}
}

static void
on_extractor_file_error (TrackerExtractWatchdog *watchdog,
                         const char             *uri,
                         const char             *msg,
                         const char             *extra,
                         TrackerIndexer         *indexer)
{
	g_autoptr (GFile) file = NULL;

	if (!indexer->error_reports)
		return;

	file = g_file_new_for_uri (uri);
	tracker_error_report_save (indexer->error_reports, file, msg, extra);
}

static gboolean
retry_after_disk_full_cb (gpointer user_data)
{
	TrackerIndexer *indexer = user_data;

	indexer->resume_after_disk_full_id = 0;
	tracker_miner_resume (TRACKER_MINER (indexer));

	return G_SOURCE_REMOVE;
}

#ifdef HAVE_POWER
static void
set_up_throttle (TrackerIndexer *indexer,
                 gboolean        enable)
{
	gdouble throttle = 0;

	throttle = enable ? 0.25 : 0;
	g_debug ("Setting new throttle to %0.3f", throttle);

	if (indexer->throttle == throttle) {
		return;
	}

	indexer->throttle = throttle;

	/* Update timeouts */
	if (indexer->item_queues_handler_id != 0) {
		g_clear_handle_id (&indexer->item_queues_handler_id, g_source_remove);
		queue_handler_maybe_set_up (indexer);
	}
}

static void
check_battery_status (TrackerIndexer *indexer)
{
	gboolean on_battery, on_low_battery;
	gboolean should_pause = FALSE;
	gboolean should_throttle = FALSE;

	if (indexer->power == NULL) {
		return;
	}

	on_low_battery = tracker_power_get_on_low_battery (indexer->power);
	on_battery = tracker_power_get_on_battery (indexer->power);

	if (!on_battery) {
		g_debug ("Running on AC power");
		should_pause = FALSE;
		should_throttle = FALSE;
	} else if (on_low_battery) {
		g_message ("Running on LOW Battery, pausing");
		should_pause = TRUE;
		should_throttle = TRUE;
	} else {
		g_debug ("Running on battery");
		should_throttle = TRUE;
		should_pause = FALSE;
	}

	set_up_throttle (indexer, should_throttle);

	if (indexer->low_battery_pause == should_pause)
		return;

	indexer->low_battery_pause = should_pause;

	if (should_pause) {
		tracker_miner_pause (TRACKER_MINER (indexer));
	} else {
		tracker_miner_resume (TRACKER_MINER (indexer));
	}
}

static void
battery_status_cb (GObject    *object,
                   GParamSpec *pspec,
                   gpointer    user_data)
{
	TrackerIndexer *indexer = user_data;

	check_battery_status (indexer);
}
#endif /* HAVE_POWER */

static void
fs_finalize (GObject *object)
{
	TrackerIndexer *indexer = TRACKER_INDEXER (object);

	g_timer_destroy (indexer->timer);
	g_timer_destroy (indexer->extraction_timer);

	g_clear_handle_id (&indexer->status_idle_id, g_source_remove);

	g_clear_pointer (&indexer->urn_lru, tracker_lru_unref);
	g_clear_handle_id (&indexer->item_queues_handler_id, g_source_remove);
	g_clear_handle_id (&indexer->grace_period_timeout_id, g_source_remove);
	g_clear_handle_id (&indexer->resume_after_disk_full_id, g_source_remove);

	if (indexer->file_notifier)
		tracker_file_notifier_stop (indexer->file_notifier);

	g_clear_object (&indexer->sparql_buffer);
	g_hash_table_unref (indexer->items_by_file);

	tracker_priority_queue_foreach (indexer->items,
	                                (GFunc) queue_event_free,
	                                NULL);
	tracker_priority_queue_unref (indexer->items);

	g_clear_object (&indexer->indexing_tree);
	g_clear_object (&indexer->file_notifier);
	g_clear_object (&indexer->monitor);
	g_clear_object (&indexer->error_reports);
	g_clear_object (&indexer->extract_watchdog);
#ifdef HAVE_POWER
	g_clear_object (&indexer->power);
#endif

	G_OBJECT_CLASS (tracker_indexer_parent_class)->finalize (object);
}

static void
fs_constructed (GObject *object)
{
	TrackerIndexer *indexer = TRACKER_INDEXER (object);

	G_OBJECT_CLASS (tracker_indexer_parent_class)->constructed (object);

	g_signal_connect (indexer->indexing_tree, "directory-added",
	                  G_CALLBACK (indexing_tree_directory_added),
	                  object);
	g_signal_connect (indexer->indexing_tree, "directory-removed",
	                  G_CALLBACK (indexing_tree_directory_removed),
	                  object);

	indexer->sparql_buffer = tracker_sparql_buffer_new (tracker_miner_get_connection (TRACKER_MINER (object)),
	                                                    BUFFER_POOL_LIMIT,
	                                                    indexer->root);
	g_signal_connect (indexer->sparql_buffer, "notify::limit-reached",
	                  G_CALLBACK (task_pool_limit_reached_notify_cb),
	                  object);

	/* Create the file notifier */
	indexer->file_notifier = tracker_file_notifier_new (indexer->indexing_tree,
	                                                    tracker_miner_get_connection (TRACKER_MINER (object)),
	                                                    indexer->monitor,
	                                                    indexer->root);

	g_signal_connect (indexer->file_notifier, "file-created",
	                  G_CALLBACK (file_notifier_file_created),
	                  object);
	g_signal_connect (indexer->file_notifier, "file-updated",
	                  G_CALLBACK (file_notifier_file_updated),
	                  object);
	g_signal_connect (indexer->file_notifier, "file-deleted",
	                  G_CALLBACK (file_notifier_file_deleted),
	                  object);
	g_signal_connect (indexer->file_notifier, "file-moved",
	                  G_CALLBACK (file_notifier_file_moved),
	                  object);
	g_signal_connect (indexer->file_notifier, "directory-finished",
	                  G_CALLBACK (file_notifier_directory_finished),
	                  object);
	g_signal_connect (indexer->file_notifier, "finished",
	                  G_CALLBACK (file_notifier_finished),
	                  object);

#ifdef HAVE_POWER
	indexer->power = tracker_power_new ();

	if (indexer->power) {
		g_signal_connect (indexer->power, "notify::on-low-battery",
		                  G_CALLBACK (battery_status_cb),
		                  object);
		g_signal_connect (indexer->power, "notify::on-battery",
		                  G_CALLBACK (battery_status_cb),
		                  object);

		check_battery_status (indexer);
	}
#endif

	indexer->extract_watchdog =
		tracker_extract_watchdog_new (tracker_miner_get_connection (TRACKER_MINER (object)),
		                              indexer->indexing_tree,
		                              indexer->root);
	g_signal_connect (indexer->extract_watchdog, "lost",
	                  G_CALLBACK (on_extractor_lost), object);
	g_signal_connect (indexer->extract_watchdog, "status",
	                  G_CALLBACK (on_extractor_status), object);
	g_signal_connect (indexer->extract_watchdog, "error",
	                  G_CALLBACK (on_extractor_file_error), object);

	g_object_set (object,
	              "progress", 0.0,
	              "status", "Initializing",
	              "remaining-time", -1,
	              NULL);
}

static void
fs_set_property (GObject      *object,
                 guint         prop_id,
                 const GValue *value,
                 GParamSpec   *pspec)
{
	TrackerIndexer *indexer = TRACKER_INDEXER (object);

	switch (prop_id) {
	case PROP_INDEXING_TREE:
		indexer->indexing_tree = g_value_dup_object (value);
		break;
	case PROP_MONITOR:
		indexer->monitor = g_value_dup_object (value);
		break;
	case PROP_ROOT:
		indexer->root = g_value_dup_object (value);
		break;
	case PROP_ERROR_REPORTS:
		indexer->error_reports = g_value_dup_object (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
fs_get_property (GObject    *object,
                 guint       prop_id,
                 GValue     *value,
                 GParamSpec *pspec)
{
	TrackerIndexer *indexer = TRACKER_INDEXER (object);

	switch (prop_id) {
	case PROP_ACTIVE:
		g_value_set_boolean (value, indexer->active);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
task_pool_limit_reached_notify_cb (GObject    *object,
				   GParamSpec *pspec,
				   gpointer    user_data)
{
	if (!tracker_task_pool_limit_reached (TRACKER_TASK_POOL (object)))
		queue_handler_maybe_set_up (user_data);
}

static void
miner_started (TrackerMiner *miner)
{
	TrackerIndexer *indexer = TRACKER_INDEXER (miner);

	if (indexer->timer_stopped) {
		g_timer_start (indexer->timer);
		indexer->timer_stopped = FALSE;
	}

	tracker_file_notifier_start (indexer->file_notifier);
	init_index_roots (indexer);
}

static void
miner_stopped (TrackerMiner *miner)
{
}

static void
miner_paused (TrackerMiner *miner)
{
	TrackerIndexer *indexer = TRACKER_INDEXER (miner);

	indexer->is_paused = TRUE;
	tracker_file_notifier_stop (indexer->file_notifier);
	g_clear_handle_id (&indexer->item_queues_handler_id, g_source_remove);
}

static void
miner_resumed (TrackerMiner *miner)
{
	TrackerIndexer *indexer = TRACKER_INDEXER (miner);

	indexer->is_paused = FALSE;

	tracker_file_notifier_start (indexer->file_notifier);

	/* Only set up queue handler if we have items waiting to be
	 * processed.
	 */
	if (tracker_file_notifier_is_active (indexer->file_notifier) ||
	    !tracker_priority_queue_is_empty (indexer->items))
		queue_handler_maybe_set_up (indexer);
}

static void
process_stop (TrackerIndexer *indexer)
{
	g_timer_stop (indexer->timer);
	g_timer_stop (indexer->extraction_timer);

	indexer->timer_stopped = TRUE;
	indexer->extraction_timer_stopped = TRUE;

	g_clear_handle_id (&indexer->status_idle_id, g_source_remove);

	set_active (indexer, FALSE);

	check_unextracted (indexer);

	g_signal_emit (indexer, signals[FINISHED], 0);
}

static void
check_notifier_high_water (TrackerIndexer *indexer)
{
	gboolean high_water;

	/* If there is more than worth 2 batches left processing, we can tell
	 * the notifier to stop a bit.
	 */
	high_water = (tracker_priority_queue_get_length (indexer->items) >
	              2 * BUFFER_POOL_LIMIT);
	tracker_file_notifier_set_high_water (indexer->file_notifier, high_water);
}

static void
sparql_buffer_flush_cb (GObject      *object,
                        GAsyncResult *result,
                        gpointer      user_data)
{
	TrackerIndexer *indexer = user_data;
	GPtrArray *tasks;
	GError *error = NULL;
	TrackerTask *task;
	GFile *task_file;
	guint i;

	tasks = tracker_sparql_buffer_flush_finish (TRACKER_SPARQL_BUFFER (object),
	                                            result, &error);

	indexer->flushing = FALSE;

	if (error) {
		g_warning ("Could not execute sparql: %s", error->message);

		if (g_error_matches (error, TRACKER_SPARQL_ERROR, TRACKER_SPARQL_ERROR_CORRUPT) ||
		    g_error_matches (error, TRACKER_SPARQL_ERROR, TRACKER_SPARQL_ERROR_CONSTRAINT)) {
			g_signal_emit (indexer, signals[CORRUPT], 0);
			return;
		} else if (g_error_matches (error, TRACKER_SPARQL_ERROR,
		                            TRACKER_SPARQL_ERROR_NO_SPACE)) {
			g_signal_emit (indexer, signals[NO_SPACE], 0);

			tracker_miner_pause (TRACKER_MINER (indexer));
			indexer->resume_after_disk_full_id =
				g_timeout_add_seconds (RETRY_AFTER_DISK_FULL,
				                       retry_after_disk_full_cb, indexer);
			return;
		}
	}

	if (indexer->error_reports) {
		for (i = 0; i < tasks->len; i++) {
			task = g_ptr_array_index (tasks, i);
			task_file = tracker_task_get_file (task);

			if (error) {
				g_autofree char *sparql = NULL;

				sparql = tracker_sparql_task_get_sparql (task);
				tracker_error_report_save (indexer->error_reports,
				                           task_file,
				                           error->message,
				                           sparql);
			} else {
				tracker_error_report_delete (indexer->error_reports,
				                             task_file);
			}
		}
	}

	if (tracker_task_pool_limit_reached (TRACKER_TASK_POOL (object))) {
		if (tracker_sparql_buffer_flush (TRACKER_SPARQL_BUFFER (object),
						 "SPARQL buffer again full after flush",
						 sparql_buffer_flush_cb,
						 indexer))
			indexer->flushing = TRUE;
	}

	queue_handler_maybe_set_up (indexer);

	g_ptr_array_unref (tasks);
	g_clear_error (&error);
}

static void
item_add_or_update (TrackerIndexer *indexer,
                    GFile          *file,
                    GFileInfo      *file_info,
                    gboolean        attributes_update,
                    gboolean        create)
{
	g_autoptr (GFileInfo) info = NULL;
	g_autofree char *uri = NULL;

	if (file_info) {
		g_set_object (&info, file_info);
	} else {
		info = g_file_query_info (file,
		                          INDEXER_FILE_ATTRIBUTES,
		                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
		                          NULL, NULL);
		if (!info)
			return;
	}

	if (!create) {
		tracker_lru_remove (indexer->urn_lru, file);
	}

	uri = g_file_get_uri (file);

	if (!attributes_update) {
		TRACKER_NOTE (MINER_FS_EVENTS, g_message ("Processing file '%s'...", uri));
		tracker_indexer_process_file (indexer, file, info,
		                              indexer->sparql_buffer, create);
	} else {
		TRACKER_NOTE (MINER_FS_EVENTS, g_message ("Processing attributes in file '%s'...", uri));
		tracker_indexer_process_file_attributes (indexer, file, info,
		                                         indexer->sparql_buffer);
	}
}

static void
item_remove (TrackerIndexer *indexer,
             GFile          *file,
             gboolean        is_dir)
{
	g_autofree char *uri = NULL;

	uri = g_file_get_uri (file);

	TRACKER_NOTE (MINER_FS_EVENTS,
	              g_message ("Removing item: '%s' (Deleted from filesystem or no longer monitored)", uri));

	if (is_dir) {
		tracker_sparql_buffer_log_delete_content (indexer->sparql_buffer,
		                                          file);
		tracker_lru_remove_foreach (indexer->urn_lru,
		                            (GEqualFunc) g_file_has_prefix,
		                            file);
	}

	tracker_sparql_buffer_log_delete (indexer->sparql_buffer, file);
	tracker_lru_remove (indexer->urn_lru, file);
}

static void
item_move (TrackerIndexer *indexer,
           GFile          *dest_file,
           GFile          *source_file,
           gboolean        is_dir)
{
	g_autofree char *uri = NULL, *source_uri = NULL;
	TrackerDirectoryFlags source_flags, flags;
	gboolean source_recursive, dest_recursive;
	const gchar *data_source = NULL;

	uri = g_file_get_uri (dest_file);
	source_uri = g_file_get_uri (source_file);

	TRACKER_NOTE (MINER_FS_EVENTS,
	              g_message ("Moving item from '%s' to '%s'",
	                         source_uri, uri));

	tracker_indexing_tree_get_root (indexer->indexing_tree, source_file, NULL, &source_flags);
	source_recursive = (source_flags & TRACKER_DIRECTORY_FLAG_RECURSE) != 0;
	tracker_indexing_tree_get_root (indexer->indexing_tree, dest_file, NULL, &flags);
	dest_recursive = (flags & TRACKER_DIRECTORY_FLAG_RECURSE) != 0;

	if (is_dir) {
		tracker_lru_remove_foreach (indexer->urn_lru,
		                            (GEqualFunc) g_file_has_prefix,
		                            source_file);
	}

	tracker_lru_remove (indexer->urn_lru, source_file);
	tracker_lru_remove (indexer->urn_lru, dest_file);

	/* If the original location is recursive, but the destination location
	 * is not, remove all children.
	 */
	if (is_dir && source_recursive && !dest_recursive) {
		TRACKER_NOTE (MINER_FS_EVENTS,
		              g_message ("Removing children for item: '%s' (No longer monitored)", source_uri));

		tracker_sparql_buffer_log_delete_content (indexer->sparql_buffer,
		                                          source_file);
	}

	if (tracker_indexing_tree_file_is_root (indexer->indexing_tree, dest_file)) {
		data_source = tracker_indexer_get_content_uri (indexer, dest_file);
	} else {
		GFile *root;

		root = tracker_indexing_tree_get_root (indexer->indexing_tree,
		                                       dest_file, NULL, NULL);

		if (root)
			data_source = tracker_indexer_get_content_uri (indexer, root);
	}

	if (!data_source)
		return;

	tracker_sparql_buffer_log_move (indexer->sparql_buffer,
	                                source_file, dest_file,
	                                data_source);

	if (is_dir && source_recursive && dest_recursive) {
		tracker_sparql_buffer_log_move_content (indexer->sparql_buffer,
		                                        source_file, dest_file);
	}
}

static void
item_finish_directory (TrackerIndexer *indexer,
                       GFile          *file)
{
	tracker_indexer_finish_directory (indexer, file, indexer->sparql_buffer);
}

static gboolean
maybe_remove_file_event_node (TrackerIndexer *indexer,
                              QueueEvent     *event)
{
	QueueEvent *item_event;

	item_event = g_hash_table_lookup (indexer->items_by_file, event->file);

	if (item_event == event) {
		g_hash_table_remove (indexer->items_by_file, event->file);
		return TRUE;
	}

	return FALSE;
}

static gboolean
remove_items_by_file_foreach (gpointer key,
                              gpointer value,
                              gpointer user_data)
{
	GFile *file = key;
	GFile *prefix = user_data;

	return (g_file_equal (file, prefix) ||
	        g_file_has_prefix (file, prefix));
}

static void
item_queue_get_next_file (TrackerIndexer           *indexer,
                          GFile                   **file,
                          GFile                   **source_file,
                          GFileInfo               **info,
                          TrackerIndexerEventType  *type,
                          gboolean                 *attributes_update,
                          gboolean                 *is_dir)
{
	QueueEvent *event;

	*file = NULL;
	*source_file = NULL;

	event = tracker_priority_queue_pop (indexer->items, NULL);

	if (event) {
		if (event->type == TRACKER_INDEXER_EVENT_MOVED) {
			g_set_object (file, event->dest_file);
			g_set_object (source_file, event->file);
		} else {
			g_set_object (file, event->file);
		}

		*type = event->type;
		*attributes_update = event->attributes_update;
		*is_dir = event->is_dir;
		g_set_object (info, event->info);

		maybe_remove_file_event_node (indexer, event);
		queue_event_free (event);
	}
}

static gboolean
miner_handle_next_item (TrackerIndexer *indexer)
{
	GFile *file = NULL;
	GFile *source_file = NULL;
	gboolean keep_processing = TRUE;
	gboolean attributes_update = FALSE;
	gboolean is_dir = FALSE;
	TrackerIndexerEventType type;
	GFileInfo *info = NULL;

	item_queue_get_next_file (indexer, &file, &source_file, &info, &type,
	                          &attributes_update, &is_dir);

	if (indexer->timer_stopped) {
		g_timer_start (indexer->timer);
		indexer->timer_stopped = FALSE;
	}

	if (file == NULL && !indexer->extraction_timer_stopped) {
		g_timer_stop (indexer->extraction_timer);
		indexer->extraction_timer_stopped = TRUE;
	} else if (file != NULL && indexer->extraction_timer_stopped) {
		g_timer_continue (indexer->extraction_timer);
		indexer->extraction_timer_stopped = FALSE;
	}

	if (file == NULL) {
		if (!tracker_file_notifier_is_active (indexer->file_notifier)) {
			if (!indexer->flushing &&
			    tracker_task_pool_get_size (TRACKER_TASK_POOL (indexer->sparql_buffer)) == 0) {
				process_stop (indexer);
			} else {
				/* Flush any possible pending update here */
				if (tracker_sparql_buffer_flush (indexer->sparql_buffer,
								 "Queue handlers NONE",
								 sparql_buffer_flush_cb,
								 indexer))
					indexer->flushing = TRUE;
			}
		}

		/* No more files left to process */
		return FALSE;
	}

	/* Handle queues */
	switch (type) {
	case TRACKER_INDEXER_EVENT_MOVED:
		item_move (indexer, file, source_file, is_dir);
		break;
	case TRACKER_INDEXER_EVENT_DELETED:
		item_remove (indexer, file, is_dir);
		break;
	case TRACKER_INDEXER_EVENT_CREATED:
		item_add_or_update (indexer, file, info, FALSE, TRUE);
		break;
	case TRACKER_INDEXER_EVENT_UPDATED:
		item_add_or_update (indexer, file, info, attributes_update, FALSE);
		break;
	case TRACKER_INDEXER_EVENT_FINISH_DIRECTORY:
		item_finish_directory (indexer, file);
		break;
	default:
		g_assert_not_reached ();
	}

	if (tracker_task_pool_limit_reached (TRACKER_TASK_POOL (indexer->sparql_buffer))) {
		if (tracker_sparql_buffer_flush (indexer->sparql_buffer,
						 "SPARQL buffer limit reached",
						 sparql_buffer_flush_cb,
						 indexer)) {
			indexer->flushing = TRUE;
		} else {
			/* If we cannot flush, wait for the pending operations
			 * to finish.
			 */
			keep_processing = FALSE;
		}
	}

	check_notifier_high_water (indexer);

	g_clear_object (&file);
	g_clear_object (&source_file);
	g_clear_object (&info);

	return keep_processing;
}

static gboolean
item_queue_handlers_cb (gpointer user_data)
{
	TrackerIndexer *indexer = user_data;
	gint i;

	for (i = 0; i < MAX_SIMULTANEOUS_ITEMS; i++) {
		if (!miner_handle_next_item (indexer)) {
			indexer->item_queues_handler_id = 0;
			return G_SOURCE_REMOVE;
		}
	}

	return G_SOURCE_CONTINUE;
}

static gboolean
update_status_cb (gpointer user_data)
{
	TrackerIndexer *indexer = user_data;
	g_autofree char *status = NULL;
	guint files_found, files_updated, files_ignored, files_reindexed;
	TrackerFileNotifierStatus notifier_status;
	GFile *current_root;

	if (tracker_file_notifier_get_status (indexer->file_notifier,
	                                      &notifier_status,
	                                      &current_root,
	                                      &files_found,
	                                      &files_updated,
	                                      &files_ignored,
	                                      &files_reindexed)) {
		g_autofree char *uri = NULL;
		GString *str;

		str = g_string_new (NULL);
		uri = g_file_get_uri (current_root);

		if (notifier_status == TRACKER_FILE_NOTIFIER_STATUS_INDEXING)
			g_string_append_printf (str, "Indexing '%s'. ", uri);
		else if (notifier_status == TRACKER_FILE_NOTIFIER_STATUS_CHECKING)
			g_string_append_printf (str, "Checking '%s'. ", uri);

		if (files_found > 0)
			g_string_append_printf (str, "Found: %d. ", files_found);
		if (files_updated > 0)
			g_string_append_printf (str, "Updated: %d. ", files_updated);
		if (files_reindexed > 0)
			g_string_append_printf (str, "Re-indexed: %d. ", files_reindexed);
		if (files_ignored > 0)
			g_string_append_printf (str, "Ignored: %d. ", files_ignored);

		status = g_string_free (str, FALSE);
	} else {
		guint elems_left;

		elems_left =
			tracker_priority_queue_get_length (indexer->items) +
			tracker_task_pool_get_size (TRACKER_TASK_POOL (indexer->sparql_buffer));

		if (elems_left > 0)
			status = g_strdup_printf ("Processing %d updates…", elems_left);
	}

	if (status != NULL) {
		g_object_set (indexer,
		              "status", status,
		              "progress", 0.0,
		              "remaining-time", -1,
		              NULL);
	}

	return G_SOURCE_CONTINUE;
}

static void
queue_handler_set_up (TrackerIndexer *indexer)
{
	guint source_id;
	guint interval;

	g_assert (indexer->item_queues_handler_id == 0);
	interval = TRACKER_CRAWLER_MAX_TIMEOUT_INTERVAL * indexer->throttle;

	if (interval == 0) {
		source_id = g_idle_add_full (TRACKER_TASK_PRIORITY,
		                             item_queue_handlers_cb,
		                             indexer, NULL);
	} else {
		source_id = g_timeout_add_full (TRACKER_TASK_PRIORITY, interval,
		                                item_queue_handlers_cb,
		                                indexer, NULL);
	}

	indexer->item_queues_handler_id = source_id;
	set_active (indexer, TRUE);
}

static void
queue_handler_maybe_set_up (TrackerIndexer *indexer)
{
	TRACKER_NOTE (MINER_FS_EVENTS, g_message (EVENT_QUEUE_LOG_PREFIX "Setting up queue handlers..."));

	if (indexer->item_queues_handler_id != 0) {
		TRACKER_NOTE (MINER_FS_EVENTS, g_message (EVENT_QUEUE_LOG_PREFIX "   cancelled: already one active"));
		return;
	}

	if (indexer->is_paused) {
		TRACKER_NOTE (MINER_FS_EVENTS, g_message (EVENT_QUEUE_LOG_PREFIX "   cancelled: paused"));
		return;
	}

	/* Already processing max number of sparql updates */
	if (tracker_task_pool_limit_reached (TRACKER_TASK_POOL (indexer->sparql_buffer))) {
		TRACKER_NOTE (MINER_FS_EVENTS,
		              g_message (EVENT_QUEUE_LOG_PREFIX "   cancelled: pool limit reached (sparql buffer: %u)",
		                         tracker_task_pool_get_limit (TRACKER_TASK_POOL (indexer->sparql_buffer))));
		return;
	}

	if (indexer->status_idle_id == 0) {
		indexer->status_idle_id = g_timeout_add (250,
		                                         update_status_cb,
		                                         indexer);
		update_status_cb (indexer);
	}

	TRACKER_NOTE (MINER_FS_EVENTS, g_message (EVENT_QUEUE_LOG_PREFIX "   scheduled in idle"));
	queue_handler_set_up (indexer);
}

static gint
indexer_get_queue_priority (TrackerIndexer *indexer,
                            GFile          *file)
{
	TrackerDirectoryFlags flags;

	tracker_indexing_tree_get_root (indexer->indexing_tree,
	                                file, NULL, &flags);

	return (flags & TRACKER_DIRECTORY_FLAG_PRIORITY) ?
	        G_PRIORITY_HIGH : G_PRIORITY_DEFAULT;
}

static void
indexer_queue_event (TrackerIndexer *indexer,
                     QueueEvent     *event,
                     guint           priority)
{
	QueueEvent *old = NULL;

	old = g_hash_table_lookup (indexer->items_by_file, event->file);

	if (old) {
		QueueCoalesceAction action;
		QueueEvent *replacement = NULL;

		action = queue_event_coalesce (old, event, &replacement);

		if (action & QUEUE_ACTION_DELETE_FIRST) {
			g_hash_table_remove (indexer->items_by_file, old->file);
			tracker_priority_queue_remove_node (indexer->items,
							    old->queue_node);
			queue_event_free (old);
		}

		if (action & QUEUE_ACTION_DELETE_SECOND) {
			queue_event_free (event);
			event = NULL;
		}

		if (replacement)
			event = replacement;
	}

	if (event) {
		if (event->is_dir &&
		    event->type == TRACKER_INDEXER_EVENT_DELETED) {
			/* Attempt to optimize by removing any children
			 * of this directory from being processed.
			 */
			g_hash_table_foreach_remove (indexer->items_by_file,
			                             remove_items_by_file_foreach,
			                             event->file);
			tracker_priority_queue_foreach_remove (indexer->items,
							       (GEqualFunc) queue_event_is_equal_or_descendant,
							       event->file,
							       (GDestroyNotify) queue_event_free);
		}

		TRACKER_NOTE (MINER_FS_EVENTS, debug_print_event (event));

		event->queue_node =
			tracker_priority_queue_add (indexer->items, event, priority);

		if (event->type == TRACKER_INDEXER_EVENT_MOVED) {
			if (event->is_dir) {
				g_hash_table_foreach_remove (indexer->items_by_file,
				                             remove_items_by_file_foreach,
				                             event->dest_file);
			} else {
				g_hash_table_remove (indexer->items_by_file, event->dest_file);
			}

			g_hash_table_remove (indexer->items_by_file, event->file);
		} else {
			g_hash_table_replace (indexer->items_by_file,
			                      g_object_ref (event->file),
			                      event);
		}

		queue_handler_maybe_set_up (indexer);
		check_notifier_high_water (indexer);
	}
}

static void
file_notifier_file_created (TrackerFileNotifier  *notifier,
                            GFile                *file,
                            GFileInfo            *info,
                            gpointer              user_data)
{
	TrackerIndexer *indexer = user_data;
	QueueEvent *event;

	event = queue_event_new (TRACKER_INDEXER_EVENT_CREATED, file, info);
	indexer_queue_event (indexer, event, indexer_get_queue_priority (indexer, file));
}

static void
file_notifier_file_deleted (TrackerFileNotifier  *notifier,
                            GFile                *file,
                            gboolean              is_dir,
                            gpointer              user_data)
{
	TrackerIndexer *indexer = user_data;
	QueueEvent *event;

	event = queue_event_new (TRACKER_INDEXER_EVENT_DELETED, file, NULL);
	event->is_dir = !!is_dir;
	indexer_queue_event (indexer, event, indexer_get_queue_priority (indexer, file));
}

static void
file_notifier_file_updated (TrackerFileNotifier  *notifier,
                            GFile                *file,
                            GFileInfo            *info,
                            gboolean              attributes_only,
                            gpointer              user_data)
{
	TrackerIndexer *indexer = user_data;
	QueueEvent *event;

	event = queue_event_new (TRACKER_INDEXER_EVENT_UPDATED, file, info);
	event->attributes_update = attributes_only;
	indexer_queue_event (indexer, event, indexer_get_queue_priority (indexer, file));
}

static void
file_notifier_file_moved (TrackerFileNotifier *notifier,
                          GFile               *source,
                          GFile               *dest,
                          gboolean             is_dir,
                          gpointer             user_data)
{
	TrackerIndexer *indexer = user_data;
	QueueEvent *event;

	event = queue_event_moved_new (source, dest, is_dir);
	indexer_queue_event (indexer, event, indexer_get_queue_priority (indexer, source));
}

static void
file_notifier_directory_finished (TrackerFileNotifier *notifier,
                                  GFile               *directory,
                                  gpointer             user_data)
{
	TrackerIndexer *indexer = user_data;
	QueueEvent *event;

	event = queue_event_new (TRACKER_INDEXER_EVENT_FINISH_DIRECTORY, directory, NULL);
	indexer_queue_event (indexer, event, indexer_get_queue_priority (indexer, directory));
}

static void
file_notifier_finished (TrackerFileNotifier *notifier,
                        gpointer             user_data)
{
	TrackerIndexer *indexer = user_data;

	queue_handler_maybe_set_up (indexer);
}

static void
indexing_tree_directory_added (TrackerIndexingTree *indexing_tree,
                               GFile               *directory,
                               gpointer             user_data)
{
	TrackerIndexer *indexer = user_data;
	TrackerDirectoryFlags flags;

	tracker_indexing_tree_get_root (indexing_tree, directory, NULL, &flags);

	if (!!(flags & TRACKER_DIRECTORY_FLAG_IS_VOLUME)) {
		TrackerSparqlConnection *conn;
		TrackerBatch *batch;
		g_autoptr (GError) error = NULL;

		conn = tracker_miner_get_connection (user_data);
		batch = tracker_sparql_connection_create_batch (conn);
		set_up_mount_point (indexer, directory, TRUE, batch);

		if (!tracker_batch_execute (batch, NULL, &error)) {
			g_critical ("Could not set mount point in database, %s",
			            error->message);
		}
	}
}

static void
indexing_tree_directory_removed (TrackerIndexingTree *indexing_tree,
                                 GFile               *directory,
                                 gpointer             user_data)
{
	TrackerIndexer *indexer = user_data;
	GTimer *timer = g_timer_new ();
	TrackerDirectoryFlags flags;
	TrackerSparqlConnection *conn;
	g_autoptr (TrackerBatch) batch = NULL;
	g_autoptr (GError) error = NULL;

	TRACKER_NOTE (MINER_FS_EVENTS, g_message ("  Cancelled processing pool tasks at %f\n", g_timer_elapsed (timer, NULL)));

	tracker_indexing_tree_get_root (indexing_tree, directory, NULL, &flags);
	conn = tracker_miner_get_connection (TRACKER_MINER (indexer));
	batch = tracker_sparql_connection_create_batch (conn);

	if ((flags & TRACKER_DIRECTORY_FLAG_PRESERVE) != 0)
		set_up_mount_point (indexer, directory, FALSE, batch);
	else
		delete_index_root (indexer, directory, batch);

	if (!tracker_batch_execute (batch, NULL, &error))
		g_warning ("Error updating indexed folder: %s", error->message);

	/* Remove anything contained in the removed directory
	 * from all relevant processing queues.
	 */
	g_hash_table_foreach_remove (indexer->items_by_file,
	                             remove_items_by_file_foreach,
	                             directory);
	tracker_priority_queue_foreach_remove (indexer->items,
	                                       (GEqualFunc) queue_event_is_equal_or_descendant,
	                                       directory,
	                                       (GDestroyNotify) queue_event_free);

	TRACKER_NOTE (MINER_FS_EVENTS, g_message ("  Removed files at %f\n", g_timer_elapsed (timer, NULL)));
	g_timer_destroy (timer);
}

const gchar *
tracker_indexer_get_content_uri (TrackerIndexer *indexer,
                                 GFile          *file)
{
	g_autoptr (GFileInfo) info = NULL;
	gchar *str;

	g_return_val_if_fail (TRACKER_IS_INDEXER (indexer), NULL);
	g_return_val_if_fail (G_IS_FILE (file), NULL);

	if (tracker_lru_find (indexer->urn_lru, file, (gpointer*) &str))
		return str;

	info = g_file_query_info (file,
	                          G_FILE_ATTRIBUTE_STANDARD_TYPE ","
	                          G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN ","
	                          G_FILE_ATTRIBUTE_ID_FILESYSTEM ","
	                          G_FILE_ATTRIBUTE_UNIX_INODE,
	                          G_FILE_QUERY_INFO_NONE,
	                          NULL,
	                          NULL);
	if (!info)
		return NULL;

	if (!tracker_indexing_tree_file_is_indexable (indexer->indexing_tree, file, info))
		return NULL;

	str = tracker_indexer_get_content_identifier (indexer, file, info);
	tracker_lru_add (indexer->urn_lru, g_object_ref (file), str);

	return str;
}

TrackerIndexingTree *
tracker_indexer_get_indexing_tree (TrackerIndexer *indexer)
{
	g_return_val_if_fail (TRACKER_IS_INDEXER (indexer), NULL);

	return indexer->indexing_tree;
}

char *
tracker_indexer_get_file_resource_uri (TrackerIndexer *indexer,
                                       GFile          *file)
{
	return tracker_file_notifier_get_file_resource_uri (indexer->file_notifier,
	                                                    file);
}

TrackerIndexer *
tracker_indexer_new (TrackerSparqlConnection  *connection,
                     TrackerIndexingTree      *indexing_tree,
                     TrackerMonitor           *monitor,
                     TrackerErrorReport       *error_reports,
                     GFile                    *root)
{
	g_return_val_if_fail (TRACKER_IS_SPARQL_CONNECTION (connection), NULL);

	return g_object_new (TRACKER_TYPE_INDEXER,
	                     "connection", connection,
	                     "indexing-tree", indexing_tree,
	                     "monitor", monitor,
	                     "error-reports", error_reports,
	                     "root", root,
	                     NULL);
}
