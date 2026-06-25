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
	OPF_TAG_TYPE_UNKNOWN,
	OPF_TAG_TYPE_TITLE,
	OPF_TAG_TYPE_CREATED,

	OPF_TAG_TYPE_AUTHOR,
	OPF_TAG_TYPE_EDITOR,
	OPF_TAG_TYPE_ILLUSTRATOR,
	OPF_TAG_TYPE_CONTRIBUTOR,

	OPF_TAG_TYPE_LANGUAGE,
	OPF_TAG_TYPE_SUBJECT,
	OPF_TAG_TYPE_DESCRIPTION,
	OPF_TAG_TYPE_UUID,
	OPF_TAG_TYPE_ISBN,
	OPF_TAG_TYPE_PUBLISHER,
	OPF_TAG_TYPE_RATING  /* calibre addition, should it be indexed? how? */
} OPFTagType;

typedef struct {
	TrackerResource *resource;
	gchar *uri;

	OPFTagType element;
	GList *pages;
	guint in_metadata : 1;
	guint in_manifest : 1;
	gchar *savedstring;
	GString *text_buf;
} OPFData;

typedef struct {
	GString *contents;
	gsize limit;
} OPFContentData;

static inline OPFData *
opf_data_new (const char *uri,
              TrackerResource *resource)
{
	OPFData *data = g_new0 (OPFData, 1);

	data->uri = g_strdup (uri);
	data->resource = g_object_ref (resource);
	data->text_buf = g_string_new ("");

	return data;
}

static inline void
opf_data_clear_saved_string (OPFData *data)
{
	if (!data || !data->savedstring) {
		return;
	}

	g_free (data->savedstring);
	data->savedstring = NULL;
}

static inline void
opf_data_free (OPFData *data)
{
	if (!data) {
		return;
	}

	g_free (data->savedstring);

	g_list_foreach (data->pages, (GFunc) g_free, NULL);
	g_list_free (data->pages);

	if (data->text_buf)
		g_string_free (data->text_buf, TRUE);

	g_object_unref (data->resource);
	g_free (data->uri);
	g_free (data);
}

/* Methods to parse the container.xml file
 * pointing to the real metadata/content
 */
static void
container_start_element_ns (void           *user_data,
                            const xmlChar  *localname,
                            const xmlChar  *prefix G_GNUC_UNUSED,
                            const xmlChar  *URI G_GNUC_UNUSED,
                            int             nb_namespaces G_GNUC_UNUSED,
                            const xmlChar **namespaces G_GNUC_UNUSED,
                            int             nb_attributes,
                            int             nb_defaulted G_GNUC_UNUSED,
                            const xmlChar **attributes)
{
	char **path_out = user_data;

	if (g_strcmp0 ((const char *) localname, "rootfile") != 0)
		return;

	for (int i = 0; i < nb_attributes; i++) {
		const xmlChar *attr_local = attributes[i*5 + 0];
		const xmlChar *value_start = attributes[i*5 + 3];
		const xmlChar *value_end   = attributes[i*5 + 4];
		int value_len = (int) (value_end - value_start);

		if (g_strcmp0 ((const char *) attr_local, "full-path") == 0) {
			if (*path_out == NULL) {
				*path_out = g_strndup ((const char *) value_start, value_len);
			}
			break;
		}
	}
}

static void
opf_characters (void          *user_data,
		const xmlChar *ch,
		int            len)
{
	OPFData *data = user_data;

	if (len <= 0 || !ch)
		return;

	if (data->element != OPF_TAG_TYPE_UNKNOWN)
		g_string_append_len (data->text_buf, (const gchar *) ch, len);
}

