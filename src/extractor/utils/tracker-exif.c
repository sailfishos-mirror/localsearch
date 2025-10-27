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

#include "config-miners.h"

#include <math.h>
#include <string.h>
#include <ctype.h>

#include "tracker-gexiv-compat.h"
#include "tracker-exif.h"
#include "tracker-resource-helpers.h"
#include "tracker-utils.h"

#include <gexiv2/gexiv2.h>

#define EXIF_DATE_FORMAT "%Y:%m:%d %H:%M:%S"

#define CMS_PER_INCH            2.54

enum {
	EXIF_FLASH_NONE = 0x0000,
	EXIF_FLASH_FIRED_MISSING_STROBE = 0x0005,
	EXIF_FLASH_DID_NOT_FIRE_COMPULSORY_ON = 0x0008,
	EXIF_FLASH_DID_NOT_FIRE_COMPULSORY_OFF = 0x0010,
	EXIF_FLASH_DID_NOT_FIRE_AUTO = 0x0018,
	EXIF_FLASH_DID_NOT_FIRE_AUTO_RED_EYE_REDUCTION = 0x0058,
};

enum {
	EXIF_METERING_MODE_UNKNOWN = 0,
	EXIF_METERING_MODE_AVERAGE = 1,
	EXIF_METERING_MODE_CENTER_WEIGHTED_AVERAGE = 2,
	EXIF_METERING_MODE_SPOT = 3,
	EXIF_METERING_MODE_MULTISPOT = 4,
	EXIF_METERING_MODE_PATTERN = 5,
	EXIF_METERING_MODE_PARTIAL = 6,
	EXIF_METERING_MODE_OTHER = 255,
};

static gchar *
convert_exif_orientation_to_nfo (GExiv2Orientation orientation)
{
	switch (orientation) {
	case GEXIV2_ORIENTATION_NORMAL:
		return g_strdup ("nfo:orientation-top");
	case GEXIV2_ORIENTATION_HFLIP:
		return g_strdup ("nfo:orientation-top-mirror");
	case GEXIV2_ORIENTATION_ROT_180:
		return g_strdup ("nfo:orientation-bottom");
	case GEXIV2_ORIENTATION_VFLIP:
		return g_strdup ("nfo:orientation-bottom-mirror");
	case GEXIV2_ORIENTATION_ROT_90_HFLIP:
		return g_strdup ("nfo:orientation-left-mirror");
	case GEXIV2_ORIENTATION_ROT_90:
		return g_strdup ("nfo:orientation-right");
	case GEXIV2_ORIENTATION_ROT_90_VFLIP:
		return g_strdup ("nfo:orientation-right-mirror");
	case GEXIV2_ORIENTATION_ROT_270:
		return g_strdup ("nfo:orientation-left");
	default:
		return NULL;
	}
}

static gchar *
convert_exif_flash_to_nmm (gulong flash)
{
	switch (flash) {
	case EXIF_FLASH_NONE:
	case EXIF_FLASH_FIRED_MISSING_STROBE:
	case EXIF_FLASH_DID_NOT_FIRE_COMPULSORY_ON:
	case EXIF_FLASH_DID_NOT_FIRE_COMPULSORY_OFF:
	case EXIF_FLASH_DID_NOT_FIRE_AUTO:
	case EXIF_FLASH_DID_NOT_FIRE_AUTO_RED_EYE_REDUCTION:
		return g_strdup ("nmm:flash-off");
	default:
		return g_strdup ("nmm:flash-on");
	}
}

static gchar *
convert_exif_metering_mode_to_nmm (gulong metering)
{
	switch (metering) {
	case EXIF_METERING_MODE_AVERAGE:
		return g_strdup ("nmm:metering-mode-average");
	case EXIF_METERING_MODE_CENTER_WEIGHTED_AVERAGE:
		return g_strdup ("nmm:metering-mode-center-weighted-average");
	case EXIF_METERING_MODE_SPOT:
		return g_strdup ("nmm:metering-mode-spot");
	case EXIF_METERING_MODE_MULTISPOT:
		return g_strdup ("nmm:metering-mode-multispot");
	case EXIF_METERING_MODE_PATTERN:
		return g_strdup ("nmm:metering-mode-pattern");
	case EXIF_METERING_MODE_PARTIAL:
		return g_strdup ("nmm:metering-mode-partial");
	case EXIF_METERING_MODE_UNKNOWN:
	case EXIF_METERING_MODE_OTHER:
	default:
		return g_strdup ("nmm:metering-mode-other");
	}
}

