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

#ifndef __LIBTRACKER_MINER_STORAGE_H__
#define __LIBTRACKER_MINER_STORAGE_H__

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define TRACKER_TYPE_STORAGE (tracker_storage_get_type ())
G_DECLARE_FINAL_TYPE (TrackerStorage,
                      tracker_storage,
                      TRACKER, STORAGE,
                      GObject)

TrackerStorage * tracker_storage_new (void);

GSList * tracker_storage_get_removable_mount_points (TrackerStorage *storage);

gboolean tracker_storage_is_removable_mount_point (TrackerStorage *storage,
                                                   GFile          *file);

G_END_DECLS

#endif /* __LIBTRACKER_MINER_STORAGE_H__ */