static void
opf_apply_text (OPFData      *data,
                const gchar  *text)
{
	switch (data->element) {
	case OPF_TAG_TYPE_PUBLISHER: {
		TrackerResource *publisher;

		publisher = tracker_resource_new (NULL);
		tracker_resource_set_uri (publisher, "rdf:type", "nco:Contact");
		tracker_resource_set_string (publisher, "nco:fullname", text);

		tracker_resource_set_relation (data->resource, "nco:publisher", publisher);
		g_object_unref (publisher);
		break;
	}

	case OPF_TAG_TYPE_AUTHOR:
	case OPF_TAG_TYPE_EDITOR:
	case OPF_TAG_TYPE_ILLUSTRATOR:
	case OPF_TAG_TYPE_CONTRIBUTOR: {
		TrackerResource *contact, *artist = NULL;
		gchar *fname = NULL, *gname = NULL, *oname = NULL;
		const gchar *fullname = NULL;
		gchar *role_uri = NULL;
		const gchar *role_str = NULL;
		gint i, j = 0, len;

		/* parse name. may not work for dissimilar cultures. */
		if (data->savedstring != NULL) {
			fullname = data->savedstring;

			/* <family name>, <given name> <other name> */
			len = (gint) strlen (data->savedstring);

			for (i = 0; i < len; i++) {
				if (data->savedstring[i] == ',') {
					fname = g_strndup (data->savedstring, i);

					for (; data->savedstring[i] == ',' || data->savedstring[i] == ' '; i++)
						;
					j = i;

					break;
				}
			}

			if (!fname && i == len) {
				fname = g_strdup (data->savedstring);
			} else {
				for (; i <= len; i++) {
					if (i == len || data->savedstring[i] == ' ') {
						gname = g_strndup (data->savedstring + j, i - j);

						for (; data->savedstring[i] == ',' || data->savedstring[i] == ' '; i++)
							;

						if (i != len)
							oname = g_strdup (data->savedstring + i);

						break;
					}
				}
			}
		} else {
			fullname = text;

			/* <given name> <other name> <family name> */
			len = (gint) strlen (text);

			for (i = 0; i < len; i++) {
				if (text[i] == ' ') {
					gname = g_strndup (text, i);
					j = i + 1;
					break;
				}
			}

			if (j == 0) {
				fname = g_strdup (text);
			} else {
				for (i = len - 1; i >= j - 1; i--) {
					if (text[i] == ' ') {
						fname = g_strdup (text + i + 1);

						if (i > j)
							oname = g_strndup (text + j, i - j);

						break;
					}
				}
			}
		}

		/* Role details */
		role_uri = tracker_sparql_escape_uri_printf ("urn:artist:%s", fullname);

		if (data->element == OPF_TAG_TYPE_AUTHOR) {
			role_str = "nco:creator";
		} else if (data->element == OPF_TAG_TYPE_EDITOR) {
			role_str = "nco:publisher";
		} else {
			/* illustrator/contributor */
			role_str = "nco:contributor";
		}

		if (role_uri) {
			artist = tracker_resource_new (role_uri);
			tracker_resource_set_uri (artist, "rdf:type", "nmm:Artist");
			tracker_resource_set_string (artist, "nmm:artistName", fullname);
		}

		/* Creator contact details */
		contact = tracker_resource_new (NULL);
		tracker_resource_set_uri (contact, "rdf:type", "nco:PersonContact");
		tracker_resource_set_string (contact, "nco:fullname", fullname);

		if (fname) {
			tracker_resource_set_string (contact, "nco:nameFamily", fname);
			g_free (fname);
		}

		if (gname) {
			tracker_resource_set_string (contact, "nco:nameGiven", gname);
			g_free (gname);
		}

		if (oname) {
			tracker_resource_set_string (contact, "nco:nameAdditional", oname);
			g_free (oname);
		}

		if (role_uri) {
			tracker_resource_set_relation (contact, role_str, artist);
			g_free (role_uri);
		}

		tracker_resource_set_relation (data->resource, role_str, contact);

		g_clear_object (&artist);
		g_object_unref (contact);

		break;
	}

	case OPF_TAG_TYPE_TITLE:
		tracker_resource_set_string (data->resource, "nie:title", text);
		break;

	case OPF_TAG_TYPE_CREATED: {
		gchar *date = tracker_date_guess (text);

		if (date) {
			tracker_resource_set_string (data->resource, "nie:contentCreated", date);
			g_free (date);
		} else {
			g_warning ("Could not parse creation time (%s) in EPUB '%s'",
			           text, data->uri);
		}
		break;
	}

	case OPF_TAG_TYPE_LANGUAGE:
		tracker_resource_set_string (data->resource, "nie:language", text);
		break;

	case OPF_TAG_TYPE_SUBJECT:
		tracker_resource_set_string (data->resource, "nie:subject", text);
		break;

	case OPF_TAG_TYPE_DESCRIPTION:
		tracker_resource_set_string (data->resource, "nie:description", text);
		break;

	case OPF_TAG_TYPE_UUID:
	case OPF_TAG_TYPE_ISBN:
		tracker_resource_set_string (data->resource, "nie:identifier", text);
		break;

	default:
		break;
	}

	opf_data_clear_saved_string (data);
}

