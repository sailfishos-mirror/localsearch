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

#include <gio/gio.h>

#include "tracker-storage.h"

#include <tracker-common.h>

#define PRE_UNMOUNT_FAILED_TIMEOUT 3

struct _TrackerStorage {
	GObject parent_instance;
	GVolumeMonitor *volume_monitor;
	GList *removable_mount_points;
	GList *pending_pre_unmounts;
};

typedef struct {
	TrackerStorage *storage;
	GMount *mount;
	guint source_id;
} UnmountCheckData;

enum {
	MOUNT_POINT_ADDED,
	MOUNT_POINT_REMOVED,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = {0};

G_DEFINE_TYPE (TrackerStorage, tracker_storage, G_TYPE_OBJECT);

static gboolean
mount_is_eligible (GMount *mount)
{
	g_autoptr (GFile) root = NULL;

	/* Do not process shadowed mounts! */
	if (g_mount_is_shadowed (mount)) {
		g_autofree char *name = g_mount_get_name (mount);

		TRACKER_NOTE (MONITORS,
		              g_message ("Skipping shadowed mount '%s'",
		                         name));
		return FALSE;
	}

	root = g_mount_get_root (mount);

	if (!g_file_is_native (root)) {
		g_autofree char *uri = g_file_get_uri (root);

		TRACKER_NOTE (MONITORS,
		              g_message ("Ignoring mount '%s', URI is not native",
		                         uri));
		return FALSE;
	}

	return TRUE;
}

static void
mount_add (TrackerStorage *storage,
           GMount         *mount)
{
	g_autoptr (GDrive) drive = NULL;
	g_autoptr (GFile) root = NULL;
	gboolean removable;

	if (!mount_is_eligible (mount))
		return;

	root = g_mount_get_root (mount);
	drive = g_mount_get_drive (mount);
	removable = drive && g_drive_is_removable (drive);

	TRACKER_NOTE (MONITORS,
	              g_message ("Location '%s' mounted, removable: %s",
	                         g_file_peek_path (root),
	                         removable ? "yes" : "no"));

	if (removable) {
		storage->removable_mount_points =
			g_list_prepend (storage->removable_mount_points,
			                g_object_ref (mount));
	}

	g_signal_emit (storage,
	               signals[MOUNT_POINT_ADDED],
	               0,
	               g_file_peek_path (root),
	               removable,
	               NULL);
}

static void
mount_remove (TrackerStorage *storage,
              GMount         *mount)
{
	g_autoptr (GFile) root = NULL;
	GList *elem;

	if (!mount_is_eligible (mount))
		return;

	root = g_mount_get_root (mount);

	TRACKER_NOTE (MONITORS,
	              g_message ("Location '%s' unmounted",
	                         g_file_peek_path (root)));

	elem = g_list_find (storage->removable_mount_points,
	                    mount);

	if (elem) {
		storage->removable_mount_points =
			g_list_remove_link (storage->removable_mount_points,
			                    elem);
		g_object_unref (elem->data);
		g_list_free_1 (elem);
	}

	g_signal_emit (storage,
	               signals[MOUNT_POINT_REMOVED],
	               0,
	               g_file_peek_path (root),
	               NULL);
}

static void
unount_check_data_free (gpointer user_data)
{
	UnmountCheckData *unmount_data = user_data;

	g_source_remove (unmount_data->source_id);
	g_object_unref (unmount_data->mount);
	g_free (unmount_data);
}

static gboolean
pre_unmount_timeout_cb (gpointer user_data)
{
	UnmountCheckData *unmount_data = user_data;
	TrackerStorage *storage = unmount_data->storage;

	g_warning ("Unmount operation failed, adding back mount point...");

	mount_add (unmount_data->storage, unmount_data->mount);

	storage->pending_pre_unmounts =
		g_list_remove (storage->pending_pre_unmounts, unmount_data);
	unount_check_data_free (unmount_data);

	return G_SOURCE_REMOVE;
}

static void
mount_added_cb (GVolumeMonitor *monitor,
                GMount         *mount,
                TrackerStorage *storage)
{
	mount_add (storage, mount);
}

static void
mount_removed_cb (GVolumeMonitor *monitor,
                  GMount         *mount,
                  TrackerStorage *storage)
{
	GList *l;

	for (l = storage->pending_pre_unmounts; l; l = l->next) {
		UnmountCheckData *unmount_data = l->data;

		if (unmount_data->mount == mount) {
			/* Pre-unmount went through */
			storage->pending_pre_unmounts =
				g_list_remove (storage->pending_pre_unmounts, unmount_data);
			unount_check_data_free (unmount_data);
			return;
		}
	}

	/* Mount removed without pre-unmount */
	mount_remove (storage, mount);
}

static void
mount_pre_unmount_cb (GVolumeMonitor *monitor,
                      GMount         *mount,
                      TrackerStorage *storage)
{
	UnmountCheckData *unmount_data;

	unmount_data = g_new0 (UnmountCheckData, 1);
	unmount_data->storage = storage;
	unmount_data->mount = g_object_ref (mount);
	unmount_data->source_id =
		g_timeout_add_seconds_full (G_PRIORITY_DEFAULT_IDLE + 10,
		                            PRE_UNMOUNT_FAILED_TIMEOUT,
		                            pre_unmount_timeout_cb,
		                            unmount_data, NULL);

	storage->pending_pre_unmounts =
		g_list_prepend (storage->pending_pre_unmounts, unmount_data);

	/* Pre-emptively remove the mount, will be added back on unmount failure */
	mount_remove (storage, mount);
}

static void
tracker_storage_finalize (GObject *object)
{
	TrackerStorage *storage = TRACKER_STORAGE (object);

	g_signal_handlers_disconnect_by_data (storage->volume_monitor, object);
	g_clear_object (&storage->volume_monitor);

	g_clear_list (&storage->removable_mount_points, g_object_unref);
	g_clear_list (&storage->pending_pre_unmounts, unount_check_data_free);

	G_OBJECT_CLASS (tracker_storage_parent_class)->finalize (object);
}

static void
tracker_storage_class_init (TrackerStorageClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = tracker_storage_finalize;

	signals[MOUNT_POINT_ADDED] =
		g_signal_new ("mount-point-added",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              0,
		              NULL, NULL,
		              NULL,
		              G_TYPE_NONE,
		              2,
		              G_TYPE_STRING,
		              G_TYPE_BOOLEAN);

	signals[MOUNT_POINT_REMOVED] =
		g_signal_new ("mount-point-removed",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              0,
		              NULL, NULL,
		              NULL,
		              G_TYPE_NONE,
		              1, G_TYPE_STRING);
}

static void
tracker_storage_init (TrackerStorage *storage)
{
	GList *mounts, *l;

	storage->volume_monitor = g_volume_monitor_get ();

	g_signal_connect (storage->volume_monitor, "mount-added",
	                  G_CALLBACK (mount_added_cb), storage);
	g_signal_connect (storage->volume_monitor, "mount-removed",
	                  G_CALLBACK (mount_removed_cb), storage);
	g_signal_connect (storage->volume_monitor, "mount-pre-unmount",
	                  G_CALLBACK (mount_pre_unmount_cb), storage);

	mounts = g_volume_monitor_get_mounts (storage->volume_monitor);

	for (l = mounts; l; l = l->next)
		mount_add (storage, l->data);

	g_list_free_full (mounts, g_object_unref);
}

TrackerStorage *
tracker_storage_new (void)
{
	return g_object_new (TRACKER_TYPE_STORAGE, NULL);
}

GSList *
tracker_storage_get_removable_mount_points (TrackerStorage *storage)
{
	GSList *retval = NULL;
	GList *l;

	for (l = storage->removable_mount_points; l; l = l->next) {
		g_autoptr (GFile) root = g_mount_get_root (l->data);

		retval = g_slist_prepend (retval, g_file_get_path (root));
	}

	return retval;
}

gboolean
tracker_storage_is_removable_mount_point (TrackerStorage *storage,
                                          GFile          *file)
{
	GList *l;

	g_return_val_if_fail (TRACKER_IS_STORAGE (storage), FALSE);

	for (l = storage->removable_mount_points; l; l = l->next) {
		g_autoptr (GFile) root = g_mount_get_root (l->data);

		if (g_file_equal (root, file))
			return TRUE;
	}

	return FALSE;
}
