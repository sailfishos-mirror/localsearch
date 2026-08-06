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

#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <gio/gio.h>

#include <libxml/parser.h>
#include <libxml/SAX2.h>

#include <tracker-common.h>

#include "utils/tracker-extract.h"

#include "tracker-main.h"
#include "tracker-zip-input-stream.h"

typedef enum {
	ODT_TAG_TYPE_UNKNOWN,
	ODT_TAG_TYPE_TITLE,
	ODT_TAG_TYPE_SUBJECT,
	ODT_TAG_TYPE_AUTHOR,
	ODT_TAG_TYPE_KEYWORDS,
	ODT_TAG_TYPE_COMMENTS,
	ODT_TAG_TYPE_STATS,
	ODT_TAG_TYPE_CREATED,
	ODT_TAG_TYPE_GENERATOR,
	ODT_TAG_TYPE_WORD_TEXT,
	ODT_TAG_TYPE_WORD_TABLE_CELL,
	ODT_TAG_TYPE_SLIDE_TEXT,
	ODT_TAG_TYPE_SPREADSHEET_TEXT,
	ODT_TAG_TYPE_GRAPHICS_TEXT
} ODTTagType;

typedef enum {
	FILE_TYPE_INVALID,
	FILE_TYPE_ODP,
	FILE_TYPE_ODT,
	FILE_TYPE_ODS,
	FILE_TYPE_ODG
} ODTFileType;

typedef struct {
	TrackerResource *metadata;
	GQueue *tag_stack;            /* (element-type: ODTTagType) */
	const gchar *uri;
	GString *text_buf;
} ODTMetadataParseInfo;

typedef struct {
	GQueue *tag_stack;            /* (element-type: ODTTagType) */
	ODTFileType file_type;
	GString *content;
	gulong bytes_pending;
	gboolean limit_reached;
} ODTContentParseInfo;

static GQuark maximum_size_error_quark = 0;

static void oasis_metadata_start_element_ns    (void           *ctx,
                                                const xmlChar  *localname,
                                                const xmlChar  *prefix,
                                                const xmlChar  *URI,
                                                int             nb_namespaces,
                                                const xmlChar **namespaces,
                                                int             nb_attributes,
                                                int             nb_defaulted,
                                                const xmlChar **attributes);
static void oasis_metadata_end_element_ns      (void          *ctx,
                                                const xmlChar *localname,
                                                const xmlChar *prefix,
                                                const xmlChar *URI);
static void oasis_metadata_characters	       (void          *ctx,
                                                const xmlChar *ch,
                                                int            len);
static void oasis_content_start_element_ns     (void           *ctx,
                                            	const xmlChar  *localname,
                                            	const xmlChar  *prefix,
                                            	const xmlChar  *URI,
                                            	int             nb_namespaces,
                                            	const xmlChar **namespaces,
                                            	int             nb_attributes,
                                            	int             nb_defaulted,
                                             	const xmlChar **attributes);
static void oasis_content_end_element_ns       (void          *ctx,
                                            	const xmlChar *localname,
                                             	const xmlChar *prefix,
                                             	const xmlChar *URI);
static void oasis_content_characters           (void          *ctx,
                                             	const xmlChar *ch,
                                            	int            len);
static void extract_oasis_content              (const gchar     *uri,
                                                gulong           total_bytes,
                                                ODTFileType      file_type,
                                                TrackerResource *metadata);

G_MODULE_EXPORT gboolean
tracker_extract_module_init (GError **error)
{
	xmlInitParser ();
	return TRUE;
}

#define ZIP_XML_BUFFER_SIZE 8192

static gboolean
parse_xml_from_zip_sax (const gchar          *zip_uri,
                        const gchar          *member_name,
                        const xmlSAXHandler  *sax,
                        gpointer              user_data,
                        GError              **error)
{
	g_autoptr (GError) inner_error = NULL;
	g_autoptr (GInputStream) stream = NULL;
	g_autoptr (GBytes) bytes = NULL;
	xmlParserCtxtPtr ctxt;
	const guint8 *data;
	gsize len;
	gboolean ok = TRUE;

	stream = tracker_zip_read_file (zip_uri, member_name, NULL, error);
	if (!stream)
		return FALSE;

	ctxt = xmlCreatePushParserCtxt ((xmlSAXHandler *) sax, user_data,
	                                NULL, 0, member_name);
	g_assert (ctxt != NULL);

	ctxt->options |= XML_PARSE_NONET;

	while ((bytes = g_input_stream_read_bytes (stream,
	                                           ZIP_XML_BUFFER_SIZE,
	                                           NULL,
	                                           &inner_error)) != NULL) {
		len = g_bytes_get_size (bytes);

		if (len == 0) {
			break; /* EOF */
		}

		data = g_bytes_get_data (bytes, NULL);

		if (xmlParseChunk (ctxt, (const char *) data, (int) len, 0) != 0) {
			ok = FALSE;
			bytes = NULL;
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
			             "XML error in %s (line %d): %s",
			             member_name,
			             ctxt->lastError.line,
			             ctxt->lastError.message);
		} else {
			g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			             "XML error in %s", member_name);
		}
	}

	xmlFreeParserCtxt (ctxt);

	g_input_stream_close (stream, NULL, NULL);

	return ok;
}

