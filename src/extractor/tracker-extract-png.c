/*
 * Copyright (C) 2006, Jamie McCracken <jamiemcc@gnome.org>
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

#include <png.h>

#include <tracker-common.h>

#include "utils/tracker-extract.h"

#ifdef HAVE_EXEMPI
#include "tracker-xmp.h"
#endif

#ifdef HAVE_GEXIV2
#include "tracker-exif.h"
#include <gexiv2/gexiv2.h>
#endif

#define RFC1123_DATE_FORMAT "%d %B %Y %H:%M:%S %z"
#define CMS_PER_INCH        2.54

typedef struct {
	const gchar *author;
	const gchar *creator;
	const gchar *description;
	const gchar *comment;
	const gchar *copyright;
	gchar *creation_time;
	const gchar *title;
	const gchar *disclaimer;
	const gchar *software;
} PngData;

static gchar *
rfc1123_to_iso8601_date (const gchar *date)
{
	/* From: ex. RFC1123 date: "22 May 1997 18:07:10 -0600"
	 * To  : ex. ISO8601 date: "2007-05-22T18:07:10-0600"
	 */
	return tracker_date_format_to_iso8601 (date, RFC1123_DATE_FORMAT);
}

#if defined(PNG_iTXt_SUPPORTED) && (defined(HAVE_EXEMPI) || defined(HAVE_GEXIV2))

/* Handle raw profiles by Imagemagick (at least). Hex encoded with
 * line-changes and other (undocumented/unofficial) twists.
 */
static gchar *
raw_profile_new (const gchar *input,
                 const guint  input_length,
                 guint       *output_length)
{
	static const gchar* const lut = "0123456789abcdef";
	gchar *output;
	const gchar *ptr;
	const gchar *length_ptr;
	gsize size;
	gchar *length_str;
	guint length;

	size_t len;
	size_t i;
	size_t o;
	char *p;
	char *q;

	ptr = input;

	if (*ptr != '\n') {
		return NULL;
	}

	ptr++;

	if (!g_ascii_isalpha (*ptr)) {
		return NULL;
	}

	/* Skip the type string */
	do {
		ptr++;
	} while (g_ascii_isalpha (*ptr));

	if (*ptr != '\n') {
		return NULL;
	}

	/* Hop over whitespaces */
	do {
		ptr++;
	} while (*ptr == ' ');

	if (!g_ascii_isdigit (*ptr)) {
		return NULL;
	}

	/* Get the length string */
	length_ptr = ptr;
	size = 1;

	do {
		ptr++;
		size++;
	} while (g_ascii_isdigit (*ptr));

	if (*ptr != '\n') {
		return NULL;
	}

	ptr++;

	length_str = g_strndup (length_ptr, size - 1);
	length = atoi (length_str);
	g_free (length_str);

	len = length;
	i = 0;
	o = 0;

	output = malloc (length + 1); /* A bit less with non-valid */

	o = 0;
	while (o < len) {
		do {
			gchar a = ptr[i];
			p = strchr (lut, a);
			i++;
		} while (p == 0);

		do {
			gchar b = ptr[i];
			q = strchr (lut, b);
			i++;
		} while (q == 0);

		output[o] = (((p - lut) << 4) | (q - lut));
		o++;
	}

	output[o] = '\0';
	*output_length = o;

	return output;
}

#endif /* defined(PNG_iTXt_SUPPORTED) && (defined(HAVE_EXEMPI) || defined(HAVE_GEXIV2)) */

