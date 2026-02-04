/*
 * Copyright (C) 2026, Red Hat Inc.
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
 *
 * Author: Nieves Montero <nmontero@redhat.com>
 */

#include "config-miners.h"

#include <glib.h>
#include <gio/gio.h>
#include <zip.h>

#include "tracker-zip-input-stream.h"

struct _TrackerZipInputStream {
	GInputStream parent_instance;

	zip_t *zip;
	zip_file_t *zfile;

	zip_uint64_t size;
	zip_uint64_t pos;

	gboolean closed;
};

G_DEFINE_TYPE (TrackerZipInputStream, tracker_zip_input_stream, G_TYPE_INPUT_STREAM)

static gssize
tracker_zip_input_stream_read (GInputStream  *stream,
                               void          *buffer,
                               gsize          count,
                               GCancellable  *cancellable,
                               GError       **error)
{
	TrackerZipInputStream *self;
	gsize to_read;
	zip_int64_t n;
	zip_error_t *ze;
	const char *msg;

	self = TRACKER_ZIP_INPUT_STREAM (stream);
	to_read = 0;
	n = 0;
	ze = NULL;
	msg = NULL;

	if (count == 0)
		return 0;

	if (self->closed) {
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Stream is closed");
		return -1;
	}

	if (cancellable && g_cancellable_is_cancelled (cancellable)) {
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Operation cancelled");
		return -1;
	}

	if (self->pos >= self->size) {
		return 0; /* EOF */
	}

	to_read = (gsize) MIN ((zip_uint64_t) count, self->size - self->pos);
	n = zip_fread (self->zfile, buffer, to_read);

	if (n > 0) {
		self->pos += (zip_uint64_t) n;
		return (gssize) n;
	}

	if (n == 0) {
		self->pos = self->size; /* EOF */
		return 0;
	}

	/* n < 0 => error */
	ze = zip_file_get_error (self->zfile);
	msg = ze ? zip_error_strerror (ze) : "Unknown libzip error";

	g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "libzip read failed: %s", msg);
	return -1;
}

static gboolean
tracker_zip_input_stream_close (GInputStream  *stream,
                                GCancellable  *cancellable,
                                GError       **error)
{
	TrackerZipInputStream *self = TRACKER_ZIP_INPUT_STREAM (stream);

	if (!self->closed) {
		if (self->zfile) {
			zip_fclose (self->zfile);
			self->zfile = NULL;
		}

		if (self->zip) {
			zip_close (self->zip);
			self->zip = NULL;
		}

		self->closed = TRUE;
	}

	return TRUE;
}

static void
tracker_zip_input_stream_finalize (GObject *object)
{
	g_input_stream_close (G_INPUT_STREAM (object), NULL, NULL);
	G_OBJECT_CLASS (tracker_zip_input_stream_parent_class)->finalize (object);
}

static void
tracker_zip_input_stream_class_init (TrackerZipInputStreamClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GInputStreamClass *istream_class = G_INPUT_STREAM_CLASS (klass);

	object_class->finalize = tracker_zip_input_stream_finalize;
	istream_class->read_fn = tracker_zip_input_stream_read;
	istream_class->close_fn = tracker_zip_input_stream_close;
}

static void
tracker_zip_input_stream_init (TrackerZipInputStream *self)
{
}

GInputStream *
tracker_zip_read_file (const gchar   *zip_file_uri,
                       const gchar   *member_name,
                       GCancellable  *cancellable,
                       GError       **error)
{
	g_autofree gchar *filename = NULL;
	zip_t *zip = NULL;
	zip_file_t *zfile = NULL;
	zip_stat_t st;
	int errcode = 0;
	zip_error_t *ze = NULL;
	const char *msg = NULL;
	TrackerZipInputStream *self = NULL;

	g_return_val_if_fail (zip_file_uri != NULL, NULL);
	g_return_val_if_fail (member_name != NULL, NULL);

	filename = g_filename_from_uri (zip_file_uri, NULL, error);
	if (!filename)
		return NULL;

	zip = zip_open (filename, 0, &errcode);
	if (!zip) {
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		             "Failed to open zip '%s' (libzip errcode=%d)", filename, errcode);
		return NULL;
	}

	zip_stat_init (&st);
	if (zip_stat (zip, member_name, 0, &st) != 0) {
		zip_close (zip);
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
		             "No member '%s' in zip '%s'", member_name, filename);
		return NULL;
	}

	zfile = zip_fopen (zip, member_name, 0);
	if (!zfile) {
		ze = zip_get_error (zip);
		msg = ze ? zip_error_strerror (ze) : "Unknown libzip error";
		zip_close (zip);
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		             "Failed to open member '%s' in zip '%s': %s", member_name, filename, msg);
		return NULL;
	}

	self = g_object_new (TRACKER_TYPE_ZIP_INPUT_STREAM, NULL);
	self->zip = zip;
	self->zfile = zfile;
	self->size = st.size;
	self->pos = 0;

	return G_INPUT_STREAM (self);
}
