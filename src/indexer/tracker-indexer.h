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

#pragma once

#include <glib-object.h>
#include <gio/gio.h>
#include <tinysparql.h>

#include <tracker-common.h>

#include "tracker-indexing-tree.h"
#include "tracker-sparql-buffer.h"
#include "tracker-error-report.h"
#include "tracker-monitor.h"

G_BEGIN_DECLS

#define TRACKER_TYPE_INDEXER tracker_indexer_get_type()
G_DECLARE_FINAL_TYPE (TrackerIndexer, tracker_indexer, TRACKER, INDEXER, TrackerMiner)

TrackerIndexer * tracker_indexer_new (TrackerSparqlConnection *connection,
                                      TrackerIndexingTree     *indexing_tree,
                                      TrackerMonitor          *monitor,
                                      TrackerErrorReport      *error_reports,
                                      GFile                   *root,
                                      gboolean                 extract_content);

/* Properties */
TrackerIndexingTree * tracker_indexer_get_indexing_tree (TrackerIndexer *indexer);

/* URNs */
const gchar * tracker_indexer_get_content_uri (TrackerIndexer *indexer,
                                               GFile          *file);

char * tracker_indexer_get_file_resource_uri (TrackerIndexer *indexer,
                                              GFile          *file);

G_END_DECLS
