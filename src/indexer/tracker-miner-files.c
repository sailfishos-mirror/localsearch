/*
 * Copyright (C) 2008, Nokia <ivan.frade@nokia.com>
 *
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

#include <glib/gstdio.h>

#include <gio/gio.h>

#include <tracker-common.h>

#include "tracker-miner-files.h"
#include "tracker-miner-files-methods.h"
#include "tracker-config.h"
#include "tracker-storage.h"
#include "tracker-extract-watchdog.h"
#include "tracker-utils.h"

#define DISK_SPACE_CHECK_FREQUENCY 10
#define SECONDS_PER_DAY 86400

#define DEFAULT_GRAPH "tracker:FileSystem"

#define FILE_ATTRIBUTES	  \
	G_FILE_ATTRIBUTE_UNIX_IS_MOUNTPOINT "," \
	G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN "," \
	G_FILE_ATTRIBUTE_STANDARD_NAME "," \
	G_FILE_ATTRIBUTE_STANDARD_TYPE "," \
	G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME "," \
	G_FILE_ATTRIBUTE_STANDARD_SIZE "," \
	G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN "," \
	G_FILE_ATTRIBUTE_TIME_MODIFIED "," \
	G_FILE_ATTRIBUTE_TIME_CREATED "," \
	G_FILE_ATTRIBUTE_TIME_ACCESS

#define TRACKER_MINER_FILES_GET_PRIVATE(o) (tracker_miner_files_get_instance_private (TRACKER_MINER_FILES (o)))

typedef struct _TrackerMinerFiles TrackerMinerFiles;
typedef struct _TrackerMinerFilesPrivate TrackerMinerFilesPrivate;

struct _TrackerMinerFiles {
	TrackerMinerFS parent_instance;
	TrackerMinerFilesPrivate *private;
};

static GQuark miner_files_error_quark = 0;

struct _TrackerMinerFilesPrivate {
	TrackerConfig *config;
	TrackerStorage *storage;

	TrackerExtractWatchdog *extract_watchdog;
	guint grace_period_timeout_id;

	GSettings *extract_settings;
	GList *allowed_text_patterns;

	guint disk_space_check_id;
	gboolean disk_space_pause;

	gboolean low_battery_pause;
	gboolean initial_index;

#ifdef HAVE_POWER
	TrackerPower *power;
#endif /* HAVE_POWER) */

	guint stale_volumes_check_id;
};

enum {
	PROP_0,
	PROP_CONFIG,
	PROP_STORAGE,
	PROP_INITIAL_INDEX,
};

#define TEXT_ALLOWLIST "text-allowlist"

static void        miner_files_set_property             (GObject              *object,
                                                         guint                 param_id,
                                                         const GValue         *value,
                                                         GParamSpec           *pspec);
static void        miner_files_get_property             (GObject              *object,
                                                         guint                 param_id,
                                                         GValue               *value,
                                                         GParamSpec           *pspec);
static void        miner_files_constructed              (GObject              *object);
static void        miner_files_finalize                 (GObject              *object);
#ifdef HAVE_POWER
static void        check_battery_status                 (TrackerMinerFiles    *fs);
static void        battery_status_cb                    (GObject              *object,
                                                         GParamSpec           *pspec,
                                                         gpointer              user_data);
#endif /* HAVE_POWER */
static void        init_index_roots                     (TrackerMinerFiles    *miner);
static void        init_stale_volume_removal            (TrackerMinerFiles    *miner);
static void        disk_space_check_start               (TrackerMinerFiles    *mf);
static void        disk_space_check_stop                (TrackerMinerFiles    *mf);
static void        low_disk_space_limit_cb              (GObject              *gobject,
                                                         GParamSpec           *arg1,
                                                         gpointer              user_data);

static void        miner_files_process_file             (TrackerMinerFS       *fs,
                                                         GFile                *file,
                                                         GFileInfo            *info,
                                                         TrackerSparqlBuffer  *buffer,
                                                         gboolean              create);
static void        miner_files_process_file_attributes  (TrackerMinerFS       *fs,
                                                         GFile                *file,
                                                         GFileInfo            *info,
                                                         TrackerSparqlBuffer  *buffer);
static void        miner_files_remove_children          (TrackerMinerFS       *fs,
                                                         GFile                *file,
                                                         TrackerSparqlBuffer  *buffer);
