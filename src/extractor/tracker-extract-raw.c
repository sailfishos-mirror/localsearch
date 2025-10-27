/*
 * Copyright (C) 2017-2018 Red Hat, Inc.
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

#include <math.h>
#include <string.h>

#include <gexiv2/gexiv2.h>

#include <tracker-common.h>

#include "utils/tracker-extract.h"
#include "utils/tracker-gexiv-compat.h"

#include "tracker-main.h"

#include "tracker-exif.h"
#include "tracker-iptc.h"

G_MODULE_EXPORT gboolean
tracker_extract_get_metadata (TrackerExtractInfo  *info,
                              GError             **error)
{
	GError *inner_error = NULL;
	GFile *file;
	GExiv2Metadata *metadata = NULL;
	TrackerIptcData *iptc_data;
	TrackerExifData *exif_data;
	TrackerResource *resource = NULL;
	gboolean retval = FALSE;
	gchar *filename = NULL;
	gchar *uri = NULL, *resource_uri;
	gint height;
	gint width;

	metadata = gexiv2_metadata_new ();
	file = tracker_extract_info_get_file (info);
	filename = g_file_get_path (file);

	if (!gexiv2_metadata_open_path (metadata, filename, &inner_error)) {
		g_propagate_prefixed_error (error, inner_error, "Could not open: ");
		goto out;
	}

	resource_uri = tracker_extract_info_get_content_id (info, NULL);
	resource = tracker_resource_new (resource_uri);
	tracker_resource_add_uri (resource, "rdf:type", "nfo:Image");
	tracker_resource_add_uri (resource, "rdf:type", "nmm:Photo");
	g_free (resource_uri);

	width = gexiv2_metadata_get_pixel_width (metadata);
	tracker_resource_set_int (resource, "nfo:width", width);
	height = gexiv2_metadata_get_pixel_height (metadata);
	tracker_resource_set_int (resource, "nfo:height", height);

	uri = g_file_get_uri (file);
	tracker_guarantee_resource_title_from_file (resource, "nie:title", NULL, uri, NULL);
	tracker_guarantee_resource_date_from_file_mtime (resource, "nie:contentCreated", NULL, uri);

	exif_data = tracker_exif_new_from_metadata (metadata);
	if (exif_data) {
		tracker_exif_apply_to_resource (resource, exif_data);
		tracker_exif_free (exif_data);
	}

	iptc_data = tracker_iptc_new_from_metadata (metadata);
	if (iptc_data) {
		tracker_iptc_apply_to_resource (resource, iptc_data);
		tracker_iptc_free (iptc_data);
	}

	tracker_extract_info_set_resource (info, resource);
	retval = TRUE;

out:
	g_clear_object (&metadata);
	g_clear_object (&resource);
	g_free (filename);
	g_free (uri);
	return retval;
}
