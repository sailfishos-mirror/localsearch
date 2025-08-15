/*
 * Copyright (C) 2025, Red Hat Inc.
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
 * Author: Nieves Montero <nmontero@redhat.com>
 */

#include "config-miners.h"

#include <stdio.h>
#include <stdlib.h>
#include <webp/demux.h>
#include <webp/decode.h>
#include <tracker-common.h>
#include "tracker-extract.h"
#include "tracker-guarantee.h"
#include "tracker-exif.h"
#include "tracker-iptc.h"
#include "tracker-xmp.h"
#include "tracker-resource-helpers.h"

G_MODULE_EXPORT gboolean
tracker_extract_get_metadata (TrackerExtractInfo  *info,
                              GError             **error)
{
	GFile *file;
	gchar *filename;
	FILE *f = NULL;
	uint8_t *data = NULL;
	size_t data_size = 0;
	gboolean success = FALSE;
	TrackerResource *metadata = NULL;
	g_autofree char *resource_uri = NULL;
	WebPDemuxer *demux = NULL;
	WebPData webp_data;
	uint32_t width, height, flags;
	WebPChunkIterator chunk_iter;

	file = tracker_extract_info_get_file (info);
	filename = g_file_get_path (file);
	f = fopen (filename, "rb");
	if (!f)
		goto cleanup;

	fseek (f, 0, SEEK_END);
	data_size = ftell (f);
	fseek (f, 0, SEEK_SET);
	data = malloc (data_size);
	if (fread (data, 1, data_size, f) != data_size)
		goto cleanup;

	webp_data = (WebPData) { data, data_size };
	demux = WebPDemux (&webp_data);
	if (!demux)
		goto cleanup;

	width = WebPDemuxGetI (demux, WEBP_FF_CANVAS_WIDTH);
	height = WebPDemuxGetI (demux, WEBP_FF_CANVAS_HEIGHT);
	flags = WebPDemuxGetI (demux, WEBP_FF_FORMAT_FLAGS);

	resource_uri = tracker_extract_info_get_content_id (info, NULL);
	metadata = tracker_resource_new (resource_uri);
	tracker_resource_add_uri (metadata, "rdf:type", "nfo:Image");
	tracker_resource_add_uri (metadata, "rdf:type", "nmm:Photo");
	tracker_resource_set_int64 (metadata, "nfo:width", width);
	tracker_resource_set_int64 (metadata, "nfo:height", height);

	if ((flags & EXIF_FLAG) && WebPDemuxGetChunk (demux, "EXIF", 1, &chunk_iter)) {
		TrackerExifData *exif;

		exif = tracker_exif_new (chunk_iter.chunk.bytes, chunk_iter.chunk.size, filename);

		if (exif) {
			tracker_exif_apply_to_resource (metadata, exif);
			tracker_exif_free (exif);
		}

		WebPDemuxReleaseChunkIterator (&chunk_iter);
	}

	if ((flags & XMP_FLAG) && WebPDemuxGetChunk (demux, "XMP ", 1, &chunk_iter)) {
		TrackerXmpData *xmp;

		xmp = tracker_xmp_new ((const gchar *) chunk_iter.chunk.bytes, chunk_iter.chunk.size, filename);

		if (xmp) {
			tracker_xmp_apply_to_resource (metadata, xmp);
			tracker_xmp_free (xmp);
		}

		WebPDemuxReleaseChunkIterator (&chunk_iter);
	}

	tracker_extract_info_set_resource (info, metadata);
	success = TRUE;

cleanup:
	if (demux)
		WebPDemuxDelete (demux);

	if (f)
		fclose (f);

	free (data);
	g_free (filename);
	g_clear_object (&metadata);

	return success;
}
