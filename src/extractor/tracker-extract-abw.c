/*
 * Copyright (C) 2007, Jamie McCracken <jamiemcc@gnome.org>
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

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/mman.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#include <libxml/parser.h>
#include <libxml/SAX2.h>

#include <tracker-common.h>

#include "utils/tracker-extract.h"

#define BUFFER_SIZE (8 * 1024)

typedef struct AbwParserData AbwParserData;
typedef enum {
	ABW_PARSER_TAG_UNHANDLED,
	ABW_PARSER_TAG_TITLE,
	ABW_PARSER_TAG_SUBJECT,
	ABW_PARSER_TAG_CREATOR,
	ABW_PARSER_TAG_KEYWORDS,
	ABW_PARSER_TAG_DESCRIPTION,
	ABW_PARSER_TAG_GENERATOR
} AbwParserTag;

struct AbwParserData {
	TrackerResource *resource;
	GString *content;
	gchar *uri;

	AbwParserTag cur_tag;
	guint in_text : 1;

	GString *text_buf;
};

G_MODULE_EXPORT gboolean
tracker_extract_module_init (GError **error)
{
	xmlInitParser ();
	return TRUE;
}

static gboolean
parse_xml_from_stream_sax (GInputStream        *stream,
                           const xmlSAXHandler *sax,
                           gpointer             user_data,
                           GError             **error)
{
	g_autoptr (GError) inner_error = NULL;
	g_autoptr (GBytes) bytes = NULL;
	xmlParserCtxtPtr ctxt;
	const guint8 *data;
	gsize len;
	gboolean ok = TRUE;

	ctxt = xmlCreatePushParserCtxt ((xmlSAXHandler *) sax, user_data, NULL, 0, NULL);
	g_assert (ctxt != NULL);

	ctxt->options |= XML_PARSE_NONET;

	while ((bytes = g_input_stream_read_bytes (stream, BUFFER_SIZE, NULL, &inner_error)) != NULL) {
		len = g_bytes_get_size (bytes);
		if (len == 0) {
			break;
		}

		data = g_bytes_get_data (bytes, NULL);

		if (xmlParseChunk (ctxt, (const char *) data, (int) len, 0) != 0) {
			ok = FALSE;
			break;
		}
		g_clear_pointer (&bytes, g_bytes_unref);
	}

	if (inner_error)
		ok = FALSE;

	if (ok) {
		if (xmlParseChunk (ctxt, NULL, 0, 1) != 0)
			ok = FALSE;
	}

	if (!ok) {
		if (inner_error) {
			g_propagate_error (error, inner_error);
		} else if (ctxt->lastError.message) {
			g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			             "XML error in line %d: %s",
			             ctxt->lastError.line,
			             ctxt->lastError.message);
		} else {
			g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			             "XML error");
		}
	}

	xmlFreeParserCtxt (ctxt);
	return ok;
}

static void
abw_sax_start_element_ns (void           *ctx,
                          const xmlChar  *localname,
                          const xmlChar  *prefix G_GNUC_UNUSED,
                          const xmlChar  *URI G_GNUC_UNUSED,
                          int             nb_namespaces G_GNUC_UNUSED,
                          const xmlChar **namespaces G_GNUC_UNUSED,
                          int             nb_attributes,
                          int             nb_defaulted G_GNUC_UNUSED,
                          const xmlChar **attributes)
{
	AbwParserData *data = ctx;

	/* Reset buffer on every start*/
	g_string_set_size (data->text_buf, 0);

	if (g_strcmp0 ((const char *) localname, "m") == 0) {
		/* <m key="dc.title">...</m> */
		for (int i = 0; i < nb_attributes; i++) {
			g_autofree gchar *key = NULL;
			const xmlChar *a_local  = attributes[i*5 + 0];
			const xmlChar *v_start  = attributes[i*5 + 3];
			const xmlChar *v_end    = attributes[i*5 + 4];
			int v_len = (int)(v_end - v_start);

			if (g_strcmp0 ((const char *) a_local, "key") != 0)
				continue;

			key = g_strndup ((const gchar *) v_start, v_len);

			data->cur_tag = ABW_PARSER_TAG_UNHANDLED;

			if (g_strcmp0 (key, "dc.title") == 0) {
				data->cur_tag = ABW_PARSER_TAG_TITLE;
			} else if (g_strcmp0 (key, "dc.subject") == 0) {
				data->cur_tag = ABW_PARSER_TAG_SUBJECT;
			} else if (g_strcmp0 (key, "dc.creator") == 0) {
				data->cur_tag = ABW_PARSER_TAG_CREATOR;
			} else if (g_strcmp0 (key, "abiword.keywords") == 0) {
				data->cur_tag = ABW_PARSER_TAG_KEYWORDS;
			} else if (g_strcmp0 (key, "dc.description") == 0) {
				data->cur_tag = ABW_PARSER_TAG_DESCRIPTION;
			} else if (g_strcmp0 (key, "abiword.generator") == 0) {
				data->cur_tag = ABW_PARSER_TAG_GENERATOR;
			}

			break;
		}
	} else if (g_strcmp0 ((const char *) localname, "section") == 0) {
		data->in_text = TRUE;
	}
}

