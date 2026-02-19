/*
 * Copyright (C) 2008-2010 Nokia <ivan.frade@nokia.com>
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
#include <stdlib.h>

#include <glib.h>
#include <gio/gio.h>

#include <libxml/parser.h>
#include <libxml/SAX2.h>

#include <tracker-common.h>

#include "utils/tracker-extract.h"

#include "tracker-main.h"
#include "tracker-zip-input-stream.h"

typedef enum {
	MS_OFFICE_XML_TAG_INVALID,
	MS_OFFICE_XML_TAG_TITLE,
	MS_OFFICE_XML_TAG_SUBJECT,
	MS_OFFICE_XML_TAG_AUTHOR,
	MS_OFFICE_XML_TAG_MODIFIED,
	MS_OFFICE_XML_TAG_COMMENTS,
	MS_OFFICE_XML_TAG_CREATED,
	MS_OFFICE_XML_TAG_GENERATOR,
	MS_OFFICE_XML_TAG_NUM_OF_PAGES,
	MS_OFFICE_XML_TAG_NUM_OF_CHARACTERS,
	MS_OFFICE_XML_TAG_NUM_OF_WORDS,
	MS_OFFICE_XML_TAG_NUM_OF_LINES,
	MS_OFFICE_XML_TAG_APPLICATION,
	MS_OFFICE_XML_TAG_NUM_OF_PARAGRAPHS,
	MS_OFFICE_XML_TAG_SLIDE_TEXT,
	MS_OFFICE_XML_TAG_WORD_TEXT,
	MS_OFFICE_XML_TAG_XLS_SHARED_TEXT,
	MS_OFFICE_XML_TAG_DOCUMENT_CORE_DATA,
	MS_OFFICE_XML_TAG_DOCUMENT_TEXT_DATA
} MsOfficeXMLTagType;

typedef enum {
	FILE_TYPE_INVALID,
	FILE_TYPE_PPTX,
	FILE_TYPE_PPSX,
	FILE_TYPE_DOCX,
	FILE_TYPE_XLSX
} MsOfficeXMLFileType;

typedef struct {
	/* Common constant stuff */
	const gchar *uri;
	MsOfficeXMLFileType file_type;

	/* Tag type, reused by Content and Metadata parsers */
	MsOfficeXMLTagType tag_type;

	/* Metadata-parsing specific things */
	TrackerResource *metadata;

	/* Content-parsing specific things */
	GString *content;
	gulong bytes_pending;
	gboolean style_element_present;
	gboolean preserve_attribute_present;
	gboolean in_xlsx_shared_strings;
	GList *parts;
	GString *text_buf;
} MsOfficeXMLParserInfo;

static void msoffice_xml_content_start_element_ns 	(void           *ctx,
                                                         const xmlChar  *localname,
                                                         const xmlChar  *prefix,
                                                         const xmlChar  *URI,
                                                         int             nb_namespaces,
                                                         const xmlChar **namespaces,
                                                         int             nb_attributes,
                                                         int             nb_defaulted,
                                                         const xmlChar **attributes);
static void msoffice_xml_content_end_element_ns   	(void *ctx,
                                                         const xmlChar *localname,
                                                         const xmlChar *prefix,
                                                         const xmlChar *URI);
static void msoffice_xml_content_parse			(gpointer      context,
                                                         const gchar  *text,
                                                         gsize         text_len,
                                                         gpointer      user_data,
                                                         GError      **error);
static void msoffice_xml_characters			(void          *ctx,
                                                         const xmlChar *ch,
                                                         int            len);
static void msoffice_xml_metadata_start_element_ns 	(void           *ctx,
                                                         const xmlChar  *localname,
                                                         const xmlChar  *prefix,
                                                         const xmlChar  *URI,
                                                         int             nb_namespaces,
                                                         const xmlChar **namespaces,
                                                         int             nb_attributes,
                                                         int             nb_defaulted,
                                                         const xmlChar **attributes);
static void msoffice_xml_metadata_end_element_ns	(void          *ctx,
                                                         const xmlChar *localname,
                                                         const xmlChar *prefix,
                                                         const xmlChar *URI);
static void msoffice_xml_metadata_parse			(gpointer      context,
                                                         const gchar  *text,
                                                         gsize	       text_len,
                                                         gpointer      user_data,
                                                         GError	     **error);
