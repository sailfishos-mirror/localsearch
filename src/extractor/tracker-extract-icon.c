/*
 * Copyright (C) 2010, Nokia <ivan.frade@nokia.com>
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

#include <tracker-common.h>

#include "utils/tracker-extract.h"

#define ICON_HEADER_SIZE_16 3
#define ICON_IMAGE_METADATA_SIZE_8 16
#define MAX_IMAGES 16

enum {
	POS_WIDTH = 0,
	POS_HEIGHT,
};

static gboolean
find_max_width_and_height (GFile   *file,
                           guint   *width,
                           guint   *height,
                           GError **error)
{
	g_autoptr (GFileInputStream) stream = NULL;
	guint n_images;
	guint i;
	guint16 header[ICON_HEADER_SIZE_16];

	*width = 0;
	*height = 0;

	stream = g_file_read (file, NULL, error);
	if (!stream)
		return FALSE;

	/* Header consists of:
	 *  - 2bytes, reserved, must be 0
	 *  - 2bytes, image type (1:icon, 2:cursor, other values invalid)
	 *  - 2bytes, number of images in the file.
	 *
	 * Right now we just need the number of images in the file.
	 */
	if (!g_input_stream_read_all (G_INPUT_STREAM (stream),
	                              header,
	                              ICON_HEADER_SIZE_16 * 2,
	                              NULL,
	                              NULL,
	                              error))
		return FALSE;

	n_images = GUINT16_FROM_LE (header[2]);
	g_debug ("Found '%u' images in the icon file...", n_images);

	/* Loop images looking for the biggest one... */
	for (i = 0; i < MIN (MAX_IMAGES, n_images); i++) {
		guint8 image_metadata[ICON_IMAGE_METADATA_SIZE_8];
		guint cur_width, cur_height;

		/* Image metadata chunk consists of:
		 *  - 1 byte, width in pixels, 0 means 256
		 *  - 1 byte, height in pixels, 0 means 256
		 *  - Plus some other stuff we don't care about...
		 */
		if (!g_input_stream_read_all (G_INPUT_STREAM (stream),
		                              image_metadata,
		                              ICON_IMAGE_METADATA_SIZE_8,
		                              NULL,
		                              NULL,
		                              error))
			return FALSE;

		g_debug ("  Image '%u'; width:%u height:%u",
		         i,
		         image_metadata[0],
		         image_metadata[1]);

		/* Width... */
		cur_width = image_metadata[POS_WIDTH] != 0 ?
		            image_metadata[POS_WIDTH] : 256;
		*width = MAX (*width, cur_width);

		/* Height... */
		cur_height = image_metadata[POS_HEIGHT] != 0 ?
		             image_metadata[POS_HEIGHT] : 256;
		*height = MAX (*height, cur_height);
	}

	g_input_stream_close (G_INPUT_STREAM (stream), NULL, NULL);
	return TRUE;
}

G_MODULE_EXPORT gboolean
tracker_extract_get_metadata (TrackerExtractInfo  *info,
                              GError             **error)
{
	g_autoptr (TrackerResource) metadata = NULL;
	g_autofree char *resource_uri = NULL;
	guint max_width;
	guint max_height;
	GFile *file;

	file = tracker_extract_info_get_file (info);

	/* The Windows Icon file format may contain the same icon with different
	 * sizes inside, so there's no clear way of setting single width and
	 * height values. Thus, we set maximum sizes found. */
	if (!find_max_width_and_height (file, &max_width, &max_height, error))
		return FALSE;

	resource_uri = tracker_extract_info_get_content_id (info, NULL);
	metadata = tracker_resource_new (resource_uri);

	tracker_resource_add_uri (metadata, "rdf:type", "nfo:Image");
	tracker_resource_add_uri (metadata, "rdf:type", "nfo:Icon");

	tracker_resource_set_int64 (metadata, "nfo:width", max_width);
	tracker_resource_set_int64 (metadata, "nfo:height", max_height);

	tracker_extract_info_set_resource (info, metadata);

	return TRUE;
}