static void
abw_sax_characters (void          *ctx,
                    const xmlChar *ch,
                    int            len)
{
	AbwParserData *data = ctx;

	if (len <= 0)
		return;

	/* 1) Metadata: accumulate text if we are inside an <m key="..."> element */
	if (data->cur_tag != ABW_PARSER_TAG_UNHANDLED) {
		g_string_append_len (data->text_buf, (const gchar *) ch, len);
	}

	/* 2) Content: if we are inside a text section, accumulate everything as-is */
	if (data->in_text) {
		if (G_UNLIKELY (!data->content))
			data->content = g_string_new ("");

		g_string_append_len (data->content, (const gchar *) ch, len);
	}
}

static void
abw_sax_end_element_ns (void          *ctx,
                        const xmlChar *localname,
                        const xmlChar *prefix G_GNUC_UNUSED,
                        const xmlChar *URI G_GNUC_UNUSED)
{
	AbwParserData *data = ctx;

	if (g_strcmp0 ((const char *) localname, "section") == 0) {
		data->in_text = FALSE;
	} else if (g_strcmp0 ((const char *) localname, "m") == 0) {
		/* Closing an <m> element => process metadata if applicable */
		if (data->cur_tag != ABW_PARSER_TAG_UNHANDLED && data->text_buf->len > 0) {
			const gchar *text = data->text_buf->str;

			switch (data->cur_tag) {
			case ABW_PARSER_TAG_TITLE:
				tracker_resource_set_string (data->resource, "nie:title", text);
				break;

			case ABW_PARSER_TAG_SUBJECT:
				tracker_resource_set_string (data->resource, "nie:subject", text);
				break;

			case ABW_PARSER_TAG_CREATOR: {
				TrackerResource *creator = tracker_extract_new_contact (text);
				tracker_resource_set_relation (data->resource, "nco:creator", creator);
				g_object_unref (creator);
				break;
			}

			case ABW_PARSER_TAG_DESCRIPTION:
				tracker_resource_set_string (data->resource, "nie:comment", text);
				break;

			case ABW_PARSER_TAG_GENERATOR:
				tracker_resource_set_string (data->resource, "nie:generator", text);
				break;

			case ABW_PARSER_TAG_KEYWORDS: {
				gchar *keywords = g_strdup (text);
				gchar *lasts = NULL;
				gchar *keyword = NULL;

				for (keyword = strtok_r (keywords, ",; ", &lasts);
				     keyword;
				     keyword = strtok_r (NULL, ",; ", &lasts)) {
					tracker_resource_add_string (data->resource, "nie:keyword", keyword);
				}

				g_free (keywords);
				break;
			}

			default:
				break;
			}
		}

		data->cur_tag = ABW_PARSER_TAG_UNHANDLED;
		g_string_set_size (data->text_buf, 0);
	}
}

G_MODULE_EXPORT gboolean
tracker_extract_get_metadata (TrackerExtractInfo  *info,
                              GError             **error)
{
	g_autoptr (GInputStream) stream = NULL, buffered_stream = NULL, converter_stream = NULL;
	GInputStream *read_stream;
	g_autoptr (GFile) file = NULL;
	g_autoptr (TrackerResource) resource = NULL;
	g_autoptr (GString) content = NULL;
	g_autofree char *resource_uri = NULL, *uri = NULL, *path = NULL;
	AbwParserData data = { 0 };
	const char *buffer;
	xmlSAXHandler sax = {
		.initialized = XML_SAX2_MAGIC,
		.startElementNs = abw_sax_start_element_ns,
		.endElementNs = abw_sax_end_element_ns,
		.characters = abw_sax_characters,
	};

	file = g_object_ref (tracker_extract_info_get_file (info));

	stream = G_INPUT_STREAM (g_file_read (file, NULL, error));
	if (!stream)
		return FALSE;

	buffered_stream = g_buffered_input_stream_new_sized (stream, BUFFER_SIZE);

	if (g_buffered_input_stream_fill (G_BUFFERED_INPUT_STREAM (buffered_stream),
					  BUFFER_SIZE, NULL, error) == -1)
		return FALSE;

	buffer = g_buffered_input_stream_peek_buffer (G_BUFFERED_INPUT_STREAM (buffered_stream), NULL);
	if (buffer[0] == '<') {
		/* Uncompressed XML */
		read_stream = buffered_stream;
	} else {
		g_autoptr (GZlibDecompressor) decompressor = NULL;

		/* Compressed data(?) */
		decompressor = g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP);
		converter_stream = g_converter_input_stream_new (buffered_stream,
								 G_CONVERTER (decompressor));
		read_stream = converter_stream;
	}

	resource_uri = tracker_extract_info_get_content_id (info, NULL);
	resource = tracker_resource_new (resource_uri);
	uri = g_file_get_uri (file);
	content = g_string_new ("");

	data.uri = uri;
	data.resource = resource;
	data.content = content;

	tracker_resource_add_uri (data.resource, "rdf:type", "nfo:Document");

	data.cur_tag = ABW_PARSER_TAG_UNHANDLED;
	data.in_text = FALSE;
	data.text_buf = g_string_new ("");

	if (!parse_xml_from_stream_sax (read_stream, &sax, &data, error)) {
		g_string_free (data.text_buf, TRUE);
		return FALSE;
	}

	g_string_free (data.text_buf, TRUE);

	if (content->len > 0)
		tracker_resource_set_string (data.resource, "nie:plainTextContent", content->str);

	tracker_extract_info_set_resource (info, resource);

	return TRUE;
}