static void msoffice_xml_content_types_start_element_ns (void           *ctx,
                                                         const xmlChar  *localname,
                                                         const xmlChar  *prefix,
                                                         const xmlChar  *URI,
                                                         int             nb_namespaces,
                                                         const xmlChar **namespaces,
                                                         int             nb_attributes,
                                                         int             nb_defaulted,
                                                         const xmlChar **attributes);
static GQuark maximum_size_error_quark = 0;

/* ------------------------- CONTENT files parsing -----------------------------------*/

static void
msoffice_xml_content_start_element_ns (void           *ctx,
                                       const xmlChar  *localname,
                                       const xmlChar  *prefix,
                                       const xmlChar  *URI G_GNUC_UNUSED,
                                       int             nb_namespaces G_GNUC_UNUSED,
                                       const xmlChar **namespaces G_GNUC_UNUSED,
                                       int             nb_attributes,
                                       int             nb_defaulted G_GNUC_UNUSED,
                                       const xmlChar **attributes)
{
	MsOfficeXMLParserInfo *info = ctx;
	g_autofree gchar *qname = NULL;

	if (prefix && *prefix)
		qname = g_strdup_printf ("%s:%s", (const gchar *) prefix, (const gchar *) localname);
	else
		qname = g_strdup ((const gchar *) localname);

	switch (info->file_type) {
	case FILE_TYPE_DOCX:
		if (g_ascii_strcasecmp (qname, "w:pStyle") == 0) {
			for (int i = 0; i < nb_attributes; i++) {
				const xmlChar *a_local  = attributes[i*5 + 0];
				const xmlChar *a_prefix = attributes[i*5 + 1];
				const xmlChar *v_start  = attributes[i*5 + 3];
				const xmlChar *v_end    = attributes[i*5 + 4];
				int v_len = (int)(v_end - v_start);
				g_autofree gchar *attr = NULL, *val = NULL;

				if (a_prefix && *a_prefix)
					attr = g_strdup_printf ("%s:%s", (const gchar *) a_prefix, (const gchar *) a_local);
				else
					attr = g_strdup ((const gchar *) a_local);

				if (g_ascii_strcasecmp (attr, "w:val") != 0)
					continue;

				val = g_strndup ((const gchar *) v_start, v_len);
				if (g_ascii_strncasecmp (val, "Heading", 7) == 0 ||
				    g_ascii_strncasecmp (val, "TOC", 3) == 0 ||
				    g_ascii_strncasecmp (val, "Section", 7) == 0 ||
				    g_ascii_strncasecmp (val, "Title", 5) == 0 ||
				    g_ascii_strncasecmp (val, "Subtitle", 8) == 0) {
					info->style_element_present = TRUE;
				}
			}
		} else if (g_ascii_strcasecmp (qname, "w:rStyle") == 0) {
			for (int i = 0; i < nb_attributes; i++) {
				const xmlChar *a_local  = attributes[i*5 + 0];
				const xmlChar *a_prefix = attributes[i*5 + 1];
				const xmlChar *v_start  = attributes[i*5 + 3];
				const xmlChar *v_end    = attributes[i*5 + 4];
				int v_len = (int)(v_end - v_start);
				g_autofree gchar *attr = NULL, *val = NULL;

				if (a_prefix && *a_prefix)
					attr = g_strdup_printf ("%s:%s", (const gchar *) a_prefix, (const gchar *) a_local);
				else
					attr = g_strdup ((const gchar *) a_local);

				if (g_ascii_strcasecmp (attr, "w:val") != 0)
					continue;

				val = g_strndup ((const gchar *) v_start, v_len);
				if (g_ascii_strncasecmp (val, "SubtleEmphasis", 14) == 0 ||
				    g_ascii_strncasecmp (val, "SubtleReference", 15) == 0) {
					info->style_element_present = TRUE;
				}
			}
		} else if (g_ascii_strcasecmp (qname, "w:sz") == 0) {
			for (int i = 0; i < nb_attributes; i++) {
				const xmlChar *a_local  = attributes[i*5 + 0];
				const xmlChar *a_prefix = attributes[i*5 + 1];
				const xmlChar *v_start  = attributes[i*5 + 3];
				const xmlChar *v_end    = attributes[i*5 + 4];
				int v_len = (int)(v_end - v_start);
				g_autofree gchar *attr = NULL, *val = NULL;

				if (a_prefix && *a_prefix)
					attr = g_strdup_printf ("%s:%s", (const gchar *) a_prefix, (const gchar *) a_local);
				else
					attr = g_strdup ((const gchar *) a_local);

				if (g_ascii_strcasecmp (attr, "w:val") != 0)
					continue;

				val = g_strndup ((const gchar *) v_start, v_len);
				if (atoi (val) >= 38)
					info->style_element_present = TRUE;
			}
		} else if (g_ascii_strcasecmp (qname, "w:smartTag") == 0 ||
		           g_ascii_strcasecmp (qname, "w:sdtContent") == 0 ||
		           g_ascii_strcasecmp (qname, "w:hyperlink") == 0) {
			info->style_element_present = TRUE;
		} else if (g_ascii_strcasecmp (qname, "w:t") == 0) {
			for (int i = 0; i < nb_attributes; i++) {
				const xmlChar *a_local  = attributes[i*5 + 0];
				const xmlChar *a_prefix = attributes[i*5 + 1];
				const xmlChar *v_start  = attributes[i*5 + 3];
				const xmlChar *v_end    = attributes[i*5 + 4];
				int v_len = (int)(v_end - v_start);
				g_autofree gchar *attr = NULL, *val = NULL;

				if (a_prefix && *a_prefix)
					attr = g_strdup_printf ("%s:%s", (const gchar *) a_prefix, (const gchar *) a_local);
				else
					attr = g_strdup ((const gchar *) a_local);

				if (g_ascii_strcasecmp (attr, "xml:space") != 0)
					continue;

				val = g_strndup ((const gchar *) v_start, v_len);
				if (g_ascii_strncasecmp (val, "preserve", 8) == 0)
					info->preserve_attribute_present = TRUE;
			}

			info->tag_type = MS_OFFICE_XML_TAG_WORD_TEXT;
			g_string_set_size (info->text_buf, 0);
		}
		break;

	case FILE_TYPE_XLSX:
		if (info->in_xlsx_shared_strings &&
			g_ascii_strcasecmp (qname, "t") == 0) {
			info->tag_type = MS_OFFICE_XML_TAG_XLS_SHARED_TEXT;
			g_string_set_size (info->text_buf, 0);
		}
		break;

	case FILE_TYPE_PPTX:
	case FILE_TYPE_PPSX:
		if (g_ascii_strcasecmp (qname, "a:t") == 0 || g_ascii_strcasecmp (qname, "t") == 0) {
			info->tag_type = MS_OFFICE_XML_TAG_SLIDE_TEXT;
			g_string_set_size (info->text_buf, 0);
		}
		break;

	case FILE_TYPE_INVALID:
		break;
	}
}

