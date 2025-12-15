/*
 * Copyright (C) 2006, Jamie McCracken <jamiemcc@gnome.org>
 * Copyright (C) 2008-2011, Nokia <ivan.frade@nokia.com>
 * Copyright (C) 2010, Amit Aggarwal <amitcs06@gmail.com>
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

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <poppler.h>

#include <gio/gunixoutputstream.h>
#include <gio/gunixinputstream.h>

#include <tracker-common.h>

#include "utils/tracker-extract.h"

#include "tracker-main.h"

#ifdef HAVE_EXEMPI
#include "tracker-xmp.h"
#endif

typedef struct {
	gchar *title;
	gchar *subject;
	gchar *author;
	gchar *date;
	gchar *keywords;
} PDFData;

static void
read_toc (PopplerIndexIter  *index,
          GString          **toc)
{
	if (!index) {
		return;
	}

	if (!*toc) {
		*toc = g_string_new ("");
	}

	do {
		PopplerAction *action;
		PopplerIndexIter *iter;

		action = poppler_index_iter_get_action (index);

		if (!action) {
			continue;
		}

		switch (action->type) {
			case POPPLER_ACTION_GOTO_DEST: {
				PopplerActionGotoDest *ag = (PopplerActionGotoDest *)action;

				if (!tracker_is_empty_string (ag->title)) {
					g_string_append_printf (*toc, "%s ", ag->title);
				}

				break;
			}

			case POPPLER_ACTION_LAUNCH: {
				PopplerActionLaunch *al = (PopplerActionLaunch *)action;

				if (!tracker_is_empty_string (al->title)) {
					g_string_append_printf (*toc, "%s ", al->title);
				}

				if (!tracker_is_empty_string (al->file_name)) {
					g_string_append_printf (*toc, "%s ", al->file_name);
				}

				if (!tracker_is_empty_string (al->params)) {
					g_string_append_printf (*toc, "%s ", al->params);
				}

				break;
			}

			case POPPLER_ACTION_URI: {
				PopplerActionUri *au = (PopplerActionUri *)action;

				if (!tracker_is_empty_string (au->uri)) {
					g_string_append_printf (*toc, "%s ", au->uri);
				}

				break;
			}

			case POPPLER_ACTION_NAMED: {
				PopplerActionNamed *an = (PopplerActionNamed *)action;

				if (!tracker_is_empty_string (an->title)) {
					g_string_append_printf (*toc, "%s, ", an->title);
				}

				if (!tracker_is_empty_string (an->named_dest)) {
					g_string_append_printf (*toc, "%s ", an->named_dest);
				}

				break;
			}

			case POPPLER_ACTION_MOVIE: {
				PopplerActionMovie *am = (PopplerActionMovie *)action;

				if (!tracker_is_empty_string (am->title)) {
					g_string_append_printf (*toc, "%s ", am->title);
				}

				break;
			}

			case POPPLER_ACTION_NONE:
			case POPPLER_ACTION_UNKNOWN:
			case POPPLER_ACTION_GOTO_REMOTE:
			case POPPLER_ACTION_RENDITION:
			case POPPLER_ACTION_OCG_STATE:
			case POPPLER_ACTION_JAVASCRIPT:
			#if POPPLER_CHECK_VERSION (0, 90, 0)
  			case POPPLER_ACTION_RESET_FORM:
			#endif

				/* Do nothing */
				break;
		}

		poppler_action_free (action);
		iter = poppler_index_iter_get_child (index);
		read_toc (iter, toc);
	} while (poppler_index_iter_next (index));

	poppler_index_iter_free (index);
}

static void
read_outline (PopplerDocument *document,
              TrackerResource *metadata)
{
	PopplerIndexIter *index;
	GString *toc = NULL;

	index = poppler_index_iter_new (document);

	if (!index) {
		return;
	}

	read_toc (index, &toc);

	if (toc) {
		if (toc->len > 0) {
			tracker_resource_set_string (metadata, "nfo:tableOfContents", toc->str);
		}

		g_string_free (toc, TRUE);
	}
}

static gchar *
extract_content_text (PopplerDocument *document,
                      gsize            n_bytes)
{
	GString *string;
	gsize remaining_bytes;
	gint n_pages, i;

	n_pages = poppler_document_get_n_pages (document);
	string = g_string_new ("");

	for (i = 0, remaining_bytes = n_bytes; i < n_pages && remaining_bytes > 0; i++) {
		PopplerPage *page;
		gsize written_bytes = 0;
		gchar *text = NULL;

		page = poppler_document_get_page (document, i);
		if (page)
			text = poppler_page_get_text (page);

		if (!text) {
			if (page)
				g_object_unref (page);
			continue;
		}

		if (tracker_text_validate_utf8 (text,
		                                MIN (strlen (text), remaining_bytes),
		                                &string,
		                                &written_bytes)) {
			g_string_append_c (string, ' ');
		}

		remaining_bytes -= written_bytes;

		g_debug ("Extracted %" G_GSIZE_FORMAT " bytes from page %d, "
		         "%" G_GSIZE_FORMAT " bytes remaining",
		         written_bytes, i, remaining_bytes);

		g_free (text);
		g_object_unref (page);
	}

	g_debug ("Content extraction finished: %d/%d pages indexed, "
	         "%" G_GSIZE_FORMAT " bytes extracted",
	         i,
	         n_pages,
	         (n_bytes - remaining_bytes));

	return g_string_free (string, FALSE);
}