static void
extract_oasis_content (const gchar     *uri,
                       gulong           total_bytes,
                       ODTFileType      file_type,
                       TrackerResource *metadata)
{
	gchar *content = NULL;
	ODTContentParseInfo info;
	GError *error = NULL;
	xmlSAXHandler sax = {
		.initialized = XML_SAX2_MAGIC,
		.startElementNs = oasis_content_start_element_ns,
		.endElementNs = oasis_content_end_element_ns,
		.characters = oasis_content_characters,
	};

	/* If no content requested, return */
	if (total_bytes == 0) {
		return;
	}

	/* Create parse info */
	info.tag_stack = g_queue_new ();
	info.file_type = file_type;
	info.content = g_string_new ("");
	info.bytes_pending = total_bytes;
	info.limit_reached = FALSE;

	parse_xml_from_zip_sax (uri, "content.xml", &sax, &info, &error);

	if (info.limit_reached) {
		g_clear_error (&error);
		g_set_error_literal (&error, maximum_size_error_quark, 0,
		                     "Maximum text limit reached");
	}

	if (!error || g_error_matches (error, maximum_size_error_quark, 0)) {
		content = g_string_free (info.content, FALSE);
		tracker_resource_set_string (metadata, "nie:plainTextContent", content);
	} else {
		g_warning ("Got error parsing XML file: %s\n", error->message);
		g_string_free (info.content, TRUE);
	}

	if (error) {
		g_error_free (error);
	}

	g_free (content);
	g_queue_free (info.tag_stack);
}

G_MODULE_EXPORT gboolean
tracker_extract_get_metadata (TrackerExtractInfo  *extract_info,
                              GError             **error)
{
	TrackerResource *metadata;
	ODTMetadataParseInfo info = { 0 };
	ODTFileType file_type;
	GFile *file;
	gchar *uri, *resource_uri;
	const gchar *mime_used;
	xmlSAXHandler sax = {
		.initialized = XML_SAX2_MAGIC,
		.startElementNs = oasis_metadata_start_element_ns,
		.endElementNs = oasis_metadata_end_element_ns,
		.characters = oasis_metadata_characters,
	};

	if (G_UNLIKELY (maximum_size_error_quark == 0)) {
		maximum_size_error_quark = g_quark_from_static_string ("maximum_size_error");
	}

	file = tracker_extract_info_get_file (extract_info);

	resource_uri = tracker_extract_info_get_content_id (extract_info, NULL);
	metadata = tracker_resource_new (resource_uri);
	mime_used = tracker_extract_info_get_mimetype (extract_info);
	g_free (resource_uri);

	uri = g_file_get_uri (file);

	g_debug ("Extracting OASIS metadata and contents from '%s'", uri);

	/* First, parse metadata */

	tracker_resource_add_uri (metadata, "rdf:type", "nfo:PaginatedTextDocument");

	/* Create parse info */
	info.metadata = metadata;
	info.tag_stack = g_queue_new ();
	info.uri = uri;
	info.text_buf = g_string_new ("");

	parse_xml_from_zip_sax (uri, "meta.xml", &sax, &info, NULL);

	if (g_ascii_strcasecmp (mime_used, "application/vnd.oasis.opendocument.text") == 0) {
		file_type = FILE_TYPE_ODT;
	} else if (g_ascii_strcasecmp (mime_used, "application/vnd.oasis.opendocument.presentation") == 0) {
		file_type = FILE_TYPE_ODP;
	} else if (g_ascii_strcasecmp (mime_used, "application/vnd.oasis.opendocument.spreadsheet") == 0) {
		file_type = FILE_TYPE_ODS;
	} else if (g_ascii_strcasecmp (mime_used, "application/vnd.oasis.opendocument.graphics") == 0) {
		file_type = FILE_TYPE_ODG;
	} else {
		g_debug ("Mime type was not recognised:'%s'", mime_used);
		file_type = FILE_TYPE_INVALID;
	}

	/* Extract content with the given limitations */
	extract_oasis_content (uri,
	                       tracker_extract_info_get_max_text (extract_info),
	                       file_type,
	                       metadata);

	if (info.text_buf)
 		g_string_free (info.text_buf, TRUE);

	g_queue_free (info.tag_stack);

	g_free (uri);

	tracker_extract_info_set_resource (extract_info, metadata);
	g_object_unref (metadata);

	return TRUE;
}