static void
msoffice_xml_content_end_element_ns (void          *ctx,
                                     const xmlChar *localname,
                                     const xmlChar *prefix,
                                     const xmlChar *URI G_GNUC_UNUSED)
{
	MsOfficeXMLParserInfo *info = ctx;
	g_autofree gchar *qname = NULL;
	/* Flush ONLY when closing the element that started the text capture */
	gboolean should_flush = FALSE;

	if (prefix && *prefix)
		qname = g_strdup_printf ("%s:%s", (const gchar *) prefix, (const gchar *) localname);
	else
		qname = g_strdup ((const gchar *) localname);

	if (info->tag_type == MS_OFFICE_XML_TAG_WORD_TEXT) {
		should_flush = (g_ascii_strcasecmp (qname, "w:t") == 0);
	} else if (info->tag_type == MS_OFFICE_XML_TAG_SLIDE_TEXT) {
		should_flush = (g_ascii_strcasecmp (qname, "a:t") == 0 ||
						g_ascii_strcasecmp (qname, "t") == 0);
	} else if (info->tag_type == MS_OFFICE_XML_TAG_XLS_SHARED_TEXT) {
		should_flush = (g_ascii_strcasecmp (qname, "t") == 0);
	}

	if (should_flush && info->text_buf->len > 0) {
		msoffice_xml_content_parse (NULL,
		                            info->text_buf->str,
		                            info->text_buf->len,
		                            info,
		                            NULL);
	}

	/* Reset capture state after element closes */
	if (should_flush) {
		info->tag_type = MS_OFFICE_XML_TAG_INVALID;
		g_string_set_size (info->text_buf, 0);
	} else if (info->tag_type != MS_OFFICE_XML_TAG_INVALID) {
		/* If we were capturing text, but this wasn't its closing tag, don't reset */
		/* keep tag_type and text_buf */
	} else {
		/* Not capturing: ensure buffer is empty */
		g_string_set_size (info->text_buf, 0);
	}

	if (g_ascii_strcasecmp (qname, "w:p") == 0) {
		info->style_element_present = FALSE;
		info->preserve_attribute_present = FALSE;
	}
}

