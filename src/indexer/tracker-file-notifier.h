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
 * Author: Carlos Garnacho  <carlos@lanedo.com>
 */

#ifndef __TRACKER_FILE_NOTIFIER_H__
#define __TRACKER_FILE_NOTIFIER_H__

#include <gio/gio.h>
#include "tracker-indexing-tree.h"
#include "tracker-miner-fs.h"

G_BEGIN_DECLS

#define TRACKER_TYPE_FILE_NOTIFIER         (tracker_file_notifier_get_type ())
#define TRACKER_FILE_NOTIFIER(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), TRACKER_TYPE_FILE_NOTIFIER, TrackerFileNotifier))
#define TRACKER_FILE_NOTIFIER_CLASS(c)     (G_TYPE_CHECK_CLASS_CAST ((c),    TRACKER_TYPE_FILE_NOTIFIER, TrackerFileNotifierClass))
#define TRACKER_IS_FILE_NOTIFIER(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), TRACKER_TYPE_FILE_NOTIFIER))
#define TRACKER_IS_FILE_NOTIFIER_CLASS(c)  (G_TYPE_CHECK_CLASS_TYPE ((c),    TRACKER_TYPE_FILE_NOTIFIER))
#define TRACKER_FILE_NOTIFIER_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o),  TRACKER_TYPE_FILE_NOTIFIER, TrackerFileNotifierClass))

typedef struct _TrackerFileNotifier TrackerFileNotifier;
typedef struct _TrackerFileNotifierClass TrackerFileNotifierClass;
typedef enum _TrackerFileNotifierType TrackerFileNotifierType;

struct _TrackerFileNotifier {
	GObject parent_instance;
};

struct _TrackerFileNotifierClass {
	GObjectClass parent_class;
};

typedef enum
{
	TRACKER_FILE_NOTIFIER_STATUS_INDEXING,
	TRACKER_FILE_NOTIFIER_STATUS_CHECKING,
} TrackerFileNotifierStatus;

GType         tracker_file_notifier_get_type     (void) G_GNUC_CONST;

TrackerFileNotifier *
              tracker_file_notifier_new          (TrackerIndexingTree     *indexing_tree,
                                                  TrackerSparqlConnection *connection,
                                                  const gchar             *file_attributes);

gboolean      tracker_file_notifier_start        (TrackerFileNotifier     *notifier);
void          tracker_file_notifier_stop         (TrackerFileNotifier     *notifier);
gboolean      tracker_file_notifier_is_active    (TrackerFileNotifier     *notifier);

void          tracker_file_notifier_set_high_water (TrackerFileNotifier *notifier,
                                                    gboolean             high_water);

gboolean tracker_file_notifier_get_status (TrackerFileNotifier        *notifier,
                                           TrackerFileNotifierStatus  *status,
                                           GFile                     **current_root,
                                           guint                      *files_found,
                                           guint                      *files_updated,
                                           guint                      *files_ignored,
                                           guint                      *files_reindexed);

G_END_DECLS

#endif /* __TRACKER_FILE_NOTIFIER_H__ */