static gchar *
convert_exif_white_balance_to_nmm (gulong white_balance)
{
	if (white_balance == 0)
		return g_strdup ("nmm:white-balance-auto");
	else
		return g_strdup ("nmm:white-balance-manual");
}


/**
 * tracker_exif_read:
 * @buffer: a chunk of data with exif data in it.
 * @len: the size of @buffer.
 * @uri: the URI this is related to.
 * @data: a pointer to a TrackerExifData struture to populate.
 *
 * This function takes @len bytes of @buffer and runs it through the
 * EXIF library. The result is that @data is populated with the EXIF
 * data found in @uri.
 *
 * Returns: %TRUE if the @data was populated successfully, otherwise
 * %FALSE is returned.
 *
 * Since: 0.8
 *
 * Deprecated: 0.9. Use tracker_exif_new() instead.
 **/

TrackerExifData *
tracker_exif_new_from_metadata (GExiv2Metadata *metadata)
{
	TrackerExifData *data;
	g_autofree char *tmp = NULL;
	gchar buf[G_ASCII_DTOSTR_BUF_SIZE];
	double fnumber, lat, lon, alt, focal_length, tmp_gps;
	int nom, den;
	glong tmp_long;

	g_return_val_if_fail (metadata != NULL, NULL);

	data = g_new0 (TrackerExifData, 1);

	data->make = gexiv2_metadata_get_tag_string (metadata, "Exif.Image.Make", NULL);
	data->model = gexiv2_metadata_get_tag_string (metadata, "Exif.Image.Model", NULL);
	data->document_name = gexiv2_metadata_get_tag_string (metadata, "Exif.Image.DocumentName", NULL);
	data->copyright = gexiv2_metadata_get_tag_string (metadata, "Exif.Image.Copyright", NULL);
	data->artist = gexiv2_metadata_get_tag_string (metadata, "Exif.Image.Artist", NULL);
	data->iso_speed_ratings = gexiv2_metadata_get_tag_string (metadata, "Exif.Photo.ISOSpeedRatings", NULL);
	data->description = gexiv2_metadata_get_tag_string (metadata, "Exif.Image.ImageDescription", NULL);
	data->software = gexiv2_metadata_get_tag_string (metadata, "Exif.Image.Software", NULL);

	fnumber = gexiv2_metadata_get_fnumber (metadata, NULL);
	if (fnumber >= 0)
		data->fnumber = g_strdup (g_ascii_dtostr (buf, sizeof (buf), fnumber));

	data->resolution_unit = gexiv2_metadata_get_tag_long (metadata, "Exif.Image.ResolutionUnit", NULL);

	tmp_long = gexiv2_metadata_get_orientation (metadata, NULL);
	if (tmp_long)
		data->orientation = convert_exif_orientation_to_nfo (tmp_long);

	tmp_long = gexiv2_metadata_get_tag_long (metadata, "Exif.Photo.MeteringMode", NULL);
	if (tmp_long != G_MAXLONG)
		data->metering_mode = convert_exif_metering_mode_to_nmm (tmp_long);

	tmp_long = gexiv2_metadata_get_tag_long (metadata, "Exif.Photo.WhiteBalance", NULL);
	if (tmp_long != G_MAXLONG)
		data->white_balance = convert_exif_white_balance_to_nmm (tmp_long);

	tmp_long = gexiv2_metadata_get_tag_long (metadata, "Exif.Photo.Flash", NULL);
	if (tmp_long != G_MAXLONG)
		data->flash = convert_exif_flash_to_nmm (tmp_long);

	tmp = gexiv2_metadata_get_tag_string (metadata, "Exif.Image.DateTimeOriginal", NULL);
	if (tmp) {
		data->time_original = tracker_date_format_to_iso8601 (tmp, EXIF_DATE_FORMAT);
		g_clear_pointer (&tmp, g_free);
	}

	if (!data->time_original) {
		tmp = gexiv2_metadata_get_tag_string (metadata, "Exif.Photo.DateTimeOriginal", NULL);
		if (tmp) {
			data->time_original = tracker_date_format_to_iso8601 (tmp, EXIF_DATE_FORMAT);
			g_clear_pointer (&tmp, g_free);
		}
	}

	tmp = gexiv2_metadata_get_tag_string (metadata, "Exif.Photo.DateTime", NULL);
	if (tmp) {
		data->date = tracker_date_format_to_iso8601 (tmp, EXIF_DATE_FORMAT);
		g_clear_pointer (&tmp, g_free);
	}

	tmp_long = gexiv2_metadata_get_tag_long (metadata, "Exif.Image.XResolution", NULL);
	if (tmp_long > 0)
		data->x_resolution = g_strdup_printf ("%ld", tmp_long);

	tmp_long = gexiv2_metadata_get_tag_long (metadata, "Exif.Image.YResolution", NULL);
	if (tmp_long > 0)
		data->y_resolution = g_strdup_printf ("%ld", tmp_long);

	focal_length = gexiv2_metadata_get_focal_length (metadata, NULL);
	if (focal_length >= 0) {
		gchar buf[G_ASCII_DTOSTR_BUF_SIZE];
		data->focal_length = g_strdup (g_ascii_dtostr (buf, sizeof (buf), focal_length));
	}

	tmp = gexiv2_metadata_get_tag_string (metadata, "Exif.Photo.UserComment", NULL);
	if (tmp) {
		/* UserComment may contain unwanted prefix, e.g. "charset=Ascii " */
		if (tmp && g_str_has_prefix (tmp, "charset=")) {
			char *stripped;

			/* Move past the prefix inline */
			stripped = strchr (tmp, ' ');
			if (stripped)
				stripped++;
			if (*stripped)
				data->user_comment = g_strdup (stripped);
		} else {
			data->user_comment = g_strdup (tmp);
		}

		g_clear_pointer (&tmp, g_free);
	}

	if (gexiv2_metadata_get_exposure_time (metadata, &nom, &den, NULL))
		data->exposure_time = g_strdup (g_ascii_dtostr (buf, sizeof (buf), (double) nom / den));

	lat = gexiv2_metadata_get_gps_latitude (metadata, NULL);
	if (!isnan (lat) && !isinf (lat))
		data->gps_latitude = g_strdup (g_ascii_dtostr (buf, sizeof (buf), lat));

	lon = gexiv2_metadata_get_gps_longitude (metadata, NULL);
	if (!isnan (lon) && !isinf (lon))
		data->gps_longitude = g_strdup (g_ascii_dtostr (buf, sizeof (buf), lon));

	alt = gexiv2_metadata_get_gps_altitude (metadata, NULL);
	if (!isnan (alt) && !isinf (alt))
		data->gps_altitude = g_strdup (g_ascii_dtostr (buf, sizeof (buf), alt));

	tmp = gexiv2_metadata_get_tag_string (metadata, "Exif.GPSInfo.GPSImgDirection", NULL);
	if (tmp) {
		if (sscanf (tmp, "%d/%d", &nom, &den)) {
			gchar buf[G_ASCII_DTOSTR_BUF_SIZE];
			data->gps_direction = g_strdup (g_ascii_dtostr (buf, sizeof (buf), (double) nom / den));
		}
		g_clear_pointer (&tmp, g_free);
	}

	return data;
}

