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

#ifndef __LIBTRACKER_MINER_MINER_FS_H__
#define __LIBTRACKER_MINER_MINER_FS_H__

#include <glib-object.h>
#include <gio/gio.h>
#include <tinysparql.h>

#include <tracker-common.h>

#include "tracker-indexing-tree.h"
#include "tracker-sparql-buffer.h"

G_BEGIN_DECLS

#define TRACKER_TYPE_MINER_FS tracker_miner_fs_get_type()
G_DECLARE_FINAL_TYPE (TrackerMinerFS, tracker_miner_fs, TRACKER, MINER_FS, TrackerMiner)

/* Properties */
TrackerIndexingTree * tracker_miner_fs_get_indexing_tree     (TrackerMinerFS  *fs);

/* URNs */
const gchar * tracker_miner_fs_get_identifier (TrackerMinerFS *miner,
                                               GFile          *file);

char * tracker_miner_fs_get_file_resource_uri (TrackerMinerFS *fs,
                                               GFile          *file);

G_END_DECLS

#endif /* __LIBTRACKER_MINER_MINER_FS_H__ */