static void
opf_start_element_ns (void           *ctx,
                      const xmlChar  *localname,
                      const xmlChar  *prefix,
                      const xmlChar  *URI G_GNUC_UNUSED,
                      int             nb_namespaces G_GNUC_UNUSED,
                      const xmlChar **namespaces G_GNUC_UNUSED,
                      int             nb_attributes,
                      int             nb_defaulted G_GNUC_UNUSED,
                      const xmlChar **attributes)
{
	OPFData *data = ctx;
	const gchar *lname = (const gchar *) localname;
	g_autofree gchar *qname = NULL;

	if (g_strcmp0 (lname, "metadata") == 0) {
		data->in_metadata = TRUE;
		return;
	} else if (g_strcmp0 (lname, "manifest") == 0) {
		data->in_manifest = TRUE;
		return;
	}

	if (prefix && *prefix)
		qname = g_strdup_printf ("%s:%s", (const gchar *) prefix, lname);
	else
		qname = g_strdup (lname);

	if (data->in_metadata) {
		gboolean has_role_attr = FALSE;

		if (g_strcmp0 (qname, "dc:title") == 0) {
			data->element = OPF_TAG_TYPE_TITLE;
		} else if (g_strcmp0 (qname, "dc:creator") == 0) {
			for (int i = 0; i < nb_attributes; i++) {
				const xmlChar *attr_local  = attributes[i*5 + 0];
				const xmlChar *attr_prefix = attributes[i*5 + 1];
				const xmlChar *value_start = attributes[i*5 + 3];
				const xmlChar *value_end   = attributes[i*5 + 4];
				int value_len = (int) (value_end - value_start);
				g_autofree gchar *attr_q = NULL;

				if (attr_prefix && *attr_prefix)
					attr_q = g_strdup_printf ("%s:%s",
					                          (const gchar *) attr_prefix,
					                          (const gchar *) attr_local);
				else
					attr_q = g_strdup ((const gchar *) attr_local);

				if (g_strcmp0 (attr_q, "opf:file-as") == 0 || g_strcmp0 (attr_q, "file-as") == 0) {
					g_free (data->savedstring);
					data->savedstring = g_strndup ((const gchar *) value_start, value_len);
				} else if (g_strcmp0 (attr_q, "opf:role") == 0 || g_strcmp0 (attr_q, "role") == 0) {
					g_autofree gchar *role = g_strndup ((const gchar *) value_start, value_len);

					has_role_attr = TRUE;

					if (g_strcmp0 (role, "aut") == 0)
						data->element = OPF_TAG_TYPE_AUTHOR;
					else if (g_strcmp0 (role, "edt") == 0)
						data->element = OPF_TAG_TYPE_EDITOR;
					else if (g_strcmp0 (role, "ill") == 0)
						data->element = OPF_TAG_TYPE_ILLUSTRATOR;
					else {
						data->element = OPF_TAG_TYPE_UNKNOWN;
						opf_data_clear_saved_string (data);
					}
				}
			}
			if (!has_role_attr)
				data->element = OPF_TAG_TYPE_AUTHOR;

		} else if (g_strcmp0 (qname, "dc:date") == 0) {
			for (int i = 0; i < nb_attributes; i++) {
				const xmlChar *attr_local  = attributes[i*5 + 0];
				const xmlChar *attr_prefix = attributes[i*5 + 1];
				const xmlChar *value_start = attributes[i*5 + 3];
				const xmlChar *value_end   = attributes[i*5 + 4];
				int value_len = (int) (value_end - value_start);
				g_autofree gchar *attr_q = NULL;

				if (attr_prefix && *attr_prefix)
					attr_q = g_strdup_printf ("%s:%s",
					                          (const gchar *) attr_prefix,
					                          (const gchar *) attr_local);
				else
					attr_q = g_strdup ((const gchar *) attr_local);

				if (g_strcmp0 (attr_q, "opf:event") == 0) {
					g_autofree gchar *val = g_strndup ((const gchar *) value_start, value_len);

					if (g_strcmp0 (val, "original-publication") == 0) {
						data->element = OPF_TAG_TYPE_CREATED;
						break;
					}
				}
			}
		} else if (g_strcmp0 (qname, "dc:publisher") == 0) {
			data->element = OPF_TAG_TYPE_PUBLISHER;
		} else if (g_strcmp0 (qname, "dc:description") == 0) {
			data->element = OPF_TAG_TYPE_DESCRIPTION;
		} else if (g_strcmp0 (qname, "dc:language") == 0) {
			data->element = OPF_TAG_TYPE_LANGUAGE;
		} else if (g_strcmp0 (qname, "dc:subject") == 0) {
			data->element = OPF_TAG_TYPE_SUBJECT;
		} else if (g_strcmp0 (qname, "dc:identifier") == 0) {
			data->element = OPF_TAG_TYPE_UUID;

			for (int i = 0; i < nb_attributes; i++) {
				const xmlChar *attr_local  = attributes[i*5 + 0];
				const xmlChar *attr_prefix = attributes[i*5 + 1];
				const xmlChar *value_start = attributes[i*5 + 3];
				const xmlChar *value_end   = attributes[i*5 + 4];
				int value_len = (int) (value_end - value_start);
				g_autofree gchar *attr_q = NULL;

				if (attr_prefix && *attr_prefix)
					attr_q = g_strdup_printf ("%s:%s",
					                          (const gchar *) attr_prefix,
					                          (const gchar *) attr_local);
				else
					attr_q = g_strdup ((const gchar *) attr_local);

				if (g_strcmp0 (attr_q, "opf:scheme") == 0) {
					g_autofree gchar *val = g_strndup ((const gchar *) value_start, value_len);

					if (g_ascii_strncasecmp (val, "isbn", 4) == 0)
						data->element = OPF_TAG_TYPE_ISBN;
				}
			}
		}

		g_string_set_size (data->text_buf, 0);
		return;
	}

	/* Outside <metadata> we don't want to accumulate metadata text */
	if (!data->in_metadata)
		data->element = OPF_TAG_TYPE_UNKNOWN;

	if (data->in_manifest && g_strcmp0 (lname, "item") == 0) {
		g_autofree gchar *href = NULL;
		gboolean is_xhtml = FALSE;

		for (int i = 0; i < nb_attributes; i++) {
			const xmlChar *attr_local  = attributes[i*5 + 0];
			const xmlChar *value_start = attributes[i*5 + 3];
			const xmlChar *value_end   = attributes[i*5 + 4];
			int value_len = (int) (value_end - value_start);

			if (!href && g_strcmp0 ((const gchar *) attr_local, "href") == 0) {
				href = g_strndup ((const gchar *) value_start, value_len);
			} else if (g_strcmp0 ((const gchar *) attr_local, "media-type") == 0) {
				g_autofree gchar *mt = g_strndup ((const gchar *) value_start, value_len);

				if (g_strcmp0 (mt, "application/xhtml+xml") == 0)
					is_xhtml = TRUE;
			}
		}

		if (is_xhtml && href)
			data->pages = g_list_append (data->pages, g_strdup (href));
	}
}

