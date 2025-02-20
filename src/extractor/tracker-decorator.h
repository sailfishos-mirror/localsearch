/*
 * Copyright (C) 2014 Carlos Garnacho  <carlosg@gnome.org>
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

#ifndef __LIBTRACKER_MINER_DECORATOR_H__
#define __LIBTRACKER_MINER_DECORATOR_H__

#include <tracker-common.h>

#include "utils/tracker-extract.h"

G_BEGIN_DECLS

#define TRACKER_TYPE_DECORATOR (tracker_decorator_get_type())
G_DECLARE_DERIVABLE_TYPE (TrackerDecorator,
                          tracker_decorator,
                          TRACKER, DECORATOR,
                          TrackerMiner)

typedef struct _TrackerDecoratorInfo TrackerDecoratorInfo;

struct _TrackerDecoratorClass {
	TrackerMinerClass parent_class;

	void (* items_available) (TrackerDecorator *decorator);
	void (* finished)        (TrackerDecorator *decorator);

	void (* error) (TrackerDecorator *decorator,
	                GFile            *file,
	                const gchar      *error_message,
	                const gchar      *extra_info);

	void (* update) (TrackerDecorator   *decorator,
	                 TrackerExtractInfo *extract_info);
};

#define TRACKER_DECORATOR_ERROR (tracker_decorator_error_quark ())

typedef enum {
	TRACKER_DECORATOR_ERROR_PAUSED
} TrackerDecoratorError;

GQuark        tracker_decorator_error_quark       (void);

guint         tracker_decorator_get_n_items       (TrackerDecorator     *decorator);

TrackerDecoratorInfo * tracker_decorator_next (TrackerDecorator  *decorator,
                                               GError           **error);

void tracker_decorator_raise_error (TrackerDecorator *decorator,
                                    GFile            *file,
                                    const char       *message,
                                    const char       *extra_info);

void          tracker_decorator_set_priority_graphs (TrackerDecorator    *decorator,
                                                     const gchar * const *graphs);

TrackerBatch * tracker_decorator_get_batch (TrackerDecorator *decorator);

void tracker_decorator_invalidate_cache (TrackerDecorator *decorator);

GType         tracker_decorator_info_get_type     (void) G_GNUC_CONST;

TrackerDecoratorInfo *
              tracker_decorator_info_ref          (TrackerDecoratorInfo *info);
void          tracker_decorator_info_unref        (TrackerDecoratorInfo *info);
const gchar * tracker_decorator_info_get_url      (TrackerDecoratorInfo *info);
const gchar * tracker_decorator_info_get_content_id (TrackerDecoratorInfo *info);
const gchar * tracker_decorator_info_get_mime_type (TrackerDecoratorInfo *info);
GCancellable * tracker_decorator_info_get_cancellable (TrackerDecoratorInfo *info);
void          tracker_decorator_info_complete     (TrackerDecoratorInfo *info,
                                                   TrackerExtractInfo   *extract_info);
void          tracker_decorator_info_complete_error (TrackerDecoratorInfo *info,
                                                     GError               *error);

G_END_DECLS

#endif /* __LIBTRACKER_MINER_DECORATOR_H__ */
