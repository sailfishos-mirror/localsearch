/*
 * Copyright (C) 2009, Nokia <ivan.frade@nokia.com>
 * Copyright (C) 2014, Lanedo <martyn@lanedo.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.          See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config-miners.h"

#include <string.h>
#include <stdlib.h>

#include <glib.h>
#include <gio/gio.h>

#define G_SETTINGS_ENABLE_BACKEND
#include <gio/gsettingsbackend.h>

#include <libtracker-miners-common/tracker-common.h>

#include "tracker-config.h"

#define CONFIG_SCHEMA "org.freedesktop.Tracker3.Miner.Files"
#define CONFIG_PATH   "/org/freedesktop/tracker/miner/files/"

typedef struct {
	/* IMPORTANT: There are 3 versions of the directories:
	 * 1. a GStrv stored in GSettings
	 * 2. a GSList stored here which is the GStrv without any
	 *    aliases or duplicates resolved.
	 * 3. a GSList stored here which has duplicates and aliases
	 *    resolved.
	 */
	GSList *index_recursive_directories;
	GSList *index_recursive_directories_unfiltered;
	GSList *index_single_directories;
	GSList *index_single_directories_unfiltered;
} TrackerConfigPrivate;

static void config_finalize (GObject *object);
static void config_constructed (GObject *object);

static void rebuild_filtered_lists (TrackerConfig *config);

G_DEFINE_TYPE_WITH_PRIVATE (TrackerConfig, tracker_config, G_TYPE_SETTINGS)

static void
tracker_config_class_init (TrackerConfigClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = config_finalize;
	object_class->constructed = config_constructed;
}

static void
tracker_config_init (TrackerConfig *object)
{
}

static void
config_finalize (GObject *object)
{
	TrackerConfigPrivate *priv;

	priv = tracker_config_get_instance_private (TRACKER_CONFIG (object));

	g_slist_free_full (priv->index_single_directories, g_free);
	g_slist_free_full (priv->index_single_directories_unfiltered, g_free);
	g_slist_free_full (priv->index_recursive_directories, g_free);
	g_slist_free_full (priv->index_recursive_directories_unfiltered, g_free);

	(G_OBJECT_CLASS (tracker_config_parent_class)->finalize) (object);
}

static GSList *
dir_mapping_get (GSList   *dirs,
                 gboolean  is_recursive)
{
	GSList *filtered = NULL;
	GSList *evaluated_dirs, *l;

	if (dirs) {
		filtered = tracker_path_list_filter_duplicates (dirs, ".", is_recursive);
	}

	evaluated_dirs = NULL;

	for (l = filtered; l; l = l->next) {
		gchar *path_to_use;

		path_to_use = tracker_path_evaluate_name (l->data);

		if (path_to_use) {
			evaluated_dirs = g_slist_prepend (evaluated_dirs, path_to_use);
		}
	}

	g_slist_foreach (filtered, (GFunc) g_free, NULL);
	g_slist_free (filtered);

	return g_slist_reverse (evaluated_dirs);
}

static void
update_directories (TrackerConfig *config)
{
	TrackerConfigPrivate *priv = tracker_config_get_instance_private (config);
	GStrv strv;

	strv = g_settings_get_strv (G_SETTINGS (config), "index-recursive-directories");
	priv->index_recursive_directories_unfiltered = tracker_string_list_to_gslist (strv, -1);
	g_strfreev (strv);

	strv = g_settings_get_strv (G_SETTINGS (config), "index-single-directories");
	priv->index_single_directories_unfiltered = tracker_string_list_to_gslist (strv, -1);
	g_strfreev (strv);

	rebuild_filtered_lists (config);
}

static void
config_constructed (GObject *object)
{
	TrackerConfig *config = TRACKER_CONFIG (object);

	(G_OBJECT_CLASS (tracker_config_parent_class)->constructed) (object);

	g_signal_connect (config, "notify::index-recursive-directories",
	                  G_CALLBACK (update_directories), NULL);
	g_signal_connect (config, "notify::index-single-directories",
	                  G_CALLBACK (update_directories), NULL);

	update_directories (config);
}

