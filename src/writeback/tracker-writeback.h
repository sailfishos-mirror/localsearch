/*
 * Copyright (C) 2011, Nokia <ivan.frade@nokia.com>
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

#ifndef __TRACKER_WRITEBACK_CONTROLLER_H__
#define __TRACKER_WRITEBACK_CONTROLLER_H__

#include <gio/gio.h>

G_BEGIN_DECLS

#define TRACKER_TYPE_CONTROLLER (tracker_controller_get_type ())
G_DECLARE_FINAL_TYPE (TrackerController,
                      tracker_controller,
                      TRACKER, CONTROLLER,
                      GObject)

TrackerController * tracker_controller_new (guint    shutdown_timeout,
                                            GError **error);

G_END_DECLS

#endif /* __TRACKER_WRITEBACK_CONTROLLER_H__ */
