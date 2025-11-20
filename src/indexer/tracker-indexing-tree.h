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

#pragma once

#include <gio/gio.h>
#include "tracker-miner-enums.h"

G_BEGIN_DECLS

#define TRACKER_TYPE_INDEXING_TREE (tracker_indexing_tree_get_type ())
G_DECLARE_FINAL_TYPE (TrackerIndexingTree,
		      tracker_indexing_tree,
		      TRACKER, INDEXING_TREE,
		      GObject)

TrackerIndexingTree * tracker_indexing_tree_new      (void);

void      tracker_indexing_tree_add                  (TrackerIndexingTree   *tree,
                                                      GFile                 *directory,
                                                      TrackerDirectoryFlags  flags);
void      tracker_indexing_tree_remove               (TrackerIndexingTree   *tree,
                                                      GFile                 *directory);
gboolean  tracker_indexing_tree_notify_update        (TrackerIndexingTree   *tree,
                                                      GFile                 *file,
                                                      gboolean               recursive);

void      tracker_indexing_tree_add_filter           (TrackerIndexingTree  *tree,
                                                      TrackerFilterType     filter,
                                                      const gchar          *glob_string);
void      tracker_indexing_tree_clear_filters        (TrackerIndexingTree  *tree,
                                                      TrackerFilterType     type);
gboolean  tracker_indexing_tree_file_matches_filter  (TrackerIndexingTree  *tree,
                                                      TrackerFilterType     type,
                                                      GFile                *file);

gboolean  tracker_indexing_tree_file_is_indexable    (TrackerIndexingTree  *tree,
                                                      GFile                *file,
                                                      GFileInfo            *info);
gboolean  tracker_indexing_tree_parent_is_indexable (TrackerIndexingTree  *tree,
                                                     GFile                *file);

gboolean  tracker_indexing_tree_get_filter_hidden    (TrackerIndexingTree  *tree);
void      tracker_indexing_tree_set_filter_hidden    (TrackerIndexingTree  *tree,
                                                      gboolean              filter_hidden);

GFile *   tracker_indexing_tree_get_root             (TrackerIndexingTree    *tree,
                                                      GFile                  *file,
                                                      const char            **id,
                                                      TrackerDirectoryFlags  *directory_flags);

gboolean  tracker_indexing_tree_file_is_root         (TrackerIndexingTree   *tree,
                                                      GFile                 *file);

GList *   tracker_indexing_tree_list_roots           (TrackerIndexingTree   *tree);

void tracker_indexing_tree_clear_allowed_text_patterns (TrackerIndexingTree *tree);

void tracker_indexing_tree_add_allowed_text_pattern (TrackerIndexingTree *tree,
                                                     const char          *pattern_str);

gboolean tracker_indexing_tree_file_has_allowed_text_extension (TrackerIndexingTree *tree,
                                                                GFile               *file);

void tracker_indexing_tree_update_all (TrackerIndexingTree *tree);

gboolean tracker_indexing_tree_save_config (TrackerIndexingTree  *tree,
                                            GFile                *config,
                                            GError              **error);

gboolean tracker_indexing_tree_check_config (TrackerIndexingTree *tree,
                                             GFile               *config,
                                             gboolean             check_locations);

G_END_DECLS