static void        miner_files_remove_file              (TrackerMinerFS       *fs,
                                                         GFile                *file,
                                                         TrackerSparqlBuffer  *buffer,
                                                         gboolean              is_dir);
static void        miner_files_move_file                (TrackerMinerFS       *fs,
                                                         GFile                *file,
                                                         GFile                *source_file,
                                                         TrackerSparqlBuffer  *buffer,
                                                         gboolean              recursive);
static void        miner_files_finish_directory         (TrackerMinerFS       *fs,
                                                         GFile                *directory,
                                                         TrackerSparqlBuffer  *buffer);
static void        miner_files_finished                 (TrackerMinerFS       *fs);

static void        miner_files_in_removable_media_remove_by_date  (TrackerMinerFiles  *miner,
                                                                   GDateTime          *datetime);

G_DEFINE_TYPE_WITH_PRIVATE (TrackerMinerFiles, tracker_miner_files, TRACKER_TYPE_MINER_FS)

static void
miner_files_started (TrackerMiner *miner)
{
	TRACKER_MINER_CLASS (tracker_miner_files_parent_class)->started (miner);
	init_index_roots (TRACKER_MINER_FILES (miner));
}

static gchar *
miner_files_get_content_identifier (TrackerMinerFS *fs,
				    GFile          *file,
				    GFileInfo      *info)
{
	return tracker_miner_files_get_content_identifier (TRACKER_MINER_FILES (fs),
	                                                   file, info);
}