static void
opf_end_element_ns (void          *ctx,
                    const xmlChar *localname,
                    const xmlChar *prefix G_GNUC_UNUSED,
                    const xmlChar *URI G_GNUC_UNUSED)
{
	OPFData *data = ctx;
	const gchar *lname = (const gchar *) localname;

	if (g_strcmp0 (lname, "metadata") == 0) {
		data->in_metadata = FALSE;
	} else if (g_strcmp0 (lname, "manifest") == 0) {
		data->in_manifest = FALSE;
	} else if (data->in_metadata) {
		g_autofree gchar *qname = NULL;

		if (prefix && *prefix)
			qname = g_strdup_printf ("%s:%s", (const gchar *) prefix, (const gchar *) localname);
		else
			qname = g_strdup ((const gchar *) localname);

		if ((g_strcmp0 (qname, "dc:title") == 0 ||
		     g_strcmp0 (qname, "dc:creator") == 0 ||
		     g_strcmp0 (qname, "dc:date") == 0 ||
		     g_strcmp0 (qname, "dc:publisher") == 0 ||
		     g_strcmp0 (qname, "dc:description") == 0 ||
		     g_strcmp0 (qname, "dc:language") == 0 ||
		     g_strcmp0 (qname, "dc:subject") == 0 ||
		     g_strcmp0 (qname, "dc:identifier") == 0) &&
		    data->element != OPF_TAG_TYPE_UNKNOWN &&
		    data->text_buf->len > 0) {
			opf_apply_text (data, data->text_buf->str);
		}

		data->element = OPF_TAG_TYPE_UNKNOWN;
		g_string_set_size (data->text_buf, 0);
	}
}

