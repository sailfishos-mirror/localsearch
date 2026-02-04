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

#ifndef TRACKER_ZIP_INPUT_STREAM_H
#define TRACKER_ZIP_INPUT_STREAM_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define TRACKER_TYPE_ZIP_INPUT_STREAM (tracker_zip_input_stream_get_type ())
G_DECLARE_FINAL_TYPE (TrackerZipInputStream,
                      tracker_zip_input_stream,
                      TRACKER,
                      ZIP_INPUT_STREAM,
                      GInputStream)

GInputStream * tracker_zip_read_file (const gchar   *zip_file_uri,
                                      const gchar   *member_name,
                                      GCancellable  *cancellable,
                                      GError       **error);

G_END_DECLS

#endif /* TRACKER_ZIP_INPUT_STREAM_H */