static void
read_metadata (TrackerResource      *metadata,
               png_structp           png_ptr,
               png_infop             info_ptr,
               png_infop             end_ptr,
               GFile                *file,
               const gchar          *uri)
{
	PngData pd = { 0 };
#ifdef HAVE_GEXIV2
	TrackerExifData *ed = NULL;
#endif
#ifdef HAVE_EXEMPI
	TrackerXmpData *xd = NULL;
#endif
	png_infop info_ptrs[2];
	png_textp text_ptr;
	gint info_index;
	gint num_text;
	gint i;
	gint found;

	info_ptrs[0] = info_ptr;
	info_ptrs[1] = end_ptr;

	for (info_index = 0; info_index < 2; info_index++) {
		if ((found = png_get_text (png_ptr, info_ptrs[info_index], &text_ptr, &num_text)) < 1) {
			g_debug ("Calling png_get_text() returned %d (< 1)", found);
			continue;
		}

		for (i = 0; i < num_text; i++) {
			if (!text_ptr[i].key || !text_ptr[i].text || text_ptr[i].text[0] == '\0') {
				continue;
			}

#if defined(HAVE_EXEMPI) && defined(PNG_iTXt_SUPPORTED)
			if (g_strcmp0 ("XML:com.adobe.xmp", text_ptr[i].key) == 0) {
				/* ATM tracker_extract_xmp_read supports setting xd
				 * multiple times, keep it that way as here it's
				 * theoretically possible that the function gets
				 * called multiple times
				 */
				xd = tracker_xmp_new (text_ptr[i].text,
				                      text_ptr[i].itxt_length,
				                      uri);

				continue;
			}

			if (!xd && g_strcmp0 ("Raw profile type xmp", text_ptr[i].key) == 0) {
				gchar *xmp_buffer;
				guint xmp_buffer_length = 0;
				guint input_len;

				if (text_ptr[i].text_length) {
					input_len = text_ptr[i].text_length;
				} else {
					input_len = text_ptr[i].itxt_length;
				}

				xmp_buffer = raw_profile_new (text_ptr[i].text,
				                              input_len,
				                              &xmp_buffer_length);

				if (xmp_buffer) {
					xd = tracker_xmp_new (xmp_buffer,
					                      xmp_buffer_length,
					                      uri);
				}

				g_free (xmp_buffer);

				continue;
			}
#endif /*HAVE_EXEMPI && PNG_iTXt_SUPPORTED */

#if defined(HAVE_GEXIV2) && defined(PNG_iTXt_SUPPORTED)
			if (!ed && g_strcmp0 ("Raw profile type exif", text_ptr[i].key) == 0) {
				gchar *exif_buffer;
				guint exif_buffer_length = 0;
				guint input_len;

				if (text_ptr[i].text_length) {
					input_len = text_ptr[i].text_length;
				} else {
					input_len = text_ptr[i].itxt_length;
				}

				exif_buffer = raw_profile_new (text_ptr[i].text,
				                               input_len,
				                               &exif_buffer_length);

				if (exif_buffer) {
					ed = tracker_exif_new (exif_buffer,
					                       exif_buffer_length,
					                       uri);

					if (!ed) {
						GExiv2Metadata *metadata;

						metadata = gexiv2_metadata_new ();
						if (gexiv2_metadata_open_path (metadata,
						                               g_file_peek_path (file),
						                               NULL)) {
							ed = tracker_exif_new_from_metadata (metadata);
						}

						g_clear_object (&metadata);
					}
				}

				g_free (exif_buffer);

				continue;
			}
#endif /* HAVE_GEXIV2 && PNG_iTXt_SUPPORTED */

			if (g_strcmp0 (text_ptr[i].key, "Author") == 0) {
				pd.author = text_ptr[i].text;
				continue;
			}

			if (g_strcmp0 (text_ptr[i].key, "Creator") == 0) {
				pd.creator = text_ptr[i].text;
				continue;
			}

			if (g_strcmp0 (text_ptr[i].key, "Description") == 0) {
				pd.description = text_ptr[i].text;
				continue;
			}

			if (g_strcmp0 (text_ptr[i].key, "Comment") == 0) {
				pd.comment = text_ptr[i].text;
				continue;
			}

			if (g_strcmp0 (text_ptr[i].key, "Copyright") == 0) {
				pd.copyright = text_ptr[i].text;
				continue;
			}

			if (g_strcmp0 (text_ptr[i].key, "Creation Time") == 0) {
				g_clear_pointer (&pd.creation_time, g_free);
				pd.creation_time = rfc1123_to_iso8601_date (text_ptr[i].text);
				continue;
			}

			if (g_strcmp0 (text_ptr[i].key, "Title") == 0) {
				pd.title = text_ptr[i].text;
				continue;
			}

			if (g_strcmp0 (text_ptr[i].key, "Disclaimer") == 0) {
				pd.disclaimer = text_ptr[i].text;
				continue;
			}

			if (g_strcmp0(text_ptr[i].key, "Software") == 0) {
				pd.software = text_ptr[i].text;
				continue;
			}
		}
	}

#ifdef HAVE_EXEMPI
	if (!xd) {
		gchar *sidecar = NULL;

		xd = tracker_xmp_new_from_sidecar (file, &sidecar);

		if (sidecar) {
			TrackerResource *sidecar_resource;

			sidecar_resource = tracker_resource_new (sidecar);
			tracker_resource_add_uri (sidecar_resource, "rdf:type", "nfo:FileDataObject");
			tracker_resource_set_uri (sidecar_resource, "nie:interpretedAs",
			                          tracker_resource_get_identifier (metadata));

			tracker_resource_add_take_relation (metadata, "nie:isStoredAs", sidecar_resource);
		}
	}
#endif

	if (pd.comment) {
		tracker_guarantee_resource_utf8_string (metadata, "nie:comment", pd.comment);
	}

	if (pd.disclaimer) {
		tracker_guarantee_resource_utf8_string (metadata, "nie:license", pd.disclaimer);
	}

	if (pd.creator || pd.author) {
		TrackerResource *creator;
		const char *str = tracker_coalesce_strip (2, pd.creator, pd.author);

		creator = tracker_extract_new_contact (str);
		tracker_resource_set_relation (metadata, "nco:creator", creator);
		g_object_unref (creator);
	}

	tracker_guarantee_resource_date_from_file_mtime (metadata,
	                                                 "nie:contentCreated",
	                                                 pd.creation_time,
	                                                 uri);

	if (pd.description) {
		tracker_guarantee_resource_utf8_string (metadata, "nie:description", pd.description);
	}

	if (pd.copyright) {
		tracker_guarantee_resource_utf8_string (metadata, "nie:copyright", pd.copyright);
	}

	tracker_guarantee_resource_title_from_file (metadata,
	                                            "nie:title",
	                                            pd.title,
	                                            uri,
	                                            NULL);

	if (g_strcmp0(pd.software, "gnome-screenshot") == 0) {
		tracker_resource_add_uri (metadata, "nie:isLogicalPartOf", "nfo:image-category-screenshot");
	}

#ifdef HAVE_EXEMPI
	if (xd) {
		tracker_xmp_apply_to_resource (metadata, xd);
		tracker_xmp_free (xd);
	}
#endif
#ifdef HAVE_GEXIV2
	if (ed) {
		tracker_exif_apply_to_resource (metadata, ed);
		tracker_exif_free (ed);
	}
#endif
	g_free (pd.creation_time);
}

