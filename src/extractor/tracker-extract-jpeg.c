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

#include <stdio.h>
#include <setjmp.h>

#include <jpeglib.h>

#include <tracker-common.h>

#include "utils/tracker-extract.h"

#include "tracker-main.h"

#ifdef HAVE_EXEMPI
#include "tracker-xmp.h"
#endif
#ifdef HAVE_GEXIV2
#include "tracker-iptc.h"
#include <gexiv2/gexiv2.h>
#endif

#define CMS_PER_INCH            2.54

#ifdef HAVE_EXEMPI
#define XMP_NAMESPACE           "http://ns.adobe.com/xap/1.0/\x00"
#define XMP_NAMESPACE_LENGTH    29
#endif /* HAVE_EXEMPI */

#ifdef HAVE_GEXIV2
#define EXIF_NAMESPACE          "Exif"
#define EXIF_NAMESPACE_LENGTH   4
#define PS3_NAMESPACE           "Photoshop 3.0\0"
#define PS3_NAMESPACE_LENGTH    14
#endif /* HAVE_GEXIV2 */

enum {
	JPEG_RESOLUTION_UNIT_UNKNOWN = 0,
	JPEG_RESOLUTION_UNIT_PER_INCH = 1,
	JPEG_RESOLUTION_UNIT_PER_CENTIMETER = 2,
};

typedef struct {
	const gchar *make;
	const gchar *model;
	const gchar *title;
	const gchar *orientation;
	const gchar *copyright;
	const gchar *white_balance;
	const gchar *fnumber;
	const gchar *flash;
	const gchar *focal_length;
	const gchar *artist;
	const gchar *exposure_time;
	const gchar *iso_speed_ratings;
	const gchar *date;
	const gchar *description;
	const gchar *metering_mode;
	const gchar *comment;
	const gchar *gps_altitude;
	const gchar *gps_latitude;
	const gchar *gps_longitude;
	const gchar *gps_direction;
} MergeData;

struct tej_error_mgr {
	struct jpeg_error_mgr jpeg;
	jmp_buf setjmp_buffer;
};

static void
extract_jpeg_error_exit (j_common_ptr cinfo)
{
	struct tej_error_mgr *h = (struct tej_error_mgr *) cinfo->err;
	(*cinfo->err->output_message)(cinfo);
	longjmp (h->setjmp_buffer, 1);
}

static gboolean
guess_dlna_profile (gint          width,
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

	if (width <= 640 && height <= 480) {
		profile = "JPEG_SM";
	} else if (width <= 1024 && height <= 768) {
		profile = "JPEG_MED";
	} else if (width <= 4096 && height <= 4096) {
		profile = "JPEG_LRG";
	}

	if (profile) {
		if (dlna_profile) {
			*dlna_profile = profile;
		}

		if (dlna_mimetype) {
			*dlna_mimetype = "image/jpeg";
		}

		return TRUE;
	}

	return FALSE;
}

