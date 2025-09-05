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

#include <string.h>

#include "tracker-gexiv-compat.h"
#include "tracker-iptc.h"
#include "tracker-resource-helpers.h"
#include "tracker-utils.h"

#include <gexiv2/gexiv2.h>

#define IPTC_DATE_FORMAT "%Y-%m-%d"

/**
 * SECTION:tracker-iptc
 * @title: IPTC
 * @short_description: Information Interchange Model (IIM) /
 * International Press Telecommunications Council (IPTC)
 * @stability: Stable
 * @include: libtracker-extract/tracker-extract.h
 *
 * The Information Interchange Model (IIM) is a file structure and set
 * of metadata attributes that can be applied to text, images and
 * other media types. It was developed in the early 1990s by the
 * International Press Telecommunications Council (IPTC) to expedite
 * the international exchange of news among newspapers and news
 * agencies.
 *
 * The full IIM specification includes a complex data structure and a
 * set of metadata definitions.
 *
 * Although IIM was intended for use with all types of news items —
 * including simple text articles — a subset found broad worldwide
 * acceptance as the standard embedded metadata used by news and
 * commercial photographers. Information such as the name of the
 * photographer, copyright information and the caption or other
 * description can be embedded either manually or automatically.
 *
 * IIM metadata embedded in images are often referred to as "IPTC
 * headers," and can be easily encoded and decoded by most popular
 * photo editing software.
 *
 * The Extensible Metadata Platform (XMP) has largely superseded IIM's
 * file structure, but the IIM image attributes are defined in the
 * IPTC Core schema for XMP and most image manipulation programs keep
 * the XMP and non-XMP IPTC attributes synchronized.
 *
 * This API is provided to remove code duplication between extractors
 * using these standards.
 **/

static const gchar *
fix_iptc_orientation (const gchar *orientation)
{
	if (g_strcmp0 (orientation, "P") == 0) {
		return "nfo:orientation-left";
	}

	return "nfo:orientation-top"; /* We take this as default */
}

TrackerIptcData *
tracker_iptc_new_from_metadata (GExiv2Metadata *metadata)
{
	TrackerIptcData *data;
	GError *error = NULL;

	g_return_val_if_fail (metadata != NULL, NULL);

	data = g_new0 (TrackerIptcData, 1);

	data->keywords = gexiv2_metadata_get_tag_string (metadata, "Iptc.Application2.Keywords", NULL);
	data->byline = gexiv2_metadata_get_tag_string (metadata, "Iptc.Application2.Byline", NULL);
	data->credit = gexiv2_metadata_get_tag_string (metadata, "Iptc.Application2.Credit", NULL);
	data->copyright_notice = gexiv2_metadata_get_tag_string (metadata, "Iptc.Application2.Copyright", NULL);
	data->byline_title = gexiv2_metadata_get_tag_string (metadata, "Iptc.Application2.BylineTitle", NULL);
	data->city = gexiv2_metadata_get_tag_string (metadata, "Iptc.Application2.City", NULL);
	data->state = gexiv2_metadata_get_tag_string (metadata, "Iptc.Application2.ProvinceState", NULL);
	data->sublocation = gexiv2_metadata_get_tag_string (metadata, "Iptc.Application2.SubLocation", NULL);
	data->country_name = gexiv2_metadata_get_tag_string (metadata, "Iptc.Application2.CountryName", NULL);
	data->contact = gexiv2_metadata_get_tag_string (metadata, "Iptc.Application2.Contact", NULL);

	char *date_created = gexiv2_metadata_get_tag_string (metadata, "Iptc.Application2.DateCreated", NULL);
	if (date_created) {
		data->date_created = tracker_date_format_to_iso8601 (date_created, IPTC_DATE_FORMAT);
		g_free (date_created);
	}

	gchar *img_orientation = gexiv2_metadata_get_tag_string (metadata, "Iptc.Application2.ImageOrientation", NULL);
	if (img_orientation) {
		data->image_orientation = g_strdup (fix_iptc_orientation (img_orientation));
		g_free (img_orientation);
	}

	if (error) { g_error_free (error); error = NULL; }

	if (!(data->keywords || data->byline || data->copyright_notice || data->city)) {
		tracker_iptc_free (data);
		return NULL;
	}

	return data;
}

/**
 * tracker_iptc_new:
 * @buffer: a chunk of data with iptc data in it.
 * @len: the size of @buffer.
 * @uri: the URI this is related to.
 *
 * This function takes @len bytes of @buffer and runs it through the
 * IPTC library.
 *
 * Returns: a newly allocated #TrackerIptcData struct if IPTC data was
 * found, %NULL otherwise. Free the returned struct with
 * tracker_iptc_free().
 *
 * Since: 0.10
 **/
TrackerIptcData *
tracker_iptc_new (const guchar *buffer,
                  gsize         len,
                  const gchar  *uri)
{
	GError *error = NULL;
	TrackerIptcData *data = NULL;
	GExiv2Metadata *metadata = NULL;

	g_return_val_if_fail (buffer != NULL, NULL);
	g_return_val_if_fail (len > 0, NULL);
	g_return_val_if_fail (uri != NULL, NULL);

	metadata = gexiv2_metadata_new ();
	if (!gexiv2_metadata_open_buf (metadata, buffer, len, &error)) {
		g_clear_object (&metadata);
		g_propagate_error (NULL, error);
		return NULL;
	}

	data = tracker_iptc_new_from_metadata (metadata);

	g_clear_object (&metadata);
	return data;
}

/**
 * tracker_iptc_free:
 * @data: a #TrackerIptcData
 *
 * Frees @data and all #TrackerIptcData members. %NULL will produce a
 * a warning.
 *
 * Since: 0.10
 **/
void
tracker_iptc_free (TrackerIptcData *data)
{
	g_return_if_fail (data != NULL);

	g_free (data->keywords);
	g_free (data->date_created);
	g_free (data->byline);
	g_free (data->credit);
	g_free (data->copyright_notice);
	g_free (data->image_orientation);
	g_free (data->byline_title);
	g_free (data->city);
	g_free (data->state);
	g_free (data->sublocation);
	g_free (data->country_name);
	g_free (data->contact);

	g_free (data);
}

void
tracker_iptc_apply_to_resource (TrackerResource *resource,
                                TrackerIptcData *iptc)
{
	if (!iptc)
		return;

	if (iptc->keywords)
		tracker_resource_set_string (resource, "nao:keywords", iptc->keywords);

	if (iptc->date_created)
		tracker_resource_set_string (resource, "nie:contentCreated", iptc->date_created);

	if (iptc->byline)
		tracker_resource_set_string (resource, "nco:creator", iptc->byline);

	if (iptc->credit)
		tracker_resource_set_string (resource, "nfo:credit", iptc->credit);

	if (iptc->copyright_notice)
		tracker_resource_set_string (resource, "nie:copyright", iptc->copyright_notice);

	if (iptc->image_orientation)
		tracker_resource_set_string (resource, "nfo:orientation", iptc->image_orientation);

	if (iptc->byline_title)
		tracker_resource_set_string (resource, "nfo:bylineTitle", iptc->byline_title);

	if (iptc->city)
		tracker_resource_set_string (resource, "nco:locality", iptc->city);

	if (iptc->state)
		tracker_resource_set_string (resource, "nco:region", iptc->state);

	if (iptc->sublocation)
		tracker_resource_set_string (resource, "nco:sublocation", iptc->sublocation);

	if (iptc->country_name)
		tracker_resource_set_string (resource, "nco:country", iptc->country_name);

	if (iptc->contact)
		tracker_resource_set_string (resource, "nco:contact", iptc->contact);
}