static void
write_pdf_data (PDFData          data,
                TrackerResource *metadata,
                GPtrArray       *keywords)
{
	if (!tracker_is_empty_string (data.title)) {
		tracker_resource_set_string (metadata, "nie:title", data.title);
	}

	if (!tracker_is_empty_string (data.subject)) {
		tracker_resource_set_string (metadata, "nie:subject", data.subject);
	}

	if (!tracker_is_empty_string (data.author)) {
		TrackerResource *author = tracker_extract_new_contact (data.author);
		tracker_resource_add_relation (metadata, "nco:creator", author);
		g_object_unref (author);
	}

	if (!tracker_is_empty_string (data.date)) {
		tracker_resource_set_string (metadata, "nie:contentCreated", data.date);
	}

	if (!tracker_is_empty_string (data.keywords)) {
		tracker_keywords_parse (keywords, data.keywords);
	}
}

G_MODULE_EXPORT gboolean
tracker_extract_get_metadata (TrackerExtractInfo  *info,
                              GError             **error)
{
	time_t creation_date;
	g_autoptr (GError) inner_error = NULL;
	g_autoptr (TrackerResource) metadata = NULL;
#ifdef HAVE_EXEMPI
	TrackerXmpData *xd = NULL;
#endif
	PDFData pd = { 0 };
	PopplerDocument *document;
	g_autofree char *uri = NULL;
	g_autofree char *xml = NULL;
	g_autofree char *content = NULL;
	g_autofree char *resource_uri = NULL;
	g_autoptr (GPtrArray) keywords = NULL;
	guint n_bytes;
	guint i;
	GFile *file;

	file = tracker_extract_info_get_file (info);
	uri = g_file_get_uri (file);
	document = poppler_document_new_from_gfile (file, NULL, NULL, &inner_error);

	if (inner_error) {
		if (inner_error->code == POPPLER_ERROR_ENCRYPTED) {
			resource_uri = tracker_extract_info_get_content_id (info, NULL);
			metadata = tracker_resource_new (resource_uri);

			tracker_resource_add_uri (metadata, "rdf:type", "nfo:PaginatedTextDocument");
			tracker_resource_set_boolean (metadata, "nfo:isContentEncrypted", TRUE);

			tracker_extract_info_set_resource (info, metadata);
			return TRUE;
		} else {
			g_propagate_error (error, g_steal_pointer (&inner_error));
			return FALSE;
		}
	}

	resource_uri = tracker_extract_info_get_content_id (info, NULL);
	metadata = tracker_resource_new (resource_uri);
	tracker_resource_add_uri (metadata, "rdf:type", "nfo:PaginatedTextDocument");

	g_object_get (document,
	              "title", &pd.title,
	              "author", &pd.author,
	              "subject", &pd.subject,
	              "keywords", &pd.keywords,
	              "metadata", &xml,
	              NULL);

	creation_date = poppler_document_get_creation_date (document);

	if (creation_date > 0) {
		g_autoptr (GDateTime) datetime = NULL;

		datetime = g_date_time_new_from_unix_local (creation_date);
		pd.date = g_date_time_format_iso8601 (datetime);
	}

	keywords = g_ptr_array_new_with_free_func ((GDestroyNotify) g_free);

#ifdef HAVE_EXEMPI
	if (xml && *xml) {
		xd = tracker_xmp_new (xml, strlen (xml), uri);
	} else {
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

	if (xd)
		tracker_xmp_apply_to_resource (metadata, xd);
#endif

	write_pdf_data (pd, metadata, keywords);

	for (i = 0; i < keywords->len; i++) {
		TrackerResource *tag;
		const gchar *p;

		p = g_ptr_array_index (keywords, i);
		tag = tracker_extract_new_tag (p);

		tracker_resource_add_relation (metadata, "nao:hasTag", tag);

		g_object_unref (tag);
	}

	tracker_resource_set_int64 (metadata, "nfo:pageCount", poppler_document_get_n_pages(document));

	n_bytes = tracker_extract_info_get_max_text (info);
	content = extract_content_text (document, n_bytes);

	if (content)
		tracker_resource_set_string (metadata, "nie:plainTextContent", content);

	read_outline (document, metadata);

	g_free (pd.keywords);
	g_free (pd.title);
	g_free (pd.subject);
	g_free (pd.author);
	g_free (pd.date);

	g_object_unref (document);

	tracker_extract_info_set_resource (info, metadata);

	return TRUE;
}