/* ------------------------- SAX helpers ----------------------------------- */

static inline gchar *
make_qname (const xmlChar *localname,
            const xmlChar *prefix)
{
	if (prefix && *prefix)
		return g_strdup_printf ("%s:%s", (const gchar *) prefix, (const gchar *) localname);

	return g_strdup ((const gchar *) localname);
}

/* ------------------------- METADATA (meta.xml) SAX ----------------------------------- */

static void
oasis_metadata_start_element_ns (void           *ctx,
                                 const xmlChar  *localname,
                                 const xmlChar  *prefix,
                                 const xmlChar  *URI G_GNUC_UNUSED,
                                 int             nb_namespaces G_GNUC_UNUSED,
                                 const xmlChar **namespaces G_GNUC_UNUSED,
                                 int             nb_attributes,
                                 int             nb_defaulted G_GNUC_UNUSED,
                                 const xmlChar **attributes)
{
	ODTMetadataParseInfo *data = ctx;
	g_autofree gchar *qname = make_qname (localname, prefix);

	if (g_ascii_strcasecmp (qname, "dc:title") == 0) {
		g_queue_push_head (data->tag_stack, GINT_TO_POINTER (ODT_TAG_TYPE_TITLE));
	} else if (g_ascii_strcasecmp (qname, "dc:subject") == 0) {
		g_queue_push_head (data->tag_stack, GINT_TO_POINTER (ODT_TAG_TYPE_SUBJECT));
	} else if (g_ascii_strcasecmp (qname, "dc:creator") == 0) {
		g_queue_push_head (data->tag_stack, GINT_TO_POINTER (ODT_TAG_TYPE_AUTHOR));
	} else if (g_ascii_strcasecmp (qname, "meta:keyword") == 0) {
		g_queue_push_head (data->tag_stack, GINT_TO_POINTER (ODT_TAG_TYPE_KEYWORDS));
	} else if (g_ascii_strcasecmp (qname, "dc:description") == 0) {
		g_queue_push_head (data->tag_stack, GINT_TO_POINTER (ODT_TAG_TYPE_COMMENTS));
	} else if (g_ascii_strcasecmp (qname, "meta:creation-date") == 0) {
		g_queue_push_head (data->tag_stack, GINT_TO_POINTER (ODT_TAG_TYPE_CREATED));
	} else if (g_ascii_strcasecmp (qname, "meta:generator") == 0) {
		g_queue_push_head (data->tag_stack, GINT_TO_POINTER (ODT_TAG_TYPE_GENERATOR));
	} else if (g_ascii_strcasecmp (qname, "meta:document-statistic") == 0) {
		/* Parse attributes like meta:word-count and meta:page-count */
		for (int i = 0; i < nb_attributes; i++) {
			const xmlChar *a_local  = attributes[i*5 + 0];
			const xmlChar *a_prefix = attributes[i*5 + 1];
			const xmlChar *v_start  = attributes[i*5 + 3];
			const xmlChar *v_end    = attributes[i*5 + 4];
			int v_len = (int) (v_end - v_start);
			g_autofree gchar *attr = make_qname (a_local, a_prefix);
			g_autofree gchar *val  = g_strndup ((const gchar *) v_start, v_len);

			if (g_ascii_strcasecmp (attr, "meta:word-count") == 0) {
				tracker_resource_set_string (data->metadata, "nfo:wordCount", val);
			} else if (g_ascii_strcasecmp (attr, "meta:page-count") == 0) {
				tracker_resource_set_string (data->metadata, "nfo:pageCount", val);
			}
		}

		g_queue_push_head (data->tag_stack, GINT_TO_POINTER (ODT_TAG_TYPE_STATS));
	} else {
		g_queue_push_head (data->tag_stack, GINT_TO_POINTER (ODT_TAG_TYPE_UNKNOWN));
	}

	/* Reset buffer for any element; only used when tag != UNKNOWN */
	g_string_set_size (data->text_buf, 0);
}