static void
tracker_miner_files_class_init (TrackerMinerFilesClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	TrackerMinerClass *miner_class = TRACKER_MINER_CLASS (klass);
	TrackerMinerFSClass *miner_fs_class = TRACKER_MINER_FS_CLASS (klass);

	object_class->constructed = miner_files_constructed;
	object_class->finalize = miner_files_finalize;
	object_class->get_property = miner_files_get_property;
	object_class->set_property = miner_files_set_property;

	miner_class->started = miner_files_started;

	miner_fs_class->process_file = miner_files_process_file;
	miner_fs_class->process_file_attributes = miner_files_process_file_attributes;
	miner_fs_class->finished = miner_files_finished;
	miner_fs_class->remove_file = miner_files_remove_file;
	miner_fs_class->remove_children = miner_files_remove_children;
	miner_fs_class->move_file = miner_files_move_file;
	miner_fs_class->finish_directory = miner_files_finish_directory;
	miner_fs_class->get_content_identifier = miner_files_get_content_identifier;

	g_object_class_install_property (object_class,
	                                 PROP_CONFIG,
	                                 g_param_spec_object ("config",
	                                                      "Config",
	                                                      "Config",
	                                                      TRACKER_TYPE_CONFIG,
	                                                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property (object_class,
	                                 PROP_STORAGE,
	                                 g_param_spec_object ("storage",
	                                                      NULL, NULL,
	                                                      TRACKER_TYPE_STORAGE,
	                                                      G_PARAM_READWRITE |
	                                                      G_PARAM_CONSTRUCT_ONLY |
	                                                      G_PARAM_STATIC_STRINGS));
	g_object_class_install_property (object_class,
	                                 PROP_INITIAL_INDEX,
	                                 g_param_spec_boolean ("initial-index",
	                                                       NULL, NULL, FALSE,
	                                                       G_PARAM_WRITABLE |
	                                                       G_PARAM_CONSTRUCT_ONLY |
	                                                       G_PARAM_STATIC_STRINGS));

	miner_files_error_quark = g_quark_from_static_string ("TrackerMinerFiles");
}

static void
tracker_miner_files_check_unextracted (TrackerMinerFiles *mf)
{
	g_debug ("Starting extractor");
	tracker_extract_watchdog_ensure_started (mf->private->extract_watchdog);
}

static gboolean
extractor_lost_timeout_cb (gpointer user_data)
{
	TrackerMinerFiles *mf = user_data;

	tracker_miner_files_check_unextracted (mf);
	mf->private->grace_period_timeout_id = 0;
	return G_SOURCE_REMOVE;
}


static void
on_extractor_lost (TrackerExtractWatchdog *watchdog,
                   TrackerMinerFiles      *mf)
{
	g_debug ("tracker-extract vanished, maybe restarting.");

	/* Give a period of grace before restarting, so we allow replacing
	 * from eg. a terminal.
	 */
	mf->private->grace_period_timeout_id =
		g_timeout_add_seconds (1, extractor_lost_timeout_cb, mf);
}

static void
on_extractor_status (TrackerExtractWatchdog *watchdog,
                     const gchar            *status,
                     gdouble                 progress,
                     gint                    remaining,
                     TrackerMinerFiles      *mf)
{
	if (!tracker_miner_is_paused (TRACKER_MINER (mf))) {
		g_object_set (mf,
		              "status", status,
		              "progress", progress,
		              "remaining-time", remaining,
		              NULL);
	}
}

static void
tracker_miner_files_init (TrackerMinerFiles *mf)
{
	TrackerMinerFilesPrivate *priv;

	priv = mf->private = TRACKER_MINER_FILES_GET_PRIVATE (mf);

#ifdef HAVE_POWER
	priv->power = tracker_power_new ();

	if (priv->power) {
		g_signal_connect (priv->power, "notify::on-low-battery",
		                  G_CALLBACK (battery_status_cb),
		                  mf);
		g_signal_connect (priv->power, "notify::on-battery",
		                  G_CALLBACK (battery_status_cb),
		                  mf);
	}
#endif /* HAVE_POWER */
}

static void
removable_days_threshold_changed (TrackerMinerFiles *mf)
{
	/* Check if the stale volume removal configuration changed from enabled to disabled
	 * or from disabled to enabled */
	if (g_settings_get_int (G_SETTINGS (mf->private->config), "removable-days-threshold") == 0 &&
	    mf->private->stale_volumes_check_id != 0) {
		/* From having the check enabled to having it disabled, remove the timeout */
		TRACKER_NOTE (CONFIG, g_message ("Stale volume removal now disabled, removing timeout"));
		g_source_remove (mf->private->stale_volumes_check_id);
		mf->private->stale_volumes_check_id = 0;
	} else if (g_settings_get_int (G_SETTINGS (mf->private->config), "removable-days-threshold") > 0 &&
	           mf->private->stale_volumes_check_id == 0) {
		TRACKER_NOTE (CONFIG, g_message ("Stale volume removal now enabled, initializing timeout"));
		/* From having the check disabled to having it enabled, so fire up the
		 * timeout. */
		init_stale_volume_removal (TRACKER_MINER_FILES (mf));
	}
}

static void
miner_files_set_property (GObject      *object,
                          guint         prop_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
	TrackerMinerFilesPrivate *priv;

	priv = TRACKER_MINER_FILES_GET_PRIVATE (object);

	switch (prop_id) {
	case PROP_CONFIG:
		priv->config = g_value_dup_object (value);
		break;
	case PROP_STORAGE:
		priv->storage = g_value_dup_object (value);
		break;
	case PROP_INITIAL_INDEX:
		priv->initial_index = g_value_get_boolean (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
miner_files_get_property (GObject    *object,
                          guint       prop_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
	TrackerMinerFilesPrivate *priv;

	priv = TRACKER_MINER_FILES_GET_PRIVATE (object);

	switch (prop_id) {
	case PROP_CONFIG:
		g_value_set_object (value, priv->config);
		break;
	case PROP_STORAGE:
		g_value_set_object (value, priv->storage);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
miner_files_finalize (GObject *object)
{
	TrackerMinerFiles *mf;
	TrackerMinerFilesPrivate *priv;
	TrackerIndexingTree *indexing_tree;

	mf = TRACKER_MINER_FILES (object);
	priv = mf->private;

	indexing_tree = tracker_miner_fs_get_indexing_tree (TRACKER_MINER_FS (mf));
	g_signal_handlers_disconnect_by_data (indexing_tree, mf);

	if (priv->grace_period_timeout_id != 0) {
		g_source_remove (priv->grace_period_timeout_id);
		priv->grace_period_timeout_id = 0;
	}

	g_clear_object (&mf->private->extract_settings);
	g_list_free_full (mf->private->allowed_text_patterns, (GDestroyNotify) g_pattern_spec_free);

	g_signal_handlers_disconnect_by_func (priv->extract_watchdog,
	                                      on_extractor_lost,
	                                      NULL);
	g_clear_object (&priv->extract_watchdog);

	if (priv->config) {
		g_signal_handlers_disconnect_by_func (priv->config,
		                                      low_disk_space_limit_cb,
		                                      NULL);
		g_object_unref (priv->config);
	}

	disk_space_check_stop (TRACKER_MINER_FILES (object));

#ifdef HAVE_POWER
	if (priv->power) {
		g_object_unref (priv->power);
	}
#endif /* HAVE_POWER */

	if (priv->storage) {
		g_object_unref (priv->storage);
	}

	if (priv->stale_volumes_check_id) {
		g_source_remove (priv->stale_volumes_check_id);
		priv->stale_volumes_check_id = 0;
	}

	G_OBJECT_CLASS (tracker_miner_files_parent_class)->finalize (object);
}

static void
set_up_mount_point_cb (GObject      *source,
                       GAsyncResult *result,
                       gpointer      user_data)
{
	g_autofree GError *error = NULL;

	tracker_sparql_statement_update_finish (TRACKER_SPARQL_STATEMENT (source),
	                                        result, &error);

	if (error) {
		g_critical ("Could not set mount point in database, %s",
		            error->message);
	}
}

static void
set_up_mount_point (TrackerMinerFiles *miner,
                    GFile             *mount_point,
                    gboolean           mounted,
                    TrackerBatch      *batch)
{
	TrackerSparqlConnection *conn;
	g_autoptr (TrackerSparqlStatement) stmt = NULL;
	g_autofree gchar *uri = NULL;
	g_autoptr (GDateTime) now = NULL;

	uri = g_file_get_uri (mount_point);
	now = g_date_time_new_now_utc ();

	g_debug ("Mount point state (%s) being set in DB for mount_point '%s'",
	         mounted ? "MOUNTED" : "UNMOUNTED",
	         uri);

	conn = tracker_miner_get_connection (TRACKER_MINER (miner));
	stmt = tracker_load_statement (conn, "update-mountpoint.rq", NULL);

	if (batch) {
		tracker_batch_add_statement (batch, stmt,
		                             "mountPoint", G_TYPE_STRING, uri,
		                             "mounted", G_TYPE_BOOLEAN, mounted,
		                             "currentDate", G_TYPE_DATE_TIME, now,
		                             NULL);
	} else {
		tracker_sparql_statement_bind_string (stmt, "mountPoint", uri);
		tracker_sparql_statement_bind_boolean (stmt, "mounted", mounted);
		tracker_sparql_statement_bind_datetime (stmt, "currentDate", now);
		tracker_sparql_statement_update_async (stmt,
		                                       NULL,
		                                       set_up_mount_point_cb,
		                                       NULL);
	}
}

static void
delete_index_root (TrackerMinerFiles *miner,
                   GFile             *mount_point,
                   TrackerBatch      *batch)
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
init_index_roots_cb (GObject      *source,
                     GAsyncResult *result,
                     gpointer      user_data)
{
	g_autofree GError *error = NULL;

	tracker_batch_execute_finish (TRACKER_BATCH (source), result, &error);

	if (error) {
		g_critical ("Could not initialize currently active mount points: %s",
		            error->message);
	} else {
		init_stale_volume_removal (user_data);
	}
}

static void
init_index_roots (TrackerMinerFiles *miner_files)
{
	TrackerMiner *miner = TRACKER_MINER (miner_files);
	TrackerSparqlConnection *conn;
	g_autoptr (TrackerSparqlStatement) stmt = NULL;
	TrackerIndexingTree *indexing_tree;
	g_autoptr (GList) roots = NULL;
	g_autoptr (GHashTable) handled = NULL;
	g_autoptr (TrackerBatch) batch = NULL;
	g_autoptr (GError) error = NULL;
	g_autoptr (TrackerSparqlCursor) cursor = NULL;
	GList *l;

	g_debug ("Initializing mount points...");

	conn = tracker_miner_get_connection (miner);
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
	indexing_tree = tracker_miner_fs_get_indexing_tree (TRACKER_MINER_FS (miner));
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

		if (tracker_indexing_tree_file_is_root (indexing_tree, file)) {
			/* Directory is indexed and configured */
			if (is_removable) {
				set_up_mount_point (TRACKER_MINER_FILES (miner),
				                    file,
				                    TRUE,
				                    batch);
			}
		} else {
			/* Directory is indexed, but no longer configured */
			if (g_settings_get_int (G_SETTINGS (miner_files->private->config), "removable-days-threshold") > 0 &&
			    (is_removable &&
			     g_settings_get_boolean (G_SETTINGS (miner_files->private->config), "index-removable-devices"))) {
				/* Preserve */
				set_up_mount_point (TRACKER_MINER_FILES (miner),
				                    file, FALSE, batch);
			} else {
				/* Not a removable device to preserve, or a no
				 * longer configured folder.
				 */
				delete_index_root (TRACKER_MINER_FILES (miner),
				                   file, batch);
			}
		}
	}

	roots = tracker_indexing_tree_list_roots (indexing_tree);

	for (l = roots; l; l = l->next) {
		TrackerStorage *storage = miner_files->private->storage;
		TrackerStorageType type;
		GFile *file = l->data;

		if (g_hash_table_contains (handled, file))
			continue;

		type = tracker_storage_get_type_for_file (storage, file);

		if ((type & TRACKER_STORAGE_REMOVABLE) != 0)
			set_up_mount_point (miner_files, file, TRUE, NULL);
	}

	tracker_batch_execute_async (batch,
	                             NULL,
	                             init_index_roots_cb,
	                             miner);
}

static gboolean
cleanup_stale_removable_volumes_cb (gpointer user_data)
{
	TrackerMinerFiles *miner = TRACKER_MINER_FILES (user_data);
	g_autoptr (GDateTime) now = NULL, n_days_ago = NULL;
	gint n_days_threshold;

	n_days_threshold = g_settings_get_int (G_SETTINGS (miner->private->config), "removable-days-threshold");

	if (n_days_threshold == 0)
		return TRUE;

	g_debug ("Running stale volumes check...");

	now = g_date_time_new_now_utc ();
	n_days_ago = g_date_time_add_days (now, -n_days_threshold);
	miner_files_in_removable_media_remove_by_date (miner, n_days_ago);

	return TRUE;
}

static void
init_stale_volume_removal (TrackerMinerFiles *miner)
{
	/* If disabled, make sure we don't do anything */
	if (g_settings_get_int (G_SETTINGS (miner->private->config), "removable-days-threshold") == 0) {
		g_debug ("Stale volume check is disabled");
		return;
	}

	/* Run right away the first check */
	cleanup_stale_removable_volumes_cb (miner);

	g_debug ("Initializing stale volume check timeout...");

	/* Then, setup new timeout event every day */
	miner->private->stale_volumes_check_id =
		g_timeout_add_seconds (SECONDS_PER_DAY + 1,
		                       cleanup_stale_removable_volumes_cb,
		                       miner);
}

#ifdef HAVE_POWER

static void
set_up_throttle (TrackerMinerFiles *mf,
                 gboolean           enable)
{
	gdouble throttle = 0;

	throttle = enable ? 0.25 : 0;
	g_debug ("Setting new throttle to %0.3f", throttle);
	tracker_miner_fs_set_throttle (TRACKER_MINER_FS (mf), throttle);
}

static void
check_battery_status (TrackerMinerFiles *mf)
{
	gboolean on_battery, on_low_battery;
	gboolean should_pause = FALSE;
	gboolean should_throttle = FALSE;

	if (mf->private->power == NULL) {
		return;
	}

	on_low_battery = tracker_power_get_on_low_battery (mf->private->power);
	on_battery = tracker_power_get_on_battery (mf->private->power);

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

	if (should_pause) {
		/* Don't try to pause again */
		if (!mf->private->low_battery_pause) {
			mf->private->low_battery_pause = TRUE;
			tracker_miner_pause (TRACKER_MINER (mf));
		}
	} else {
		/* Don't try to resume again */
		if (mf->private->low_battery_pause) {
			tracker_miner_resume (TRACKER_MINER (mf));
			mf->private->low_battery_pause = FALSE;
		}
	}

	set_up_throttle (mf, should_throttle);
}

/* Called when battery status change is detected */
static void
battery_status_cb (GObject    *object,
                   GParamSpec *pspec,
                   gpointer    user_data)
{
	TrackerMinerFiles *mf = user_data;

	check_battery_status (mf);
}

#endif /* HAVE_POWER */

static GFile *
get_cache_dir (TrackerMinerFiles *mf)
{
	TrackerSparqlConnection *conn;
	GFile *cache;

	conn = tracker_miner_get_connection (TRACKER_MINER (mf));
	g_object_get (conn, "store-location", &cache, NULL);

	return cache;
}

static gboolean
disk_space_check (TrackerMinerFiles *mf)
{
	GFile *file;
	gint limit;
	gchar *data_dir;
	gdouble remaining;

	limit = g_settings_get_int (G_SETTINGS (mf->private->config), "low-disk-space-limit");

	if (limit < 1) {
		return FALSE;
	}

	/* Get % of remaining space in the partition where the cache is */
	file = get_cache_dir (mf);
	data_dir = g_file_get_path (file);
	remaining = tracker_file_system_get_remaining_space_percentage (data_dir);
	g_free (data_dir);
	g_object_unref (file);

	if (remaining <= limit) {
		g_message ("WARNING: Available disk space (%lf%%) is below "
		           "configured threshold for acceptable working (%d%%)",
		           remaining, limit);
		return TRUE;
	}

	return FALSE;
}

static gboolean
disk_space_check_cb (gpointer user_data)
{
	TrackerMinerFiles *mf = user_data;

	if (disk_space_check (mf)) {
		/* Don't try to pause again */
		if (!mf->private->disk_space_pause) {
			mf->private->disk_space_pause = TRUE;
			tracker_miner_pause (TRACKER_MINER (mf));
		}
	} else {
		/* Don't try to resume again */
		if (mf->private->disk_space_pause) {
			tracker_miner_resume (TRACKER_MINER (mf));
			mf->private->disk_space_pause = FALSE;
		}
	}

	return TRUE;
}

static void
disk_space_check_start (TrackerMinerFiles *mf)
{
	gint limit;

	if (mf->private->disk_space_check_id != 0) {
		return;
	}

	limit = g_settings_get_int (G_SETTINGS (mf->private->config), "low-disk-space-limit");

	if (limit != -1) {
		TRACKER_NOTE (CONFIG, g_message ("Starting disk space check for every %d seconds",
		                      DISK_SPACE_CHECK_FREQUENCY));
		mf->private->disk_space_check_id =
			g_timeout_add_seconds (DISK_SPACE_CHECK_FREQUENCY,
			                       disk_space_check_cb,
			                       mf);

		/* Call the function now too to make sure we have an
		 * initial value too!
		 */
		disk_space_check_cb (mf);
	} else {
		TRACKER_NOTE (CONFIG, g_message ("Not setting disk space, configuration is set to -1 (disabled)"));
	}
}

static void
disk_space_check_stop (TrackerMinerFiles *mf)
{
	if (mf->private->disk_space_check_id) {
		TRACKER_NOTE (CONFIG, g_message ("Stopping disk space check"));
		g_source_remove (mf->private->disk_space_check_id);
		mf->private->disk_space_check_id = 0;
	}
}

static void
low_disk_space_limit_cb (GObject    *gobject,
                         GParamSpec *arg1,
                         gpointer    user_data)
{
	TrackerMinerFiles *mf = user_data;

	disk_space_check_cb (mf);
}

static void
indexing_tree_directory_added_cb (TrackerIndexingTree *indexing_tree,
                                  GFile               *directory,
                                  gpointer             user_data)
{
	TrackerMinerFiles *miner_files = user_data;
	TrackerStorage *storage = miner_files->private->storage;
	TrackerStorageType type;

	type = tracker_storage_get_type_for_file (storage, directory);

	if ((type & TRACKER_STORAGE_REMOVABLE) != 0)
		set_up_mount_point (miner_files, directory, TRUE, NULL);
}

static void
indexing_tree_directory_removed_cb (TrackerIndexingTree *indexing_tree,
                                    GFile               *directory,
                                    gpointer             user_data)
{
	TrackerMinerFiles *miner_files = user_data;
	TrackerStorage *storage = miner_files->private->storage;
	TrackerStorageType type;
	TrackerSparqlConnection *conn;
	g_autoptr (TrackerBatch) batch = NULL;
	g_autoptr (GError) error = NULL;
	gboolean delete = FALSE, update_mount = FALSE;

	type = tracker_storage_get_type_for_file (storage, directory);

	if ((type & TRACKER_STORAGE_REMOVABLE) != 0) {
		if (!g_settings_get_boolean (G_SETTINGS (miner_files->private->config), "index-removable-devices"))
			delete = TRUE;
		else if (g_settings_get_int (G_SETTINGS (miner_files->private->config), "removable-days-threshold") == 0)
			delete = TRUE;
		else
			update_mount = TRUE;
	} else {
		delete = TRUE;
	}

	conn = tracker_miner_get_connection (TRACKER_MINER (miner_files));
	batch = tracker_sparql_connection_create_batch (conn);

	if (delete)
		delete_index_root (miner_files, directory, batch);
	else if (update_mount)
		set_up_mount_point (miner_files, directory, FALSE, batch);

	if (!tracker_batch_execute (batch, NULL, &error))
		g_warning ("Error updating indexed folder: %s", error->message);
}

static void
miner_files_process_file (TrackerMinerFS       *fs,
                          GFile                *file,
                          GFileInfo            *info,
                          TrackerSparqlBuffer  *buffer,
                          gboolean              create)
{
	tracker_miner_files_process_file (fs, file, info, buffer, create);
}

static void
miner_files_process_file_attributes (TrackerMinerFS       *fs,
                                     GFile                *file,
                                     GFileInfo            *info,
                                     TrackerSparqlBuffer  *buffer)
{
	tracker_miner_files_process_file_attributes (fs, file, info, buffer);
}

static void
miner_files_finished (TrackerMinerFS *fs)
{
	tracker_miner_files_check_unextracted (TRACKER_MINER_FILES (fs));
}

static void
miner_files_remove_children (TrackerMinerFS      *fs,
                             GFile               *file,
                             TrackerSparqlBuffer *buffer)
{
	tracker_sparql_buffer_log_delete_content (buffer, file);
}

static void
miner_files_remove_file (TrackerMinerFS      *fs,
                         GFile               *file,
                         TrackerSparqlBuffer *buffer,
                         gboolean             is_dir)
{
	if (is_dir)
		tracker_sparql_buffer_log_delete_content (buffer, file);

	tracker_sparql_buffer_log_delete (buffer, file);
}

static void
miner_files_move_file (TrackerMinerFS      *fs,
                       GFile               *file,
                       GFile               *source_file,
                       TrackerSparqlBuffer *buffer,
                       gboolean             recursive)
{
	TrackerIndexingTree *indexing_tree;
	const gchar *data_source = NULL;

	indexing_tree = tracker_miner_fs_get_indexing_tree (fs);

	if (tracker_indexing_tree_file_is_root (indexing_tree, file)) {
		data_source = tracker_miner_fs_get_identifier (fs, file);
	} else {
		GFile *root;

		root = tracker_indexing_tree_get_root (indexing_tree, file, NULL, NULL);

		if (root)
			data_source = tracker_miner_fs_get_identifier (fs, root);
	}

	if (!data_source)
		return;

	tracker_sparql_buffer_log_move (buffer, source_file, file,
	                                data_source);

	if (recursive)
		tracker_sparql_buffer_log_move_content (buffer, source_file, file);
}

static void
miner_files_finish_directory (TrackerMinerFS      *fs,
                              GFile               *file,
                              TrackerSparqlBuffer *buffer)
{
	tracker_miner_files_finish_directory (fs, file, buffer);
}

static void
text_allowlist_changed_cb (GSettings         *settings,
                           const gchar       *key,
                           TrackerMinerFiles *mf)
{
	GStrv allow_list;
	gint i;

	g_list_free_full (mf->private->allowed_text_patterns, (GDestroyNotify) g_pattern_spec_free);
	mf->private->allowed_text_patterns = NULL;

	allow_list = g_settings_get_strv (settings, TEXT_ALLOWLIST);

	for (i = 0; allow_list[i]; i++) {
		mf->private->allowed_text_patterns =
			g_list_prepend (mf->private->allowed_text_patterns,
			                g_pattern_spec_new (allow_list[i]));
	}

	g_strfreev (allow_list);
}

static void
miner_files_constructed (GObject *object)
{
	TrackerMinerFiles *mf = TRACKER_MINER_FILES (object);;
	TrackerIndexingTree *indexing_tree;

	G_OBJECT_CLASS (tracker_miner_files_parent_class)->constructed (object);

	indexing_tree = tracker_miner_fs_get_indexing_tree (TRACKER_MINER_FS (object));
	g_signal_connect (indexing_tree, "directory-added",
	                  G_CALLBACK (indexing_tree_directory_added_cb), object);
	g_signal_connect (indexing_tree, "directory-removed",
	                  G_CALLBACK (indexing_tree_directory_removed_cb), object);

	/* We want to get notified when config changes */
	g_signal_connect (mf->private->config, "notify::low-disk-space-limit",
	                  G_CALLBACK (low_disk_space_limit_cb),
	                  mf);
	g_signal_connect_swapped (mf->private->config,
	                          "notify::removable-days-threshold",
	                          G_CALLBACK (removable_days_threshold_changed),
	                          mf);

#ifdef HAVE_POWER
	check_battery_status (mf);
#endif /* HAVE_POWER */

	disk_space_check_start (mf);

	mf->private->extract_watchdog =
		tracker_extract_watchdog_new (tracker_miner_get_connection (TRACKER_MINER (mf)),
		                              tracker_miner_fs_get_indexing_tree (TRACKER_MINER_FS (mf)),
		                              mf->private->storage);
	g_signal_connect (mf->private->extract_watchdog, "lost",
	                  G_CALLBACK (on_extractor_lost), mf);
	g_signal_connect (mf->private->extract_watchdog, "status",
	                  G_CALLBACK (on_extractor_status), mf);

	mf->private->extract_settings = g_settings_new ("org.freedesktop.Tracker3.Extract");
	g_signal_connect (mf->private->extract_settings, "changed::" TEXT_ALLOWLIST,
	                  G_CALLBACK (text_allowlist_changed_cb), mf);
	text_allowlist_changed_cb (mf->private->extract_settings, TEXT_ALLOWLIST, mf);
}

TrackerMiner *
tracker_miner_files_new (TrackerSparqlConnection  *connection,
                         TrackerIndexingTree      *indexing_tree,
                         TrackerStorage           *storage,
                         TrackerConfig            *config,
                         gboolean                  initial_index)
{
	g_return_val_if_fail (TRACKER_IS_SPARQL_CONNECTION (connection), NULL);
	g_return_val_if_fail (TRACKER_IS_CONFIG (config), NULL);

	return g_object_new (TRACKER_TYPE_MINER_FILES,
	                     "connection", connection,
	                     "indexing-tree", indexing_tree,
	                     "storage", storage,
	                     "config", config,
	                     "file-attributes", FILE_ATTRIBUTES,
	                     "initial-index", initial_index,
	                     NULL);
}

static void
remove_files_in_removable_media_cb (GObject      *object,
                                    GAsyncResult *result,
                                    gpointer      user_data)
{
	g_autofree GError *error = NULL;

	tracker_sparql_statement_update_finish (TRACKER_SPARQL_STATEMENT (object),
	                                        result, &error);

	if (error)
		g_critical ("Could not remove files in volumes: %s", error->message);
}

static void
miner_files_in_removable_media_remove_by_date (TrackerMinerFiles *miner,
                                               GDateTime         *datetime)
{
	TrackerSparqlConnection *conn;
	g_autoptr (TrackerSparqlStatement) stmt = NULL;

#ifdef G_ENABLE_DEBUG
	if (TRACKER_DEBUG_CHECK (CONFIG)) {
		g_autofree gchar *date;

		date = g_date_time_format_iso8601 (datetime);
		g_message ("  Removing all resources in store from removable "
			   "devices not mounted after '%s'",
			   date);
	}
#endif

	conn = tracker_miner_get_connection (TRACKER_MINER (miner));
	stmt = tracker_load_statement (conn, "delete-mountpoints-by-date.rq", NULL);

	tracker_sparql_statement_bind_datetime (stmt, "unmountDate", datetime);
	tracker_sparql_statement_update_async (stmt, NULL,
	                                       remove_files_in_removable_media_cb,
	                                       NULL);
}

TrackerStorage *
tracker_miner_files_get_storage (TrackerMinerFiles *mf)
{
	return mf->private->storage;
}

gboolean
tracker_miner_files_check_allowed_text_file (TrackerMinerFiles *mf,
                                             GFile             *file)
{
	g_autofree gchar *basename = NULL;
	GList *l;

	basename = g_file_get_basename (file);

	for (l = mf->private->allowed_text_patterns; l; l = l->next) {
#if GLIB_CHECK_VERSION (2, 70, 0)
		if (g_pattern_spec_match_string (l->data, basename))
#else
		if (g_pattern_match_string (l->data, basename))
#endif
			return TRUE;
	}

	return FALSE;
}