static void
msoffice_xml_content_parse (gpointer      context G_GNUC_UNUSED,
                            const gchar  *text,
                            gsize         text_len,
                            gpointer      user_data,
                            GError      **error G_GNUC_UNUSED)
{
	MsOfficeXMLParserInfo *info = user_data;
	gsize written_bytes = 0;

	/* Nothing to extract */
	if (text_len == 0 || text == NULL || *text == '\0')
		return;

	/* Ignore if reaching the limit */
	if (info->bytes_pending == 0)
		return;

	/* Create content doesn't exist */
	if (G_UNLIKELY (info->content == NULL)) {
		info->content = g_string_new ("");
	}

	switch (info->tag_type) {
	case MS_OFFICE_XML_TAG_WORD_TEXT:
	case MS_OFFICE_XML_TAG_SLIDE_TEXT:
		tracker_text_validate_utf8 (text,
		                            MIN (text_len, info->bytes_pending),
		                            &info->content,
		                            &written_bytes);
		if (written_bytes > 0) {
			/* Append a separator only if last char is not whitespace */
			if (info->content->len > 0) {
				gchar last = info->content->str[info->content->len - 1];
				if (last != ' ' && last != '\n' && last != '\t' && last != '\r')
					g_string_append_c (info->content, ' ');
			}
			info->bytes_pending -= written_bytes;
		}
		break;

	case MS_OFFICE_XML_TAG_XLS_SHARED_TEXT: {
		gboolean all_digits = TRUE;
		for (gsize k = 0; k < text_len; k++) {
			if (text[k] < '0' || text[k] > '9') {
				all_digits = FALSE;
				break;
			}
		}
		if (all_digits)
			break;

		tracker_text_validate_utf8 (text,
		                            MIN (text_len, info->bytes_pending),
		                            &info->content,
		                            &written_bytes);
		if (written_bytes > 0) {
			if (info->content->len > 0) {
				gchar last = info->content->str[info->content->len - 1];
				if (last != ' ' && last != '\n' && last != '\t' && last != '\r')
					g_string_append_c (info->content, ' ');
			}
			info->bytes_pending -= written_bytes;
		}
		break;
	}

	default:
		break;
	}
}

static void
msoffice_xml_characters (void          *ctx,
                         const xmlChar *ch,
                         int            len)
{
	MsOfficeXMLParserInfo *info = ctx;

	if (len <= 0)
		return;

	if (info->tag_type != MS_OFFICE_XML_TAG_INVALID) {
		if (info->text_buf->len < 16384) {
			g_string_append_len (info->text_buf, (const gchar *) ch, len);
		} else {
			info->tag_type = MS_OFFICE_XML_TAG_INVALID;
			g_string_set_size (info->text_buf, 0);
		}
	}
}

/* ------------------------- METADATA files parsing -----------------------------------*/

