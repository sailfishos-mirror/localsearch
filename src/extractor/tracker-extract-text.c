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

#include <tracker-common.h>

#include "utils/tracker-extract.h"

#include "tracker-main.h"
#include "tracker-extract.h"

G_MODULE_EXPORT gboolean
tracker_extract_get_metadata (TrackerExtractInfo  *info,
                              GError             **error)
{
	g_autoptr (TrackerResource) metadata = NULL;
	g_autofree char *resource_uri = NULL;
	g_autoptr (GInputStream) stream = NULL;
	g_autoptr (GBytes) bytes = NULL;

	stream = G_INPUT_STREAM (g_file_read (tracker_extract_info_get_file (info),
	                                      NULL, error));
	if (!stream)
		return FALSE;

	bytes = g_input_stream_read_bytes (stream,
	                                   tracker_extract_info_get_max_text (info),
	                                   NULL,
	                                   error);
	if (!bytes)
		return FALSE;

	resource_uri = tracker_extract_info_get_content_id (info, NULL);
	metadata = tracker_resource_new (resource_uri);
	tracker_resource_add_uri (metadata, "rdf:type", "nfo:PlainTextDocument");

	if (g_bytes_get_size (bytes) > 0) {
		const char *str, *end;
		gssize size;

		str = g_bytes_get_data (bytes, &size);

		if (g_utf8_validate_len (str, size, &end) &&
		    (end - str) - size < 4) {
			g_autofree char *copy = NULL;

			copy = g_strndup (str, end - str);
			tracker_resource_set_string (metadata, "nie:plainTextContent", copy);
		} else if (size > 2) {
			g_autofree char *converted = NULL;

			/* Support also UTF-16 encoded text files, as the ones generated in
			 * Windows OS. We will only accept text files in UTF-16 which come
			 * with a proper BOM.
			 */
			if (memcmp (str, "\xFF\xFE", 2) == 0) {
				g_debug ("String comes in UTF-16LE, converting");
				converted = g_convert (&str[2],
				                       size - 2,
				                       "UTF-8",
				                       "UTF-16LE",
				                       NULL, NULL, NULL);
				g_print ("HMM %s\n", converted);
			} else if (memcmp (str, "\xFE\xFF", 2) == 0) {
				g_debug ("String comes in UTF-16BE, converting");
				converted = g_convert (&str[2],
				                       size - 2,
				                       "UTF-8",
				                       "UTF-16BE",
				                       NULL, NULL, NULL);
			} else {
				/* Fallback to windows-1252 */
				converted = g_convert (str,
				                       size,
				                       "UTF-8",
				                       "windows-1252",
				                       NULL, NULL, NULL);
			}

			if (converted)
				tracker_resource_set_string (metadata, "nie:plainTextContent", converted);
		}
	}

	tracker_extract_info_set_resource (info, metadata);

	return TRUE;
}