static void
oasis_metadata_characters (void          *ctx,
                           const xmlChar *ch,
                           int            len)
{
	ODTMetadataParseInfo *data = ctx;
	ODTTagType current;

	if (len <= 0)
		return;

	current = GPOINTER_TO_INT (g_queue_peek_head (data->tag_stack));
	if (current == ODT_TAG_TYPE_UNKNOWN || current == ODT_TAG_TYPE_STATS)
		return;

	g_string_append_len (data->text_buf, (const gchar *) ch, len);
}

static void
oasis_metadata_end_element_ns (void          *ctx,
                               const xmlChar *localname G_GNUC_UNUSED,
                               const xmlChar *prefix G_GNUC_UNUSED,
                               const xmlChar *URI G_GNUC_UNUSED)
{
	ODTMetadataParseInfo *data = ctx;
	ODTTagType current;

	current = GPOINTER_TO_INT (g_queue_peek_head (data->tag_stack));

	/* Flush buffered text on end for relevant tags */
	if (data->text_buf->len > 0) {
		const gchar *text = data->text_buf->str;

		switch (current) {
		case ODT_TAG_TYPE_TITLE:
			tracker_resource_set_string (data->metadata, "nie:title", text);
			break;

		case ODT_TAG_TYPE_SUBJECT:
			tracker_resource_set_string (data->metadata, "nie:subject", text);
			break;

		case ODT_TAG_TYPE_AUTHOR: {
			TrackerResource *publisher = tracker_extract_new_contact (text);
			tracker_resource_set_relation (data->metadata, "nco:publisher", publisher);
			g_object_unref (publisher);
			break;
		}

		case ODT_TAG_TYPE_KEYWORDS: {
			gchar *keywords = g_strdup (text);
			gchar *lasts = NULL;
			gchar *keyw = NULL;

			for (keyw = strtok_r (keywords, ",;", &lasts);
				keyw;
				keyw = strtok_r (NULL, ",;", &lasts)) {
				g_strstrip (keyw);
				if (*keyw)
					tracker_resource_add_string (data->metadata, "nie:keyword", keyw);
			}

			g_free (keywords);
			break;
		}

		case ODT_TAG_TYPE_COMMENTS:
			tracker_resource_set_string (data->metadata, "nie:comment", text);
			break;

		case ODT_TAG_TYPE_CREATED: {
			gchar *date = tracker_date_guess (text);
			if (date) {
				tracker_resource_set_string (data->metadata, "nie:contentCreated", date);
				g_free (date);
			} else {
				g_warning ("Could not parse creation time (%s) in OASIS document '%s'",
				           text, data->uri);
			}
			break;
		}

		case ODT_TAG_TYPE_GENERATOR:
			tracker_resource_set_string (data->metadata, "nie:generator", text);
			break;

		default:
			break;
		}
	}

	/* Pop stack and reset buffer */
	g_queue_pop_head (data->tag_stack);
	g_string_set_size (data->text_buf, 0);
}

/* ------------------------- CONTENT (content.xml) SAX ----------------------------------- */