static gboolean
guess_dlna_profile (gint          depth,
                    gint          width,
                    gint          height,
                    const gchar **dlna_profile,
                    const gchar **dlna_mimetype)
{
	const gchar *profile = NULL;

	if (dlna_profile) {
		*dlna_profile = NULL;
	}

	if (dlna_mimetype) {
		*dlna_mimetype = NULL;
	}

	if (width == 120 && height == 120) {
		profile = "PNG_LRG_ICO";
	} else if (width == 48 && height == 48) {
		profile = "PNG_SM_ICO";
	} else if (width <= 160 && height <= 160) {
		profile = "PNG_TN";
	} else if (depth <= 32 && width <= 4096 && height <= 4096) {
		profile = "PNG_LRG";
	}

	if (profile) {
		if (dlna_profile) {
			*dlna_profile = profile;
		}

		if (dlna_mimetype) {
			*dlna_mimetype = "image/png";
		}

		return TRUE;
	}

	return FALSE;
}

G_MODULE_EXPORT gboolean
tracker_extract_get_metadata (TrackerExtractInfo  *info,
                              GError             **error)
{
	TrackerResource *metadata;
	goffset size;
	FILE *f;
	png_structp png_ptr;
	png_infop info_ptr;
	png_infop end_ptr;
	png_bytep row_data;
	guint row;
	png_uint_32 width, height;
	gint bit_depth, color_type;
	gint interlace_type, compression_type, filter_type;
	const gchar *dlna_profile, *dlna_mimetype;
	gchar *filename, *uri, *resource_uri;
	GFile *file;

	file = tracker_extract_info_get_file (info);
	filename = g_file_get_path (file);
	size = tracker_file_get_size (filename);

	if (size < 64) {
		g_set_error (error,
		             G_IO_ERROR,
		             G_IO_ERROR_INVALID_DATA,
		             "File too small to be a PNG");
		return FALSE;
	}

	f = tracker_file_open (filename);
	g_free (filename);

	if (!f) {
		return FALSE;
	}

	png_ptr = png_create_read_struct (PNG_LIBPNG_VER_STRING,
	                                  NULL,
	                                  NULL,
	                                  NULL);
	if (!png_ptr) {
		tracker_file_close (f, FALSE);
		return FALSE;
	}

	info_ptr = png_create_info_struct (png_ptr);
	if (!info_ptr) {
		png_destroy_read_struct (&png_ptr, NULL, NULL);
		tracker_file_close (f, FALSE);
		return FALSE;
	}

	end_ptr = png_create_info_struct (png_ptr);
	if (!end_ptr) {
		png_destroy_read_struct (&png_ptr, &info_ptr, NULL);
		tracker_file_close (f, FALSE);
		return FALSE;
	}

	if (setjmp (png_jmpbuf (png_ptr))) {
		png_destroy_read_struct (&png_ptr, &info_ptr, &end_ptr);
		tracker_file_close (f, FALSE);
		return FALSE;
	}

	png_init_io (png_ptr, f);
	png_read_info (png_ptr, info_ptr);

	if (!png_get_IHDR (png_ptr,
	                   info_ptr,
	                   &width,
	                   &height,
	                   &bit_depth,
	                   &color_type,
	                   &interlace_type,
	                   &compression_type,
	                   &filter_type)) {
		png_destroy_read_struct (&png_ptr, &info_ptr, &end_ptr);
		tracker_file_close (f, FALSE);
		return FALSE;
	}

	/* Read the image. FIXME We should be able to skip this step and
	 * just get the info from the end. This causes some errors atm.
	 */
	row_data = png_malloc (png_ptr, png_get_rowbytes (png_ptr, info_ptr));
	for (row = 0; row < height; row++)
		png_read_row (png_ptr, row_data, NULL);
	png_free (png_ptr, row_data);

	png_read_end (png_ptr, end_ptr);

	resource_uri = tracker_extract_info_get_content_id (info, NULL);
	metadata = tracker_resource_new (resource_uri);
	g_free (resource_uri);

	tracker_resource_add_uri (metadata, "rdf:type", "nfo:Image");
	tracker_resource_add_uri (metadata, "rdf:type", "nmm:Photo");

	uri = g_file_get_uri (file);

	read_metadata (metadata, png_ptr, info_ptr, end_ptr, file, uri);
	g_free (uri);

	tracker_resource_set_int64 (metadata, "nfo:width", width);
	tracker_resource_set_int64 (metadata, "nfo:height", height);

	if (guess_dlna_profile (bit_depth, width, height, &dlna_profile, &dlna_mimetype)) {
		tracker_resource_set_string (metadata, "nmm:dlnaProfile", dlna_profile);
		tracker_resource_set_string (metadata, "nmm:dlnaMime", dlna_mimetype);
	}

	png_destroy_read_struct (&png_ptr, &info_ptr, &end_ptr);
	tracker_file_close (f, FALSE);

	tracker_extract_info_set_resource (info, metadata);
	g_object_unref (metadata);

	return TRUE;
}