static void
msoffice_xml_metadata_start_element_ns (void           *ctx,
                                        const xmlChar  *localname,
                                        const xmlChar  *prefix,
                                        const xmlChar  *URI G_GNUC_UNUSED,
                                        int             nb_namespaces G_GNUC_UNUSED,
                                        const xmlChar **namespaces G_GNUC_UNUSED,
                                        int             nb_attributes G_GNUC_UNUSED,
                                        int             nb_defaulted G_GNUC_UNUSED,
                                        const xmlChar **attributes G_GNUC_UNUSED)
{
	MsOfficeXMLParserInfo *info = ctx;
	g_autofree gchar *qname = NULL;

	if (prefix && *prefix)
		qname = g_strdup_printf ("%s:%s", (const gchar *) prefix, (const gchar *) localname);
	else
		qname = g_strdup ((const gchar *) localname);

	if (g_ascii_strcasecmp (qname, "dc:title") == 0) {
		info->tag_type = MS_OFFICE_XML_TAG_TITLE;
	} else if (g_ascii_strcasecmp (qname, "dc:subject") == 0) {
		info->tag_type = MS_OFFICE_XML_TAG_SUBJECT;
	} else if (g_ascii_strcasecmp (qname, "dc:creator") == 0) {
		info->tag_type = MS_OFFICE_XML_TAG_AUTHOR;
	} else if (g_ascii_strcasecmp (qname, "dc:description") == 0) {
		info->tag_type = MS_OFFICE_XML_TAG_COMMENTS;
	} else if (g_ascii_strcasecmp (qname, "dcterms:created") == 0) {
		info->tag_type = MS_OFFICE_XML_TAG_CREATED;
	} else if (g_ascii_strcasecmp (qname, "meta:generator") == 0) {
		info->tag_type = MS_OFFICE_XML_TAG_GENERATOR;
	} else if (g_ascii_strcasecmp (qname, "dcterms:modified") == 0) {
		info->tag_type = MS_OFFICE_XML_TAG_MODIFIED;
	} else if (g_ascii_strcasecmp (qname, "Pages") == 0 ||
	           g_ascii_strcasecmp (qname, "Slides") == 0) {
		info->tag_type = MS_OFFICE_XML_TAG_NUM_OF_PAGES;
	} else if (g_ascii_strcasecmp (qname, "Paragraphs") == 0) {
		info->tag_type = MS_OFFICE_XML_TAG_NUM_OF_PARAGRAPHS;
	} else if (g_ascii_strcasecmp (qname, "Characters") == 0) {
		info->tag_type = MS_OFFICE_XML_TAG_NUM_OF_CHARACTERS;
	} else if (g_ascii_strcasecmp (qname, "Words") == 0) {
		info->tag_type = MS_OFFICE_XML_TAG_NUM_OF_WORDS;
	} else if (g_ascii_strcasecmp (qname, "Lines") == 0) {
		info->tag_type = MS_OFFICE_XML_TAG_NUM_OF_LINES;
	} else if (g_ascii_strcasecmp (qname, "Application") == 0) {
		info->tag_type = MS_OFFICE_XML_TAG_APPLICATION;
	} else {
		info->tag_type = MS_OFFICE_XML_TAG_INVALID;
	}

	if (info->tag_type != MS_OFFICE_XML_TAG_INVALID)
		g_string_set_size (info->text_buf, 0);
}

static void
msoffice_xml_metadata_end_element_ns (void          *ctx,
                                      const xmlChar *localname G_GNUC_UNUSED,
                                      const xmlChar *prefix G_GNUC_UNUSED,
                                      const xmlChar *URI G_GNUC_UNUSED)
{
	MsOfficeXMLParserInfo *info = ctx;

	if (info->tag_type != MS_OFFICE_XML_TAG_INVALID && info->text_buf->len > 0) {
		msoffice_xml_metadata_parse (NULL,
		                             info->text_buf->str,
		                             info->text_buf->len,
		                             info,
		                             NULL);
	}

	info->tag_type = MS_OFFICE_XML_TAG_INVALID;
	g_string_set_size (info->text_buf, 0);
}

static void
msoffice_xml_metadata_parse (gpointer     context G_GNUC_UNUSED,
                             const gchar *text,
                             gsize        text_len G_GNUC_UNUSED,
                             gpointer     user_data,
                             GError     **error G_GNUC_UNUSED)
{
	MsOfficeXMLParserInfo *info = user_data;

	if (!text || *text == '\0')
		return;

	switch (info->tag_type) {
	case MS_OFFICE_XML_TAG_TITLE:
		tracker_resource_set_string (info->metadata, "nie:title", text);
		break;

	case MS_OFFICE_XML_TAG_SUBJECT:
		tracker_resource_set_string (info->metadata, "nie:subject", text);
		break;

	case MS_OFFICE_XML_TAG_AUTHOR: {
		TrackerResource *publisher = tracker_extract_new_contact (text);
		tracker_resource_set_relation (info->metadata, "nco:publisher", publisher);
		g_object_unref (publisher);
		break;
	}

	case MS_OFFICE_XML_TAG_COMMENTS:
		tracker_resource_set_string (info->metadata, "nie:comment", text);
		break;

	case MS_OFFICE_XML_TAG_CREATED: {
		gchar *date = tracker_date_guess (text);
		if (date) {
			tracker_resource_set_string (info->metadata, "nie:contentCreated", date);
			g_free (date);
		} else {
			g_warning ("Could not parse creation time (%s) from MsOffice XML document '%s'",
			           text, info->uri);
		}
		break;
	}

	case MS_OFFICE_XML_TAG_MODIFIED: {
		gchar *date = tracker_date_guess (text);
		if (date) {
			tracker_resource_set_string (info->metadata, "nie:contentLastModified", date);
			g_free (date);
		} else {
			g_warning ("Could not parse last modification time (%s) from MsOffice XML document '%s'",
			           text, info->uri);
		}
		break;
	}

	case MS_OFFICE_XML_TAG_GENERATOR:
	case MS_OFFICE_XML_TAG_APPLICATION:
		tracker_resource_set_string (info->metadata, "nie:generator", text);
		break;

	case MS_OFFICE_XML_TAG_NUM_OF_PAGES:
		tracker_resource_set_string (info->metadata, "nfo:pageCount", text);
		break;

	case MS_OFFICE_XML_TAG_NUM_OF_CHARACTERS:
		tracker_resource_set_string (info->metadata, "nfo:characterCount", text);
		break;

	case MS_OFFICE_XML_TAG_NUM_OF_WORDS:
		tracker_resource_set_string (info->metadata, "nfo:wordCount", text);
		break;

	case MS_OFFICE_XML_TAG_NUM_OF_LINES:
		tracker_resource_set_string (info->metadata, "nfo:lineCount", text);
		break;

	case MS_OFFICE_XML_TAG_NUM_OF_PARAGRAPHS:
		/* TODO: no ontology */
		break;

	default:
		break;
	}
}

