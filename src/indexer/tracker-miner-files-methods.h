/*
 * Copyright (C) 2008, Nokia <ivan.frade@nokia.com>
 * Copyright (C) 2021, Red Hat Inc.
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

#pragma once

#include "tracker-indexer.h"

void tracker_miner_files_process_file (TrackerIndexer      *indexer,
                                       GFile               *file,
                                       GFileInfo           *file_info,
                                       TrackerSparqlBuffer *buffer,
                                       gboolean             create);

void tracker_miner_files_process_file_attributes (TrackerIndexer      *indexer,
                                                  GFile               *file,
                                                  GFileInfo           *info,
                                                  TrackerSparqlBuffer *buffer);

void tracker_miner_files_finish_directory (TrackerIndexer      *indexer,
                                           GFile               *file,
                                           TrackerSparqlBuffer *buffer);

gchar * tracker_miner_files_get_content_identifier (TrackerIndexer *indexer,
                                                    GFile          *file,
                                                    GFileInfo      *info);
