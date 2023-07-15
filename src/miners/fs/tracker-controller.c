#include "config-miners.h"

#include "tracker-controller.h"

#include "tracker-config.h"

#include <libtracker-miners-common/tracker-common.h>

struct _TrackerController
{
	GObject parent_instance;

	TrackerIndexingTree *indexing_tree;
	TrackerStorage *storage;
};

enum {
	PROP_0,
	PROP_INDEXING_TREE,
	PROP_STORAGE,
	N_PROPS,
};

static GParamSpec *props[N_PROPS] = { 0, };

G_DEFINE_TYPE (TrackerController, tracker_controller, G_TYPE_OBJECT)

static void
tracker_controller_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
	TrackerController *controller = TRACKER_CONTROLLER (object);

	switch (prop_id) {
	case PROP_INDEXING_TREE:
		controller->indexing_tree = g_value_dup_object (value);
		break;
	case PROP_STORAGE:
		controller->storage = g_value_dup_object (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
tracker_controller_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
	TrackerController *controller = TRACKER_CONTROLLER (object);

	switch (prop_id) {
	case PROP_INDEXING_TREE:
		g_value_set_object (value, controller->indexing_tree);
		break;
	case PROP_STORAGE:
		g_value_set_object (value, controller->storage);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
tracker_controller_finalize (GObject *object)
{
	TrackerController *controller = TRACKER_CONTROLLER (object);

	g_clear_object (&controller->indexing_tree);
	g_clear_object (&controller->storage);

	G_OBJECT_CLASS (tracker_controller_parent_class)->finalize (object);
}

static void
tracker_controller_class_init (TrackerControllerClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->set_property = tracker_controller_set_property;
	object_class->get_property = tracker_controller_get_property;
	object_class->finalize = tracker_controller_finalize;

	props[PROP_INDEXING_TREE] =
		g_param_spec_object ("indexing-tree",
		                     "Indexing tree",
		                     "Indexing tree",
		                     TRACKER_TYPE_INDEXING_TREE,
		                     G_PARAM_READWRITE |
		                     G_PARAM_CONSTRUCT_ONLY |
		                     G_PARAM_STATIC_STRINGS);
	props[PROP_STORAGE] =
		g_param_spec_object ("storage",
		                     "Storage",
		                     "Storage",
		                     TRACKER_TYPE_STORAGE,
		                     G_PARAM_READWRITE |
		                     G_PARAM_CONSTRUCT_ONLY |
		                     G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
tracker_controller_init (TrackerController *controller)
{
}

TrackerController *
tracker_controller_new (TrackerIndexingTree *tree,
                        TrackerStorage      *storage)
{
	return g_object_new (TRACKER_TYPE_CONTROLLER,
			     "indexing-tree", tree,
	                     "storage", storage,
			     NULL);
}