TrackerExifData *
tracker_exif_new (const guchar *buffer,
                  size_t        len,
                  const gchar  *uri)
{
        TrackerExifData *data = NULL;
        GExiv2Metadata *metadata;

        g_return_val_if_fail (buffer != NULL, NULL);
        g_return_val_if_fail (len > 0, NULL);
        g_return_val_if_fail (uri != NULL, NULL);

        metadata = gexiv2_metadata_new ();

        if (gexiv2_metadata_open_buf (metadata, buffer, len, NULL))
                data = tracker_exif_new_from_metadata (metadata);

        g_clear_object (&metadata);

        return data;
}

/**
 * tracker_exif_new:
 * @buffer: a chunk of data with exif data in it.
 * @len: the size of @buffer.
 * @uri: the URI this is related to.
 *
 * This function takes @len bytes of @buffer and runs it through the
 * EXIF library.
 *
 * Returns: a newly allocated #TrackerExifData struct if EXIF data was
 * found, %NULL otherwise. Free the returned struct with tracker_exif_free().
 *
 * Since: 0.10
 **/

void
tracker_exif_apply_to_resource (TrackerResource *resource,
                                TrackerExifData *exif)
{
	if (!exif)
		return;

	if (exif->document_name)
		tracker_resource_set_string (resource, "nie:title", exif->document_name);

	if (exif->time_original || exif->date) {
		const gchar *str = tracker_coalesce_strip (2, exif->date, exif->time_original);
		tracker_resource_set_string (resource, "nie:contentCreated", str);
	}

	if (exif->orientation)
		tracker_resource_set_uri (resource, "nfo:orientation", exif->orientation);

	if (exif->make || exif->model) {
		TrackerResource *equipment = tracker_extract_new_equipment (exif->make, exif->model);
		tracker_resource_set_relation (resource, "nfo:equipment", equipment);
		g_object_unref (equipment);
	}

	if (exif->artist) {
		TrackerResource *artist = tracker_extract_new_contact (exif->artist);
		tracker_resource_set_relation (resource, "nco:creator", artist);
		g_object_unref (artist);
	}

	if (exif->description)
		tracker_resource_set_string (resource, "nie:description", exif->description);

	if (exif->user_comment)
		tracker_resource_set_string (resource, "nie:comment", exif->user_comment);

	if (exif->copyright)
		tracker_resource_set_string (resource, "nie:copyright", exif->copyright);

	if (exif->fnumber) {
		gdouble value;
		value = g_strtod (exif->fnumber, NULL);
		tracker_resource_set_double (resource, "nmm:fnumber", value);
	}

	if (exif->flash)
		tracker_resource_set_uri (resource, "nmm:flash", exif->flash);

	if (exif->focal_length) {
		gdouble value;
		value = g_strtod (exif->focal_length, NULL);
		tracker_resource_set_double (resource, "nmm:focalLength", value);
	}

	if (exif->iso_speed_ratings) {
		gdouble value;
		value = g_strtod (exif->iso_speed_ratings, NULL);
		tracker_resource_set_double (resource, "nmm:isoSpeed", value);
	}

	if (exif->exposure_time) {
		gdouble value;
		value = g_strtod (exif->exposure_time, NULL);
		tracker_resource_set_double (resource, "nmm:exposureTime", value);
	}

	if (exif->metering_mode)
		tracker_resource_set_uri (resource, "nmm:meteringMode", exif->metering_mode);

	if (exif->white_balance)
		tracker_resource_set_uri (resource, "nmm:whiteBalance", exif->white_balance);

	if (exif->x_resolution > 0) {
		gdouble value;

		value = g_strtod (exif->x_resolution, NULL);

		if (exif->resolution_unit == EXIF_RESOLUTION_UNIT_PER_CENTIMETER)
			value *= CMS_PER_INCH;

		tracker_resource_set_double (resource, "nfo:horizontalResolution", value);
	}

	if (exif->y_resolution) {
		gdouble value;

		value = g_strtod (exif->y_resolution, NULL);

		if (exif->resolution_unit == EXIF_RESOLUTION_UNIT_PER_CENTIMETER)
			value *= CMS_PER_INCH;

		tracker_resource_set_double (resource, "nfo:verticalResolution", value);
	}

	if (exif->gps_latitude || exif->gps_longitude || exif->gps_altitude) {
		TrackerResource *geopoint;

		geopoint = tracker_resource_get_first_relation (resource, "slo:location");

		if (geopoint) {
			tracker_extract_merge_location (geopoint,
			                                NULL, NULL,
			                                NULL, NULL,
			                                exif->gps_altitude,
			                                exif->gps_latitude,
			                                exif->gps_longitude);
		} else {
			geopoint = tracker_extract_new_location (NULL, NULL,
			                                         NULL, NULL,
			                                         exif->gps_altitude,
			                                         exif->gps_latitude,
			                                         exif->gps_longitude);
			tracker_resource_set_take_relation (resource, "slo:location", geopoint);
		}
	}

	if (exif->gps_direction) {
		gdouble value;
		value = g_strtod (exif->gps_direction, NULL);
		tracker_resource_set_double (resource, "nfo:heading", value);
	}
}

/**
 * tracker_exif_free:
 * @data: a #TrackerExifData
 *
 * Frees @data and all #TrackerExifData members. %NULL will produce a
 * a warning.
 *
 * Since: 0.10
 **/
void
tracker_exif_free (TrackerExifData *data)
{
	g_return_if_fail (data != NULL);

	g_free (data->y_dimension);
	g_free (data->x_dimension);
	g_free (data->image_width);
	g_free (data->document_name);
	g_free (data->time);
	g_free (data->time_original);
	g_free (data->artist);
	g_free (data->user_comment);
	g_free (data->description);
	g_free (data->make);
	g_free (data->model);
	g_free (data->orientation);
	g_free (data->exposure_time);
	g_free (data->flash);
	g_free (data->focal_length);
	g_free (data->iso_speed_ratings);
	g_free (data->metering_mode);
	g_free (data->white_balance);
	g_free (data->copyright);
	g_free (data->software);
	g_free (data->x_resolution);
	g_free (data->y_resolution);
	g_free (data->gps_altitude);
	g_free (data->gps_latitude);
	g_free (data->gps_longitude);
	g_free (data->gps_direction);
	g_free (data->date);

	g_free (data);
}