static void
oasis_content_start_element_ns (void           *ctx,
                                const xmlChar  *localname,
                                const xmlChar  *prefix,
                                const xmlChar  *URI G_GNUC_UNUSED,
                                int             nb_namespaces G_GNUC_UNUSED,
                                const xmlChar **namespaces G_GNUC_UNUSED,
                                int             nb_attributes G_GNUC_UNUSED,
                                int             nb_defaulted G_GNUC_UNUSED,
                                const xmlChar **attributes G_GNUC_UNUSED)
{
	ODTContentParseInfo *data = ctx;
	g_autofree gchar *qname = make_qname (localname, prefix);

	switch (data->file_type) {
	case FILE_TYPE_ODT:
		/* Explicit whitespace-ish elements in ODT */
		if (g_ascii_strcasecmp (qname, "text:s") == 0 ||
		    g_ascii_strcasecmp (qname, "text:tab") == 0 ||
		    g_ascii_strcasecmp (qname, "text:line-break") == 0) {

			if (data->bytes_pending > 0) {
				if (data->content->len > 0) {
					gchar last = data->content->str[data->content->len - 1];
					if (last != ' ' && last != '\n' && last != '\t' && last != '\r') {
						g_string_append_c (data->content, ' ');
						data->bytes_pending--;
					}
				} else {
					g_string_append_c (data->content, ' ');
					data->bytes_pending--;
				}
			} else {
				data->limit_reached = TRUE;
			}

			g_queue_push_head (data->tag_stack, GINT_TO_POINTER (ODT_TAG_TYPE_WORD_TEXT));
			return;
		}

		/* Regular text containers in ODT */
		if (g_ascii_strcasecmp (qname, "text:p") == 0 ||
		    g_ascii_strcasecmp (qname, "text:h") == 0 ||
		    g_ascii_strcasecmp (qname, "text:a") == 0 ||
		    g_ascii_strcasecmp (qname, "text:span") == 0) {
			g_queue_push_head (data->tag_stack, GINT_TO_POINTER (ODT_TAG_TYPE_WORD_TEXT));
			return;
		} else if (g_ascii_strcasecmp (qname, "table:table-cell") == 0) {
			g_queue_push_head (data->tag_stack, GINT_TO_POINTER (ODT_TAG_TYPE_WORD_TABLE_CELL));
			return;
		}

		g_queue_push_head (data->tag_stack, GINT_TO_POINTER (ODT_TAG_TYPE_UNKNOWN));
		return;

	case FILE_TYPE_ODP:
		g_queue_push_head (data->tag_stack, GINT_TO_POINTER (ODT_TAG_TYPE_SLIDE_TEXT));
		return;

	case FILE_TYPE_ODS:
		/* Match any "text:*" */
		if (g_ascii_strncasecmp (qname, "text:", 5) == 0) {
			g_queue_push_head (data->tag_stack, GINT_TO_POINTER (ODT_TAG_TYPE_SPREADSHEET_TEXT));
			return;
		}
		g_queue_push_head (data->tag_stack, GINT_TO_POINTER (ODT_TAG_TYPE_UNKNOWN));
		return;

	case FILE_TYPE_ODG:
		if (g_ascii_strncasecmp (qname, "text:", 5) == 0) {
			g_queue_push_head (data->tag_stack, GINT_TO_POINTER (ODT_TAG_TYPE_GRAPHICS_TEXT));
			return;
		}
		g_queue_push_head (data->tag_stack, GINT_TO_POINTER (ODT_TAG_TYPE_UNKNOWN));
		return;

	case FILE_TYPE_INVALID:
	default:
		g_queue_push_head (data->tag_stack, GINT_TO_POINTER (ODT_TAG_TYPE_UNKNOWN));
		return;
	}
}

static void
oasis_content_characters (void          *ctx,
                          const xmlChar *ch,
                          int            len)
{
	ODTContentParseInfo *data = ctx;
	ODTTagType current;
	gsize written_bytes = 0;

	if (len <= 0)
		return;

	current = GPOINTER_TO_INT (g_queue_peek_head (data->tag_stack));
	switch (current) {
	case ODT_TAG_TYPE_WORD_TEXT:
	case ODT_TAG_TYPE_WORD_TABLE_CELL:
	case ODT_TAG_TYPE_SLIDE_TEXT:
	case ODT_TAG_TYPE_SPREADSHEET_TEXT:
	case ODT_TAG_TYPE_GRAPHICS_TEXT:
		if (data->bytes_pending == 0) {
			data->limit_reached = TRUE;
			return;
		}

		if (tracker_text_validate_utf8 ((const gchar *) ch,
		                                MIN ((gsize) len, (gsize) data->bytes_pending),
		                                &data->content,
		                                &written_bytes)) {
			data->bytes_pending -= written_bytes;
		}

		data->bytes_pending -= written_bytes;
		break;

	default:
		break;
	}
}

static void
oasis_content_end_element_ns (void          *ctx,
                              const xmlChar *localname G_GNUC_UNUSED,
                              const xmlChar *prefix G_GNUC_UNUSED,
                              const xmlChar *URI G_GNUC_UNUSED)
{
	ODTContentParseInfo *data = ctx;
	ODTTagType current = GPOINTER_TO_INT (g_queue_peek_head (data->tag_stack));

	if (current == ODT_TAG_TYPE_WORD_TABLE_CELL ||
	    current == ODT_TAG_TYPE_SPREADSHEET_TEXT ||
	    current == ODT_TAG_TYPE_GRAPHICS_TEXT ||
	    current == ODT_TAG_TYPE_SLIDE_TEXT) {

		if (data->bytes_pending > 0 && data->content->len > 0) {
			gchar last = data->content->str[data->content->len - 1];
			if (last != ' ' && last != '\n' && last != '\t' && last != '\r') {
				g_string_append_c (data->content, ' ');
				data->bytes_pending--;
			}
		} else if (data->bytes_pending == 0) {
			data->limit_reached = TRUE;
		}
	}

	/* Pop current element */
	g_queue_pop_head (data->tag_stack);
}
