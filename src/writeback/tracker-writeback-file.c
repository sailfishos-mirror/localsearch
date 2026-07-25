/*
 * Copyright (C) 2009, Nokia <ivan.frade@nokia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config-miners.h"

#include <stdio.h>
#include <fcntl.h> /* O_WRONLY */

#include <gio/gunixoutputstream.h>
#include <glib/gstdio.h>

#include <tracker-common.h>

#include "tracker-writeback-file.h"

static gboolean tracker_writeback_file_write_metadata (TrackerWriteback         *writeback,
                                                       TrackerResource          *resource,
                                                       GCancellable             *cancellable,
                                                       GError                  **error);

G_DEFINE_ABSTRACT_TYPE (TrackerWritebackFile, tracker_writeback_file, TRACKER_TYPE_WRITEBACK)

static void
tracker_writeback_file_class_init (TrackerWritebackFileClass *klass)
{
	TrackerWritebackClass *writeback_class = TRACKER_WRITEBACK_CLASS (klass);

	writeback_class->write_metadata = tracker_writeback_file_write_metadata;
}

static void
tracker_writeback_file_init (TrackerWritebackFile *writeback_file)
{
}

static GFile *
create_temporary_file (GFile      *file,
                       GFileInfo  *file_info,
                       GError    **error)
{
	g_autoptr (GInputStream) input_stream = NULL;
	g_autoptr (GOutputStream) output_stream = NULL;
	g_autoptr (GFile) tmp_file = NULL, parent = NULL;
	g_autofree char *dir = NULL, *name = NULL, *tmp_path = NULL;
	g_autofd int fd = -1;
	mode_t mode;

	if (!g_file_is_native (file)) {
		g_autofree char *uri = NULL;

		uri = g_file_get_uri (file);
		g_set_error (error,
		             G_IO_ERROR,
		             G_IO_ERROR_FAILED,
		             "Not writing back on non-native file '%s'",
		             uri);
		return NULL;
	}

	/* Create input stream */
	input_stream = G_INPUT_STREAM (g_file_read (file, NULL, error));
	if (!input_stream)
		return NULL;

	/* Create output stream in a tmp file */
	parent = g_file_get_parent (file);
	dir = g_file_get_path (parent);

	name = g_file_get_basename (file);
	tmp_path = g_strdup_printf ("%s" G_DIR_SEPARATOR_S ".tracker-XXXXXX.%s",
	                            dir, name);

	mode = g_file_info_get_attribute_uint32 (file_info,
	                                         G_FILE_ATTRIBUTE_UNIX_MODE);
	fd = g_mkstemp_full (tmp_path, O_WRONLY, mode);

	if (fd < 0) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_FAILED,
			     "Could not create temporary file: %m");
		return NULL;
	}

	output_stream = g_unix_output_stream_new (g_steal_fd (&fd), TRUE);
	tmp_file = g_file_new_for_path (tmp_path);

	/* Splice the original file into the tmp file */
	if (g_output_stream_splice (output_stream,
				    input_stream,
				    G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE |
				    G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
				    NULL, error) < 0) {
		g_file_delete (tmp_file, NULL, NULL);
		return NULL;
	}

	return g_steal_pointer (&tmp_file);
}

static gboolean
tracker_writeback_file_write_metadata (TrackerWriteback  *writeback,
                                       TrackerResource   *resource,
                                       GCancellable      *cancellable,
                                       GError           **error)
{
	TrackerWritebackFileClass *writeback_file_class;
	g_autoptr (GFile) file = NULL, tmp_file = NULL;
	g_autoptr (GFileInfo) file_info = NULL;
	g_autoptr (GList) values = NULL;
	g_autoptr (GError) n_error = NULL;
	const gchar * const *content_types = NULL;
	const gchar *mime_type = NULL, *url = NULL;
	gboolean retval;

	writeback_file_class = TRACKER_WRITEBACK_FILE_GET_CLASS (writeback);

	if (!writeback_file_class->write_file_metadata) {
		g_set_error (error,
		             G_IO_ERROR,
		             G_IO_ERROR_FAILED,
		             "%s doesn't implement write_file_metadata()",
		             G_OBJECT_TYPE_NAME (writeback));

		return FALSE;
	}

	if (!writeback_file_class->content_types) {
		g_critical ("%s doesn't implement content_types()",
		            G_OBJECT_TYPE_NAME (writeback));

		g_set_error (error,
		             G_IO_ERROR,
		             G_IO_ERROR_FAILED,
		             "%s doesn't implement content_types()",
		             G_OBJECT_TYPE_NAME (writeback));

		return FALSE;
	}

	/* Get the file from the resource */
	values = tracker_resource_get_values (resource, "nie:isStoredAs");

	if (values) {
		if (G_VALUE_HOLDS_STRING (values->data)) {
			url = g_value_get_string (values->data);
		} else if (G_VALUE_HOLDS_OBJECT (values->data)) {
			TrackerResource *file_resource;

			file_resource = g_value_get_object (values->data);
			url = tracker_resource_get_identifier (file_resource);
		}
	}

	if (!url) {
		g_set_error (error,
		             G_IO_ERROR,
		             G_IO_ERROR_INVALID_DATA,
		             "RDF does not contain nie:isStoredAs");
		return FALSE;
	}

	file = g_file_new_for_uri (url);

	file_info = g_file_query_info (file,
	                               G_FILE_ATTRIBUTE_UNIX_MODE ","
	                               G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE ","
	                               G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE,
	                               G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
	                               NULL, NULL);

	if (!file_info) {
		g_set_error (error,
		             G_IO_ERROR,
		             G_IO_ERROR_FAILED,
		             "%s doesn't exist",
		             url);

		return FALSE;
	}

	if (!g_file_info_get_attribute_boolean (file_info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE)) {
		g_set_error (error,
		             G_IO_ERROR,
		             G_IO_ERROR_FAILED,
		             "%s not writable",
		             url);

		return FALSE;
	}

	mime_type = g_file_info_get_content_type (file_info);
	content_types = (writeback_file_class->content_types) (TRACKER_WRITEBACK_FILE (writeback));

	if (!g_strv_contains (content_types, mime_type)) {
		g_set_error (error,
		             G_DBUS_ERROR,
		             G_DBUS_ERROR_NOT_SUPPORTED,
		             "Module does not support writeback for %s",
		             mime_type);
		return FALSE;
	}

	/* Copy to a temporary file so we can perform an atomic write on move */
	tmp_file = create_temporary_file (file, file_info, error);
	if (!tmp_file)
		return FALSE;

	retval = (writeback_file_class->write_file_metadata) (TRACKER_WRITEBACK_FILE (writeback),
	                                                      tmp_file,
	                                                      resource,
	                                                      cancellable,
	                                                      error);

	if (!retval) {
		g_autoptr (GError) inner_error = NULL;

		/* Delete the temporary file and preserve original */
		if (!g_file_delete (tmp_file, NULL, &inner_error) &&
		    !g_error_matches (inner_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
			g_warning ("Failed to delete temporary file: %s", inner_error->message);
		}

		return FALSE;
	} else {
		/* Move back the modified file to the original location. Correct UNIX
		 * mode has been set for tmp_file in create_temporary_file() already.
		 */
		return g_file_move (tmp_file, file,
				    G_FILE_COPY_OVERWRITE,
				    NULL, NULL, NULL, error);
	}
}