TrackerConfig *
tracker_config_new (void)
{
	TrackerConfig *config = NULL;

	/* FIXME: should we unset GSETTINGS_BACKEND env var? */

	if (G_UNLIKELY (g_getenv ("TRACKER_USE_CONFIG_FILES"))) {
		GSettingsBackend *backend;
		gchar *filename, *basename;
		gboolean need_to_save;

		basename = g_strdup_printf ("%s.cfg", g_get_prgname ());
		filename = g_build_filename (g_get_user_config_dir (), "tracker", basename, NULL);
		g_free (basename);

		need_to_save = g_file_test (filename, G_FILE_TEST_EXISTS) == FALSE;

		backend = g_keyfile_settings_backend_new (filename, CONFIG_PATH, "General");
		g_info ("Using config file '%s'", filename);
		g_free (filename);

		config = g_object_new (TRACKER_TYPE_CONFIG,
		                       "backend", backend,
		                       "schema-id", CONFIG_SCHEMA,
		                       "path", CONFIG_PATH,
		                       NULL);
		g_object_unref (backend);

		if (need_to_save) {
			g_info ("  Config file does not exist, using default values...");
		}
	} else {
		config = g_object_new (TRACKER_TYPE_CONFIG,
		                       "schema-id", CONFIG_SCHEMA,
		                       "path", CONFIG_PATH,
		                       NULL);
	}

	return config;
}

GSList *
tracker_config_get_index_recursive_directories (TrackerConfig *config)
{
	TrackerConfigPrivate *priv;

	g_return_val_if_fail (TRACKER_IS_CONFIG (config), NULL);

	priv = tracker_config_get_instance_private (config);

	return priv->index_recursive_directories;
}

GSList *
tracker_config_get_index_single_directories (TrackerConfig *config)
{
	TrackerConfigPrivate *priv;

	g_return_val_if_fail (TRACKER_IS_CONFIG (config), NULL);

	priv = tracker_config_get_instance_private (config);

	return priv->index_single_directories;
}

static void
rebuild_filtered_lists (TrackerConfig *config)
{
	TrackerConfigPrivate *priv;
	GSList *old_list;

	/* This function does 3 things:
	 * 1. Convert aliases like &DESKTOP to real paths
	 * 2. Filter and remove duplicates
	 * 3. Save the new list to the lists we return with public API
	 *
	 * Importantly, we:
	 * 1. Only notify on changes.
	 * 2. Don't update the unfiltered lists (since they have aliases)
	 */
	priv = tracker_config_get_instance_private (config);

	/* Filter single directories first, checking duplicates */
	old_list = priv->index_single_directories;
	priv->index_single_directories = NULL;

	if (priv->index_single_directories_unfiltered) {
		GSList *mapped_dirs = dir_mapping_get (priv->index_single_directories_unfiltered, FALSE);

		priv->index_single_directories =
			tracker_path_list_filter_duplicates (mapped_dirs, ".", FALSE);
		g_slist_free_full (mapped_dirs, g_free);
	}

	g_slist_free_full (old_list, g_free);

	/* Filter recursive directories */
	old_list = priv->index_recursive_directories;
	priv->index_recursive_directories = NULL;

	if (priv->index_recursive_directories_unfiltered) {
		GSList *l, *checked_dirs = NULL;
		GSList *mapped_dirs;

		/* First, translate aliases */
		mapped_dirs = dir_mapping_get (priv->index_recursive_directories_unfiltered, TRUE);

		/* Second, remove elements already in single directories */
		for (l = mapped_dirs; l; l = l->next) {
			if (g_slist_find_custom (priv->index_single_directories,
			                         l->data,
			                         (GCompareFunc) g_strcmp0) != NULL) {
				g_message ("Path '%s' being removed from recursive directories "
				           "list, as it also exists in single directories list",
				           (gchar *) l->data);
			} else {
				checked_dirs = g_slist_prepend (checked_dirs, l->data);
			}
		}

		g_slist_free (mapped_dirs);
		checked_dirs = g_slist_reverse (checked_dirs);

		/* Third, clean up any duplicates */
		priv->index_recursive_directories =
			tracker_path_list_filter_duplicates (checked_dirs, ".", TRUE);

		g_slist_free_full (checked_dirs, g_free);
	}

	g_slist_free_full (old_list, g_free);
}