static void
content_characters (void          *ctx,
		    const xmlChar *ch,
		    int            len)
{
	OPFContentData *content_data = ctx;
	gsize written_bytes = 0;

	if (len <= 0 || !ch || content_data->limit == 0)
		return;

	tracker_text_validate_utf8 ((const gchar *) ch,
	                            MIN ((gsize) len, content_data->limit),
	                            &content_data->contents,
	                            &written_bytes);

	if (written_bytes > 0) {
		content_data->limit -= written_bytes;

		/* Padding between libxml2 "characters" chunks */
		if (content_data->contents->len > 0 && content_data->limit > 0) {
			gchar last = content_data->contents->str[content_data->contents->len - 1];

			if (last != ' ' && last != '\n' && last != '\t' && last != '\r') {
				g_string_append_c (content_data->contents, ' ');
				content_data->limit--;
			}
		}
	}
}

#define ZIP_XML_BUFFER_SIZE 8192

G_MODULE_EXPORT gboolean
tracker_extract_module_init (GError **error)
{
	xmlInitParser ();
	return TRUE;
}

static gboolean
parse_xml_from_zip_sax (const gchar          *zip_uri,
                        const gchar          *member_name,
                        const xmlSAXHandler  *sax,
                        gpointer              user_data,
                        GError              **error)
{
	g_autoptr (GInputStream) stream = NULL;
	g_autoptr (GError) inner_error = NULL;
	g_autoptr (GBytes) bytes = NULL;
	xmlParserCtxtPtr ctx;
	const guint8 *data;
	gsize len;
	gboolean ok = TRUE;

	stream = tracker_zip_read_file (zip_uri, member_name, NULL, error);
	if (!stream)
		return FALSE;

	/* Push parser context */
	ctx = xmlCreatePushParserCtxt ((xmlSAXHandler*) sax, user_data, NULL, 0, NULL);
	g_assert (ctx != NULL);

	ctx->options |= XML_PARSE_NONET;

	while ((bytes = g_input_stream_read_bytes (stream,
	                                           ZIP_XML_BUFFER_SIZE,
	                                           NULL,
	                                           &inner_error)) != NULL) {
		len = g_bytes_get_size (bytes);

		if (len == 0) {
			break;
		}

		data = g_bytes_get_data (bytes, NULL);

		if (xmlParseChunk (ctx, (const char *) data, (int) len, 0) != 0) {
			ok = FALSE;
			break;
		}

		g_clear_pointer (&bytes, g_bytes_unref);
	}

	if (inner_error)
		ok = FALSE;

	if (ok) {
		if (xmlParseChunk (ctx, NULL, 0, 1) != 0)
			ok = FALSE;
	}

	if (!ok) {
		if (inner_error) {
			g_propagate_error (error, inner_error);
		} else if (ctx->lastError.message) {
			g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			             "XML error in %s (line %d): %s",
			             member_name,
			             ctx->lastError.line,
			             ctx->lastError.message);
		} else {
			g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			             "XML error in %s", member_name);
		}
	}

	xmlFreeParserCtxt (ctx);

	g_input_stream_close (stream, NULL, NULL);

	return ok;
}

