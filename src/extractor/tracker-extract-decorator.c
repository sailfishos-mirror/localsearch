/*
 * Copyright (C) 2014 Carlos Garnacho <carlosg@gnome.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config-miners.h"

#include "utils/tracker-extract.h"

#include "tracker-extract-decorator.h"
#include "tracker-extract-persistence.h"

#include <tinysparql.h>

#define THROTTLED_TIMEOUT_MS 10

enum {
	PROP_0,
	PROP_EXTRACTOR,
	PROP_PERSISTENCE,
	N_PROPS,
};

static GParamSpec *props[N_PROPS] = { 0, };

struct _TrackerExtractDecorator {
	TrackerDecorator parent_instance;

	TrackerExtract *extractor;
	GTimer *timer;
	gboolean extracting;

	TrackerSparqlStatement *update_hash;
	TrackerSparqlStatement *delete_file;

	TrackerExtractPersistence *persistence;
	GVolumeMonitor *volume_monitor;

	guint throttle_id;
	guint throttled : 1;
};

typedef struct _ExtractData ExtractData;

struct _ExtractData {
	TrackerDecorator *decorator;
	TrackerDecoratorInfo *decorator_info;
	GFile *file;
};

static void decorator_get_next_file (TrackerDecorator *decorator);

G_DEFINE_TYPE (TrackerExtractDecorator, tracker_extract_decorator,
               TRACKER_TYPE_DECORATOR)

static void
tracker_extract_decorator_set_property (GObject      *object,
                                        guint         param_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
	TrackerExtractDecorator *extract_decorator =
		TRACKER_EXTRACT_DECORATOR (object);

	switch (param_id) {
	case PROP_EXTRACTOR:
		extract_decorator->extractor = g_value_dup_object (value);
		break;
	case PROP_PERSISTENCE:
		extract_decorator->persistence = g_value_dup_object (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
		break;
	}
}

static TrackerSparqlStatement *
load_statement (TrackerExtractDecorator *decorator,
                const gchar             *query_filename)
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
tracker_extract_decorator_constructed (GObject *object)
{
	TrackerExtractDecorator *decorator = TRACKER_EXTRACT_DECORATOR (object);

	G_OBJECT_CLASS (tracker_extract_decorator_parent_class)->constructed (object);

	decorator->update_hash = load_statement (decorator, "update-hash.rq");
	decorator->delete_file = load_statement (decorator, "delete-file.rq");
}

static void
tracker_extract_decorator_finalize (GObject *object)
{
	TrackerExtractDecorator *extract_decorator =
		TRACKER_EXTRACT_DECORATOR (object);

	g_clear_handle_id (&extract_decorator->throttle_id, g_source_remove);
	g_clear_object (&extract_decorator->extractor);
	g_clear_object (&extract_decorator->volume_monitor);
	g_clear_object (&extract_decorator->update_hash);
	g_clear_object (&extract_decorator->delete_file);
	g_clear_object (&extract_decorator->persistence);
	g_clear_pointer (&extract_decorator->timer, g_timer_destroy);

	G_OBJECT_CLASS (tracker_extract_decorator_parent_class)->finalize (object);
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

static void
tracker_extract_decorator_update (TrackerDecorator   *decorator,
                                  TrackerExtractInfo *info,
                                  TrackerBatch       *batch)
{
	TrackerExtractDecorator *extract_decorator =
		TRACKER_EXTRACT_DECORATOR (decorator);
	TrackerResource *resource;
	const gchar *graph, *mime_type, *hash;
	g_autofree gchar *uri = NULL;
	GFile *file;

	mime_type = tracker_extract_info_get_mimetype (info);
	hash = tracker_extract_module_manager_get_hash (mime_type);
	graph = tracker_extract_info_get_graph (info);
	resource = tracker_extract_info_get_resource (info);
	file = tracker_extract_info_get_file (info);
	uri = g_file_get_uri (file);

	tracker_batch_add_statement (batch,
	                             extract_decorator->update_hash,
	                             "file", G_TYPE_STRING, uri,
	                             "hash", G_TYPE_STRING, hash,
	                             NULL);

	if (resource)
		tracker_batch_add_resource (batch, graph, resource);
}

static gboolean
throttle_next_item_cb (gpointer user_data)
{
	TrackerExtractDecorator *extract_decorator = user_data;

	extract_decorator->throttle_id = 0;
	decorator_get_next_file (user_data);

	return G_SOURCE_REMOVE;
}

static void
throttle_next_item (TrackerDecorator *decorator)
{
	TrackerExtractDecorator *extract_decorator =
		TRACKER_EXTRACT_DECORATOR (decorator);

	if (extract_decorator->throttled) {
		extract_decorator->throttle_id =
			g_timeout_add (THROTTLED_TIMEOUT_MS,
				       throttle_next_item_cb,
				       decorator);
	} else {
		extract_decorator->throttle_id =
			g_idle_add (throttle_next_item_cb,
				    decorator);
	}
}

static void
get_metadata_cb (TrackerExtract *extract,
                 GAsyncResult   *result,
                 ExtractData    *data)
{
	TrackerExtractDecorator *extract_decorator =
		TRACKER_EXTRACT_DECORATOR (data->decorator);
	TrackerExtractInfo *info;
	GError *error = NULL;

	info = tracker_extract_file_finish (extract, result, &error);

	tracker_extract_persistence_set_file (extract_decorator->persistence, NULL);

	if (error) {
		tracker_decorator_info_complete_error (data->decorator_info, error);
	} else {
		ensure_data (info);
		tracker_decorator_info_complete (data->decorator_info, info);
		tracker_extract_info_unref (info);
	}

	extract_decorator->extracting = FALSE;

	throttle_next_item (data->decorator);

	tracker_decorator_info_unref (data->decorator_info);
	g_object_unref (data->file);
	g_free (data);
}

static void
decorator_get_next_file (TrackerDecorator *decorator)
{
	TrackerExtractDecorator *extract_decorator =
		TRACKER_EXTRACT_DECORATOR (decorator);
	TrackerDecoratorInfo *info;
	g_autoptr (GError) error = NULL;
	ExtractData *data;
	GFile *file;

	if (!tracker_miner_is_started (TRACKER_MINER (decorator)) ||
	    tracker_miner_is_paused (TRACKER_MINER (decorator)))
		return;

	if (extract_decorator->extracting)
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

	file = g_file_new_for_uri (tracker_decorator_info_get_url (info));

	if (!g_file_is_native (file)) {
		g_warning ("URI '%s' is not native",
		           tracker_decorator_info_get_url (info));
		tracker_decorator_info_unref (info);
		decorator_get_next_file (decorator);
		return;
	}

	extract_decorator->extracting = TRUE;

	data = g_new0 (ExtractData, 1);
	data->decorator = decorator;
	data->decorator_info = info;
	data->file = file;

	TRACKER_NOTE (DECORATOR,
	              g_message ("[Decorator] Extracting metadata for '%s'",
	                         tracker_decorator_info_get_url (info)));

	tracker_extract_persistence_set_file (extract_decorator->persistence, data->file);

	tracker_extract_file (extract_decorator->extractor,
	                      tracker_decorator_info_get_url (info),
	                      tracker_decorator_info_get_content_id (info),
	                      tracker_decorator_info_get_mime_type (info),
	                      tracker_decorator_info_get_cancellable (info),
	                      (GAsyncReadyCallback) get_metadata_cb, data);
}

static void
tracker_extract_decorator_paused (TrackerMiner *miner)
{
	TrackerExtractDecorator *extract_decorator =
		TRACKER_EXTRACT_DECORATOR (miner);

	g_debug ("Decorator paused");

	g_clear_handle_id (&extract_decorator->throttle_id, g_source_remove);

	if (extract_decorator->timer)
		g_timer_stop (extract_decorator->timer);
}

static void
tracker_extract_decorator_resumed (TrackerMiner *miner)
{
	TrackerExtractDecorator *extract_decorator =
		TRACKER_EXTRACT_DECORATOR (miner);

	g_debug ("Decorator resumed, processing remaining %d items",
		 tracker_decorator_get_n_items (TRACKER_DECORATOR (miner)));

	if (extract_decorator->timer)
		g_timer_continue (extract_decorator->timer);

	decorator_get_next_file (TRACKER_DECORATOR (miner));
}

static void
tracker_extract_decorator_started (TrackerMiner *miner)
{
	TrackerExtractDecorator *decorator = TRACKER_EXTRACT_DECORATOR (miner);
	GFile *file;

	file = tracker_extract_persistence_get_file (decorator->persistence);

	if (file) {
		tracker_decorator_raise_error (TRACKER_DECORATOR (miner), file,
		                               "Crash/hang handling file", NULL);
	}

	TRACKER_MINER_CLASS (tracker_extract_decorator_parent_class)->started (miner);
}

static void
tracker_extract_decorator_items_available (TrackerDecorator *decorator)
{
	TrackerExtractDecorator *extract_decorator =
		TRACKER_EXTRACT_DECORATOR (decorator);

	g_debug ("Starting to process %d items",
	         tracker_decorator_get_n_items (decorator));

	g_clear_pointer (&extract_decorator->timer, g_timer_destroy);
	extract_decorator->timer = g_timer_new ();
	if (tracker_miner_is_paused (TRACKER_MINER (decorator)))
		g_timer_stop (extract_decorator->timer);

	decorator_get_next_file (decorator);
}

static void
tracker_extract_decorator_finished (TrackerDecorator *decorator)
{
	TrackerExtractDecorator *extract_decorator =
		TRACKER_EXTRACT_DECORATOR (decorator);
	g_autofree gchar *time_str = NULL;
	gdouble elapsed = 0;

	if (extract_decorator->timer) {
		elapsed = g_timer_elapsed (extract_decorator->timer, NULL);
		g_clear_pointer (&extract_decorator->timer, g_timer_destroy);
	}

	time_str = tracker_seconds_to_string (elapsed, TRUE);
	g_debug ("Extraction finished in %s", time_str);
}

static void
tracker_extract_decorator_error (TrackerDecorator   *decorator,
                                 GFile              *file,
                                 const gchar        *error_message,
                                 const gchar        *extra_info)
{
	TrackerExtractDecorator *extract_decorator =
		TRACKER_EXTRACT_DECORATOR (decorator);
	g_autoptr (GError) error = NULL, info_error = NULL;
	g_autofree gchar *uri = NULL;
	g_autoptr (GFileInfo) info = NULL;
	const gchar *hash = NULL;
	gboolean removed_hash = FALSE;

	uri = g_file_get_uri (file);
	g_debug ("Extraction on file '%s' failed in previous execution, ignoring", uri);

	info = g_file_query_info (file,
	                          G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
	                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
	                          NULL, &info_error);

	if (info) {
		const gchar *mimetype;

		mimetype = g_file_info_get_attribute_string (info,
		                                             G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE);
		hash = tracker_extract_module_manager_get_hash (mimetype);
	}

	if (hash) {
		g_signal_emit_by_name (decorator, "raise-error", file, error_message, extra_info);

		tracker_sparql_statement_bind_string (extract_decorator->update_hash,
		                                      "file", uri);
		tracker_sparql_statement_bind_string (extract_decorator->update_hash,
		                                      "hash", hash);

		removed_hash = tracker_sparql_statement_update (extract_decorator->update_hash,
		                                                NULL, &error);
	}

	if (!removed_hash) {
		if (info_error && !g_error_matches (info_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
			g_signal_emit_by_name (decorator, "raise-error", file, error_message, extra_info);

		g_clear_error (&error);
		tracker_sparql_statement_bind_string (extract_decorator->delete_file,
		                                      "file", uri);
		tracker_sparql_statement_update (extract_decorator->delete_file,
		                                 NULL, &error);
	}

	if (error) {
		g_warning ("Failed to update ignored file '%s': %s",
		           uri, error->message);
	}
}

static void
tracker_extract_decorator_class_init (TrackerExtractDecoratorClass *klass)
{
	TrackerDecoratorClass *decorator_class = TRACKER_DECORATOR_CLASS (klass);
	TrackerMinerClass *miner_class = TRACKER_MINER_CLASS (klass);
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->constructed = tracker_extract_decorator_constructed;
	object_class->finalize = tracker_extract_decorator_finalize;
	object_class->set_property = tracker_extract_decorator_set_property;

	miner_class->paused = tracker_extract_decorator_paused;
	miner_class->resumed = tracker_extract_decorator_resumed;
	miner_class->started = tracker_extract_decorator_started;

	decorator_class->items_available = tracker_extract_decorator_items_available;
	decorator_class->finished = tracker_extract_decorator_finished;
	decorator_class->error = tracker_extract_decorator_error;
	decorator_class->update = tracker_extract_decorator_update;

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
}

static void
mount_points_changed_cb (GVolumeMonitor *monitor,
                         GMount         *mount,
                         gpointer        user_data)
{
	GDrive *drive = g_mount_get_drive (mount);

	if (drive) {
		if (g_drive_is_media_removable (drive))
			tracker_decorator_invalidate_cache (user_data);
		g_object_unref (drive);
	}
}

static void
tracker_extract_decorator_init (TrackerExtractDecorator *decorator)
{
	decorator->volume_monitor = g_volume_monitor_get ();
	g_signal_connect_object (decorator->volume_monitor, "mount-added",
	                         G_CALLBACK (mount_points_changed_cb), decorator, 0);
	g_signal_connect_object (decorator->volume_monitor, "mount-pre-unmount",
	                         G_CALLBACK (mount_points_changed_cb), decorator, 0);
	g_signal_connect_object (decorator->volume_monitor, "mount-removed",
	                         G_CALLBACK (mount_points_changed_cb), decorator, 0);
}

TrackerDecorator *
tracker_extract_decorator_new (TrackerSparqlConnection   *connection,
                               TrackerExtract            *extract,
                               TrackerExtractPersistence *persistence)
{
	return g_object_new (TRACKER_TYPE_EXTRACT_DECORATOR,
			     "connection", connection,
			     "extractor", extract,
	                     "persistence", persistence,
			     NULL);
}

void
tracker_extract_decorator_set_throttled (TrackerExtractDecorator *decorator,
                                         gboolean                 throttled)
{
	decorator->throttled = !!throttled;
}