G_MODULE_EXPORT gboolean
tracker_extract_get_metadata (TrackerExtractInfo  *info,
                              GError             **error)
{
	struct jpeg_decompress_struct cinfo = { 0, };
	struct tej_error_mgr tejerr;
	struct jpeg_marker_struct *marker;
	TrackerResource *metadata = NULL;
#ifdef HAVE_EXEMPI
	TrackerXmpData *xd = NULL;
#endif
#ifdef HAVE_GEXIV2
	TrackerIptcData *id = NULL;
#endif
	TrackerExifData *ed = NULL;
	MergeData md = { 0 };
	GFile *file;
	FILE *f;
	goffset size;
	g_autofree char *resource_uri = NULL;
	gchar *filename, *uri;
	gchar *comment = NULL;
	const gchar *dlna_profile, *dlna_mimetype;
	gboolean success = TRUE;

	file = tracker_extract_info_get_file (info);
	filename = g_file_get_path (file);

	size = tracker_file_get_size (filename);

	if (size < 18) {
		g_free (filename);
		return FALSE;
	}

	f = tracker_file_open (filename);
	g_free (filename);

	if (!f) {
		return FALSE;
	}

	uri = g_file_get_uri (file);

	cinfo.err = jpeg_std_error (&tejerr.jpeg);
	tejerr.jpeg.error_exit = extract_jpeg_error_exit;
	if (setjmp (tejerr.setjmp_buffer)) {
		success = FALSE;
		goto fail;
	}

	jpeg_create_decompress (&cinfo);

	jpeg_save_markers (&cinfo, JPEG_COM, 0xFFFF);
	jpeg_save_markers (&cinfo, JPEG_APP0 + 1, 0xFFFF);
	jpeg_save_markers (&cinfo, JPEG_APP0 + 13, 0xFFFF);

	jpeg_stdio_src (&cinfo, f);

	jpeg_read_header (&cinfo, TRUE);

	/* FIXME? It is possible that there are markers after SOS,
	 * but there shouldn't be. Should we decompress the whole file?
	 *
	 * jpeg_start_decompress(&cinfo);
	 * jpeg_finish_decompress(&cinfo);
	 *
	 * jpeg_calc_output_dimensions(&cinfo);
	 */

	resource_uri = tracker_extract_info_get_content_id (info, NULL);
	metadata = tracker_resource_new (resource_uri);
	tracker_resource_add_uri (metadata, "rdf:type", "nfo:Image");
	tracker_resource_add_uri (metadata, "rdf:type", "nmm:Photo");

	marker = (struct jpeg_marker_struct *) &cinfo.marker_list;

	while (marker) {
		gchar *str;
		gsize len;

		switch (marker->marker) {
		case JPEG_COM:
			g_free (comment);
			comment = g_strndup ((gchar*) marker->data, marker->data_length);
			break;

		case JPEG_APP0 + 1:
			str = (gchar*) marker->data;
			len = marker->data_length;

#ifdef HAVE_GEXIV2
			if (len > 0 && strncmp (EXIF_NAMESPACE, str, EXIF_NAMESPACE_LENGTH) == 0) {
				GExiv2Metadata *metadata = gexiv2_metadata_new ();

				if (gexiv2_metadata_from_app1_segment (metadata, (const guint8 *) str, len, NULL))
					ed = tracker_exif_new_from_metadata (metadata);

				g_object_unref (metadata);
			}
#endif /* HAVE_GEXIV2 */

#ifdef HAVE_EXEMPI
			if (!xd && strncmp (XMP_NAMESPACE, str, XMP_NAMESPACE_LENGTH) == 0) {
				xd = tracker_xmp_new (str + XMP_NAMESPACE_LENGTH,
				                      len - XMP_NAMESPACE_LENGTH,
				                      uri);
			}
#endif /* HAVE_EXEMPI */

			break;

		case JPEG_APP0 + 13:
			str = (gchar*) marker->data;
			len = marker->data_length;
#ifdef HAVE_GEXIV2
			if (len > 0 && strncmp(PS3_NAMESPACE, str, PS3_NAMESPACE_LENGTH) == 0) {
				const gchar *filepath = g_file_peek_path(file);
				GError *error = NULL;
				GExiv2Metadata *metadata = gexiv2_metadata_new();

				if (gexiv2_metadata_open_path(metadata, filepath, &error)) {
					id = tracker_iptc_new_from_metadata(metadata);
					g_object_unref(metadata);
				} else {
					g_object_unref(metadata);
					if (error) {
						g_error_free(error);
					}
					id = NULL;
				}
			}
#endif /* HAVE_GEXIV2 */

			break;

		default:
			marker = marker->next;
			continue;
		}

		marker = marker->next;
	}

	if (!ed) {
		ed = g_new0 (TrackerExifData, 1);
	}

#ifdef HAVE_EXEMPI
	if (!xd) {
		gchar *sidecar = NULL;

		xd = tracker_xmp_new_from_sidecar (file, &sidecar);

		if (sidecar) {
			TrackerResource *sidecar_resource;

			sidecar_resource = tracker_resource_new (sidecar);
			tracker_resource_add_uri (sidecar_resource, "rdf:type", "nfo:FileDataObject");
			tracker_resource_set_uri (sidecar_resource, "nie:interpretedAs", resource_uri);

			tracker_resource_add_take_relation (metadata, "nie:isStoredAs", sidecar_resource);
		}
	}
#endif

	md.title = tracker_coalesce_strip (1, ed->document_name);
	md.orientation = tracker_coalesce_strip (1, ed->orientation);
	md.copyright = tracker_coalesce_strip (1, ed->copyright);
	md.white_balance = tracker_coalesce_strip (1, ed->white_balance);
	md.fnumber = tracker_coalesce_strip (1, ed->fnumber);
	md.flash = tracker_coalesce_strip (1, ed->flash);
	md.focal_length =  tracker_coalesce_strip (1, ed->focal_length);
	md.artist = tracker_coalesce_strip (1, ed->artist);
	md.exposure_time = tracker_coalesce_strip (1, ed->exposure_time);
	md.iso_speed_ratings = tracker_coalesce_strip (1, ed->iso_speed_ratings);
	md.date = tracker_coalesce_strip (2, ed->time, ed->time_original);
	md.description = tracker_coalesce_strip (1, ed->description);
	md.metering_mode = tracker_coalesce_strip (1, ed->metering_mode);

	/* FIXME We are not handling the altitude ref here for xmp */
	md.gps_altitude = tracker_coalesce_strip (1, ed->gps_altitude);
	md.gps_latitude = tracker_coalesce_strip (1, ed->gps_latitude);
	md.gps_longitude = tracker_coalesce_strip (1, ed->gps_longitude);
	md.gps_direction = tracker_coalesce_strip (1, ed->gps_direction);
	md.comment = tracker_coalesce_strip (2, comment, ed->user_comment);
	md.make = tracker_coalesce_strip (1, ed->make);
	md.model = tracker_coalesce_strip (1, ed->model);

	/* Prioritize on native dimention in all cases */
	tracker_resource_set_int64 (metadata, "nfo:width", cinfo.image_width);
	tracker_resource_set_int64 (metadata, "nfo:height", cinfo.image_height);

	if (guess_dlna_profile (cinfo.image_width, cinfo.image_height, &dlna_profile, &dlna_mimetype)) {
		tracker_resource_set_string (metadata, "nmm:dlnaProfile", dlna_profile);
		tracker_resource_set_string (metadata, "nmm:dlnaMime", dlna_mimetype);
	}

	if (md.make || md.model) {
		TrackerResource *equipment = tracker_extract_new_equipment (md.make, md.model);
		tracker_resource_set_relation (metadata, "nfo:equipment", equipment);
		g_object_unref (equipment);
	}

	tracker_guarantee_resource_title_from_file (metadata,
	                                            "nie:title",
	                                            md.title,
	                                            uri,
	                                            NULL);

	if (md.orientation) {
		TrackerResource *orientation;

		orientation = tracker_resource_new (md.orientation);
		tracker_resource_set_relation (metadata, "nfo:orientation", orientation);
		g_object_unref (orientation);
	}

	if (md.copyright) {
		tracker_guarantee_resource_utf8_string (metadata, "nie:copyright", md.copyright);
	}

	if (md.white_balance) {
		TrackerResource *white_balance;

		white_balance = tracker_resource_new (md.white_balance);
		tracker_resource_set_relation (metadata, "nmm:whiteBalance", white_balance);
		g_object_unref (white_balance);
	}

	if (md.fnumber) {
		gdouble value;

		value = g_strtod (md.fnumber, NULL);
		tracker_resource_set_double (metadata, "nmm:fnumber", value);
	}

	if (md.flash) {
		TrackerResource *flash;

		flash = tracker_resource_new (md.flash);
		tracker_resource_set_relation (metadata, "nmm:flash", flash);
		g_object_unref (flash);
	}

	if (md.focal_length) {
		gdouble value;

		value = g_strtod (md.focal_length, NULL);
		tracker_resource_set_double (metadata, "nmm:focalLength", value);
	}

	if (md.artist) {
		TrackerResource *artist = tracker_extract_new_contact (md.artist);
		tracker_resource_add_relation (metadata, "nco:contributor", artist);
		g_object_unref (artist);
	}

	if (md.exposure_time) {
		gdouble value;

		value = g_strtod (md.exposure_time, NULL);
		tracker_resource_set_double (metadata, "nmm:exposureTime", value);
	}

	if (md.iso_speed_ratings) {
		gdouble value;

		value = g_strtod (md.iso_speed_ratings, NULL);
		tracker_resource_set_double (metadata, "nmm:isoSpeed", value);
	}

	tracker_guarantee_resource_date_from_file_mtime (metadata,
	                                                 "nie:contentCreated",
	                                                 md.date,
	                                                 uri);

	if (md.description) {
		tracker_guarantee_resource_utf8_string (metadata, "nie:description", md.description);
	}

	if (md.metering_mode) {
		TrackerResource *metering;

		metering = tracker_resource_new (md.metering_mode);
		tracker_resource_set_relation (metadata, "nmm:meteringMode", metering);
		g_object_unref (metering);
	}

	if (md.comment) {
		tracker_guarantee_resource_utf8_string (metadata, "nie:comment", md.comment);
	}

	if (md.gps_altitude || md.gps_latitude || md.gps_longitude) {
		TrackerResource *location;

		location = tracker_extract_new_location (NULL, NULL, NULL, NULL,
		                                         md.gps_altitude, md.gps_latitude,
		                                         md.gps_longitude);

		tracker_resource_set_relation (metadata, "slo:location", location);

		g_object_unref (location);
	}

	if (md.gps_direction) {
		tracker_resource_set_string (metadata, "nfo:heading", md.gps_direction);
	}

	if (cinfo.density_unit != 0 || ed->x_resolution) {
		gdouble value;

		if (cinfo.density_unit == JPEG_RESOLUTION_UNIT_UNKNOWN) {
			if (ed->resolution_unit == EXIF_RESOLUTION_UNIT_PER_CENTIMETER)
				value = g_strtod (ed->x_resolution, NULL) * CMS_PER_INCH;
			else
				value = g_strtod (ed->x_resolution, NULL);
		} else {
			if (cinfo.density_unit == JPEG_RESOLUTION_UNIT_PER_INCH)
				value = cinfo.X_density;
			else
				value = cinfo.X_density * CMS_PER_INCH;
		}

		tracker_resource_set_double (metadata, "nfo:horizontalResolution", value);
	}

	if (cinfo.density_unit != 0 || ed->y_resolution) {
		gdouble value;

		if (cinfo.density_unit == JPEG_RESOLUTION_UNIT_UNKNOWN) {
			if (ed->resolution_unit == EXIF_RESOLUTION_UNIT_PER_CENTIMETER)
				value = g_strtod (ed->y_resolution, NULL) * CMS_PER_INCH;
			else
				value = g_strtod (ed->y_resolution, NULL);
		} else {
			if (cinfo.density_unit == JPEG_RESOLUTION_UNIT_PER_INCH)
				value = cinfo.Y_density;
			else
				value = cinfo.Y_density * CMS_PER_INCH;
		}

		tracker_resource_set_double (metadata, "nfo:verticalResolution", value);
	}

#ifdef HAVE_EXEMPI
	if (xd)
		tracker_xmp_apply_to_resource (metadata, xd);
#endif
#ifdef HAVE_GEXIV2
	if (id)
		tracker_iptc_apply_to_resource (metadata, id);
#endif

	tracker_extract_info_set_resource (info, metadata);

	fail :
	jpeg_destroy_decompress (&cinfo);

	g_clear_pointer (&ed, tracker_exif_free);
#ifdef HAVE_EXEMPI
	g_clear_pointer (&xd, tracker_xmp_free);
#endif
#ifdef HAVE_GEXIV2
	g_clear_pointer (&id, tracker_iptc_free);
#endif
	g_clear_pointer (&comment, g_free);
	g_clear_object (&metadata);

	tracker_file_close (f, FALSE);
	g_free (uri);

	return success;
}