static gchar *
extract_opf_path (const gchar *uri)
{
	g_autofree char *path = NULL;
	g_autoptr(GError) error = NULL;
	xmlSAXHandler sax = {
		.initialized = XML_SAX2_MAGIC,
		.startElementNs = container_start_element_ns,
	};

	parse_xml_from_zip_sax (uri, "META-INF/container.xml", &sax, &path, &error);

	if (error) {
		g_warning ("Could not get EPUB container.xml file: %s", error->message);
		return NULL;
	}

	return g_steal_pointer (&path);
}

static gchar *
extract_opf_contents (TrackerExtractInfo *info,
                      const gchar        *uri,
                      const gchar        *content_prefix,
                      GList              *content_files)
{
	OPFContentData content_data = { 0 };
	GError *error = NULL;
	GList *l;
	xmlSAXHandler sax = {
		.initialized = XML_SAX2_MAGIC,
		.characters = content_characters,
	};

	content_data.contents = g_string_new ("");
	content_data.limit = (gsize) tracker_extract_info_get_max_text (info);

	g_debug ("Extracting up to %" G_GSIZE_FORMAT " bytes of content", content_data.limit);

	for (l = content_files; l; l = l->next) {
		gchar *path;

		/* Page file is relative to OPF file location */
		if (g_strcmp0 (content_prefix, ".") == 0)
			path = g_strdup (l->data);
		else
			path = g_build_filename (content_prefix, l->data, NULL);

		parse_xml_from_zip_sax (uri, path, &sax, &content_data, &error);

		if (error) {
			g_warning ("Error extracting EPUB contents (%s): %s",
				   path, error->message);
			g_clear_error (&error);
		}
		g_free (path);

		if (content_data.limit <= 0) {
			/* Reached plain text extraction limit */
			break;
		}
	}

	return g_string_free (content_data.contents, FALSE);
}

static TrackerResource *
extract_opf (TrackerExtractInfo *info,
             const gchar        *uri,
             const gchar        *opf_path)
{
	TrackerResource *ebook;
	OPFData *data = NULL;
	GError *error = NULL;
	gchar *dirname, *contents, *resource_uri;
	GFile *file;
	xmlSAXHandler sax = {
		.initialized = XML_SAX2_MAGIC,
		.startElementNs = opf_start_element_ns,
		.endElementNs = opf_end_element_ns,
		.characters = opf_characters,
	};

	g_debug ("Extracting OPF file contents from EPUB '%s'", uri);

	file = g_file_new_for_uri (uri);
	resource_uri = tracker_extract_info_get_content_id (info, NULL);
	ebook = tracker_resource_new (resource_uri);
	tracker_resource_add_uri (ebook, "rdf:type", "nfo:EBook");
	g_free (resource_uri);
	g_object_unref (file);

	data = opf_data_new (uri, ebook);

	parse_xml_from_zip_sax (uri, opf_path, &sax, data, &error);

	if (error) {
		g_warning ("Could not get EPUB '%s' file: %s\n", opf_path,
		           error->message);
		g_error_free (error);
		opf_data_free (data);
		g_object_unref (ebook);
		return NULL;
	}

	dirname = g_path_get_dirname (opf_path);
	contents = extract_opf_contents (info, uri, dirname, data->pages);
	g_free (dirname);

	if (contents && *contents) {
		tracker_resource_set_string (ebook, "nie:plainTextContent", contents);
	}

	opf_data_free (data);
	g_free (contents);

	return ebook;
}

G_MODULE_EXPORT gboolean
tracker_extract_get_metadata (TrackerExtractInfo  *info,
                              GError             **error)
{
	g_autoptr (TrackerResource) ebook = NULL;
	g_autofree char *opf_path = NULL, *uri = NULL;
	GFile *file;

	file = tracker_extract_info_get_file (info);
	uri = g_file_get_uri (file);

	opf_path = extract_opf_path (uri);

	if (!opf_path)
		return FALSE;

	ebook = extract_opf (info, uri, opf_path);

	tracker_extract_info_set_resource (info, ebook);

	return TRUE;
}