/* ------------------------- CONTENT-TYPES file parsing -----------------------------------*/

#define ZIP_XML_BUFFER_SIZE 8192

G_MODULE_EXPORT gboolean
tracker_extract_module_init (GError **error)
{
       xmlInitParser ();
       return TRUE;
}

static gboolean
parse_xml_from_zip_sax (const gchar         *zip_uri,
                        const gchar         *member_name,
                        const xmlSAXHandler *sax,
                        gpointer             user_data,
                        GError             **error)
{
	g_autoptr (GInputStream) stream = NULL;
	g_autoptr (GBytes) bytes = NULL;
	g_autoptr (GError) inner_error = NULL;
	xmlParserCtxtPtr ctxt;
	const guint8 *data;
	gsize len;
	gboolean ok = TRUE;

	stream = tracker_zip_read_file (zip_uri, member_name, NULL, error);
	if (!stream)
		return FALSE;

	ctxt = xmlCreatePushParserCtxt ((xmlSAXHandler *) sax,
	                                user_data,
	                                NULL, 0,
	                                member_name);
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

static gboolean
xml_read (MsOfficeXMLParserInfo *parser_info,
          const gchar           *xml_filename,
          MsOfficeXMLTagType     type)
{
	g_autoptr (GError) error = NULL;
	xmlSAXHandler sax = {
		.initialized = XML_SAX2_MAGIC,
	};

	/* State reset */
	parser_info->tag_type = MS_OFFICE_XML_TAG_INVALID;
	g_string_set_size (parser_info->text_buf, 0);

	if (parser_info->file_type == FILE_TYPE_XLSX) {
		parser_info->in_xlsx_shared_strings =
			(g_strrstr (xml_filename, "sharedStrings") != NULL);
	}

	switch (type) {
	case MS_OFFICE_XML_TAG_DOCUMENT_CORE_DATA:
		sax.startElementNs = msoffice_xml_metadata_start_element_ns;
		sax.endElementNs   = msoffice_xml_metadata_end_element_ns;
		sax.characters     = msoffice_xml_characters;
		break;

	case MS_OFFICE_XML_TAG_DOCUMENT_TEXT_DATA:
		parser_info->style_element_present = FALSE;
		parser_info->preserve_attribute_present = FALSE;

		sax.startElementNs = msoffice_xml_content_start_element_ns;
		sax.endElementNs   = msoffice_xml_content_end_element_ns;
		sax.characters     = msoffice_xml_characters;
		break;

	default:
		return TRUE;
	}

	if (!parse_xml_from_zip_sax (parser_info->uri, xml_filename, &sax, parser_info, &error) && error) {
		g_debug ("Parsing internal '%s' gave error: '%s'",
		         xml_filename,
		         error->message);
	}

	return TRUE;
}

static gint
compare_slide_name (gconstpointer a,
                    gconstpointer b)
{
	gchar *col_a, *col_b;
	gint result;

	col_a = g_utf8_collate_key_for_filename (a, -1);
	col_b = g_utf8_collate_key_for_filename (b, -1);
	result = strcmp (col_a, col_b);

	g_free (col_a);
	g_free (col_b);

	return result;
}

static void
msoffice_xml_content_types_start_element_ns (void           *ctx,
                                             const xmlChar  *localname,
                                             const xmlChar  *prefix G_GNUC_UNUSED,
                                             const xmlChar  *URI G_GNUC_UNUSED,
                                             int             nb_namespaces G_GNUC_UNUSED,
                                             const xmlChar **namespaces G_GNUC_UNUSED,
                                             int             nb_attributes,
                                             int             nb_defaulted G_GNUC_UNUSED,
                                             const xmlChar **attributes)
{
	MsOfficeXMLParserInfo *info = ctx;
	g_autofree gchar *part_name = NULL;
	g_autofree gchar *content_type = NULL;

	if (g_ascii_strcasecmp ((const gchar *) localname, "Override") != 0)
		return;

	for (int i = 0; i < nb_attributes; i++) {
		const xmlChar *a_local = attributes[i*5 + 0];
		const xmlChar *v_start = attributes[i*5 + 3];
		const xmlChar *v_end   = attributes[i*5 + 4];
		int v_len = (int) (v_end - v_start);

		if (g_ascii_strcasecmp ((const gchar *) a_local, "PartName") == 0) {
			part_name = g_strndup ((const gchar *) v_start, v_len);
		} else if (g_ascii_strcasecmp ((const gchar *) a_local, "ContentType") == 0) {
			content_type = g_strndup ((const gchar *) v_start, v_len);
		}
	}

	if (!part_name || !content_type)
		return;

	/* Metadata part? */
	if (g_ascii_strcasecmp (content_type, "application/vnd.openxmlformats-package.core-properties+xml") == 0 ||
	    g_ascii_strcasecmp (content_type, "application/vnd.openxmlformats-officedocument.extended-properties+xml") == 0) {
		xml_read (info, part_name + 1, MS_OFFICE_XML_TAG_DOCUMENT_CORE_DATA);
		return;
	}

	/* If the file type is unknown, skip content */
	if (info->file_type == FILE_TYPE_INVALID)
		return;

	/* Content part? */
	if ((info->file_type == FILE_TYPE_DOCX &&
	     g_ascii_strcasecmp (content_type, "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml") == 0) ||
	    ((info->file_type == FILE_TYPE_PPTX || info->file_type == FILE_TYPE_PPSX) &&
	     (g_ascii_strcasecmp (content_type, "application/vnd.openxmlformats-officedocument.presentationml.slide+xml") == 0 ||
	      g_ascii_strcasecmp (content_type, "application/vnd.openxmlformats-officedocument.drawingml.diagramData+xml") == 0)) ||
	    (info->file_type == FILE_TYPE_XLSX &&
	     (g_ascii_strcasecmp (content_type, "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml") == 0 ||
	      g_ascii_strcasecmp (content_type, "application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml") == 0))) {

		if (info->file_type == FILE_TYPE_PPTX) {
			info->parts = g_list_insert_sorted (info->parts,
			                                    g_strdup (part_name + 1),
			                                    compare_slide_name);
		} else {
			info->parts = g_list_append (info->parts, g_strdup (part_name + 1));
		}
	}
}

/* ------------------------- Main methods -----------------------------------*/

static MsOfficeXMLFileType
msoffice_xml_get_file_type (const gchar *uri)
{
	GFile *file;
	GFileInfo *file_info;
	const gchar *mime_used;
	MsOfficeXMLFileType file_type;

	/* Get GFile from uri... */
	file = g_file_new_for_uri (uri);
	if (!file) {
		g_warning ("Could not create GFile for URI:'%s'", uri);
		return FILE_TYPE_INVALID;
	}

	/* Get GFileInfo from GFile... (synchronous) */
	file_info = g_file_query_info (file,
	                               G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
	                               G_FILE_QUERY_INFO_NONE,
	                               NULL,
	                               NULL);
	g_object_unref (file);
	if (!file_info) {
		g_warning ("Could not get GFileInfo for URI:'%s'", uri);
		return FILE_TYPE_INVALID;
	}

	/* Get Content Type from GFileInfo. The constant string will be valid
	 * as long as the file info reference is valid */
	mime_used = g_file_info_get_content_type (file_info);
	if (g_ascii_strcasecmp (mime_used, "application/vnd.openxmlformats-officedocument.wordprocessingml.document") == 0) {
		/* MsOffice Word document */
		file_type = FILE_TYPE_DOCX;
	} else if (g_ascii_strcasecmp (mime_used, "application/vnd.openxmlformats-officedocument.presentationml.presentation") == 0) {
		/* MsOffice Powerpoint document */
		file_type = FILE_TYPE_PPTX;
	} else if (g_ascii_strcasecmp (mime_used, "application/vnd.openxmlformats-officedocument.presentationml.slideshow") == 0) {
		/* MsOffice Powerpoint (slideshow) document */
		file_type = FILE_TYPE_PPSX;
	} else if (g_ascii_strcasecmp (mime_used, "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet") == 0) {
		/* MsOffice Excel document */
		file_type = FILE_TYPE_XLSX;
	} else {
		g_debug ("Mime type was not recognised:'%s'", mime_used);
		file_type = FILE_TYPE_INVALID;
	}

	g_object_unref (file_info);

	return file_type;
}

static void
extract_content (MsOfficeXMLParserInfo *info)
{
	GList *parts;

	if (!info->parts) {
		return;
	}

	for (parts = info->parts; parts; parts = parts->next) {
		const gchar *part_name;

		part_name = parts->data;
		/* If reached max bytes to extract, don't event start parsing the file... just return */
		if (info->bytes_pending == 0) {
			g_debug ("Skipping '%s' as already reached max bytes to extract",
			         part_name);
			break;
		} else {
			xml_read (info, part_name, MS_OFFICE_XML_TAG_DOCUMENT_TEXT_DATA);
		}
	}
}

G_MODULE_EXPORT gboolean
tracker_extract_get_metadata (TrackerExtractInfo  *extract_info,
                              GError             **error)
{
	MsOfficeXMLParserInfo info = { 0 };
	MsOfficeXMLFileType file_type;
	TrackerResource *metadata;
	GError *inner_error = NULL;
	GFile *file;
	gchar *uri, *resource_uri;
	xmlSAXHandler sax = {
		.initialized = XML_SAX2_MAGIC,
		.startElementNs = msoffice_xml_content_types_start_element_ns,
	};

	if (G_UNLIKELY (maximum_size_error_quark == 0)) {
		maximum_size_error_quark = g_quark_from_static_string ("maximum_size_error");
	}

	file = tracker_extract_info_get_file (extract_info);
	uri = g_file_get_uri (file);

	/* Get current Content Type */
	file_type = msoffice_xml_get_file_type (uri);

	g_debug ("Extracting MsOffice XML format...");

	resource_uri = tracker_extract_info_get_content_id (extract_info, NULL);
	metadata = tracker_resource_new (resource_uri);
	tracker_resource_add_uri (metadata, "rdf:type", "nfo:PaginatedTextDocument");
	g_free (resource_uri);

	/* Setup Parser info */
	info.metadata = metadata;
	info.file_type = file_type;
	info.tag_type = MS_OFFICE_XML_TAG_INVALID;
	info.in_xlsx_shared_strings = FALSE;
	info.style_element_present = FALSE;
	info.preserve_attribute_present = FALSE;
	info.uri = uri;
	info.content = NULL;
	info.bytes_pending = tracker_extract_info_get_max_text (extract_info);
	info.text_buf = g_string_new ("");

	if (!parse_xml_from_zip_sax (uri, "[Content_Types].xml", &sax, &info, &inner_error)) {
		if (inner_error)
			g_propagate_prefixed_error (error, inner_error, "Could not open:");
		else
			g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not open: unknown error");

		g_free (uri);
		g_object_unref (metadata);
		if (info.text_buf)
			g_string_free (info.text_buf, TRUE);

		return FALSE;
	}

	extract_content (&info);

	/* If we got any content, add it */
	if (info.content) {
		gchar *content;

		content = g_string_free (info.content, FALSE);
		info.content = NULL;

		if (content) {
			tracker_resource_set_string (metadata, "nie:plainTextContent", content);
			g_free (content);
		}
	}

	if (info.parts) {
		g_list_foreach (info.parts, (GFunc) g_free, NULL);
		g_list_free (info.parts);
	}

	g_free (uri);

	tracker_extract_info_set_resource (extract_info, metadata);
	g_object_unref (metadata);

	if (info.text_buf)
		g_string_free (info.text_buf, TRUE);

	return TRUE;
}
