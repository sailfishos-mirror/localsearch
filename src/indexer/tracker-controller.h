#ifndef __TRACKER_CONTROLLER_H__
#define __TRACKER_CONTROLLER_H__

#include <glib-object.h>

#include "tracker-indexing-tree.h"
#include "tracker-monitor.h"
#include "tracker-files-interface.h"

G_BEGIN_DECLS

#define TRACKER_TYPE_CONTROLLER (tracker_controller_get_type ())
G_DECLARE_FINAL_TYPE (TrackerController, tracker_controller, TRACKER, CONTROLLER, GObject)

TrackerController * tracker_controller_new (TrackerIndexingTree   *tree,
                                            TrackerMonitor        *monitor,
                                            TrackerFilesInterface *files_interface);

void tracker_controller_initialize_removable_devices (TrackerController *controller);

void tracker_controller_register_indexing_tree (TrackerController   *controller,
						TrackerIndexingTree *indexing_tree);

void tracker_controller_unregister_indexing_tree (TrackerController   *controller,
						  TrackerIndexingTree *indexing_tree);

G_END_DECLS

#endif /* __TRACKER_CONTROLLER_H__ */
