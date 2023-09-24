/*
 * Copyright (C) 2014 - Collabora Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config-miners.h"

#include "tracker-extract-controller.h"

#include "tracker-main.h"

enum {
	PROP_DECORATOR = 1,
	PROP_CONNECTION,
};

struct TrackerExtractControllerPrivate {
	TrackerDecorator *decorator;
	GCancellable *cancellable;
	GDBusConnection *connection;
	guint object_id;
	gint paused;
};

#define OBJECT_PATH "/org/freedesktop/Tracker3/Extract"

static const gchar *introspection_xml =
	"<node>"
	"  <interface name='org.freedesktop.Tracker3.Extract'>"
	"    <signal name='Error'>"
	"      <arg type='a{sv}' name='data' direction='out' />"
	"    </signal>"
	"  </interface>"
	"</node>";

G_DEFINE_TYPE_WITH_PRIVATE (TrackerExtractController, tracker_extract_controller, G_TYPE_OBJECT)

static void
decorator_raise_error_cb (TrackerDecorator         *decorator,
                          GFile                    *file,
                          gchar                    *msg,
                          gchar                    *extra,
                          TrackerExtractController *controller)
{
	TrackerExtractControllerPrivate *priv =
		tracker_extract_controller_get_instance_private (controller);
	g_autoptr (GError) error = NULL;
	g_autofree gchar *uri = NULL;
	GVariantBuilder builder;

	uri = g_file_get_uri (file);

	g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
	g_variant_builder_add (&builder, "{sv}", "uri",
	                       g_variant_new_string (uri));
	g_variant_builder_add (&builder, "{sv}", "message",
	                       g_variant_new_string (msg));

	if (extra) {
		g_variant_builder_add (&builder, "{sv}", "extra-info",
		                       g_variant_new_string (extra));
	}

	g_dbus_connection_emit_signal (priv->connection,
	                               NULL,
	                               OBJECT_PATH,
	                               "org.freedesktop.Tracker3.Extract",
	                               "Error",
	                               g_variant_new ("(@a{sv})", g_variant_builder_end (&builder)),
	                               &error);

	if (error)
		g_warning ("Could not emit signal: %s\n", error->message);
}

static void
tracker_extract_controller_constructed (GObject *object)
{
	TrackerExtractController *self = (TrackerExtractController *) object;
	g_autoptr (GDBusNodeInfo) introspection_data = NULL;
	GDBusInterfaceVTable interface_vtable = {
		NULL, NULL, NULL
	};

	G_OBJECT_CLASS (tracker_extract_controller_parent_class)->constructed (object);

	g_assert (self->priv->decorator != NULL);

	g_signal_connect (self->priv->decorator, "raise-error",
	                  G_CALLBACK (decorator_raise_error_cb), object);

	introspection_data = g_dbus_node_info_new_for_xml (introspection_xml, NULL);
	g_assert (introspection_data);
	self->priv->object_id =
		g_dbus_connection_register_object (self->priv->connection,
						   OBJECT_PATH,
		                                   introspection_data->interfaces[0],
		                                   &interface_vtable,
		                                   object,
		                                   NULL, NULL);
}

static void
tracker_extract_controller_get_property (GObject    *object,
                                         guint       param_id,
                                         GValue     *value,
                                         GParamSpec *pspec)
{
	TrackerExtractController *self = (TrackerExtractController *) object;

	switch (param_id) {
	case PROP_DECORATOR:
		g_value_set_object (value, self->priv->decorator);
		break;
	case PROP_CONNECTION:
		g_value_set_object (value, self->priv->connection);
		break;
	}
}

static void
tracker_extract_controller_set_property (GObject      *object,
                                        guint         param_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
	TrackerExtractController *self = (TrackerExtractController *) object;

	switch (param_id) {
	case PROP_DECORATOR:
		g_assert (self->priv->decorator == NULL);
		self->priv->decorator = g_value_dup_object (value);
		break;
	case PROP_CONNECTION:
		self->priv->connection = g_value_dup_object (value);
		break;
	}
}

static void
tracker_extract_controller_dispose (GObject *object)
{
	TrackerExtractController *self = (TrackerExtractController *) object;

	if (self->priv->connection && self->priv->object_id) {
		g_dbus_connection_unregister_object (self->priv->connection, self->priv->object_id);
		self->priv->object_id = 0;
	}

	g_clear_object (&self->priv->decorator);

	G_OBJECT_CLASS (tracker_extract_controller_parent_class)->dispose (object);
}

static void
tracker_extract_controller_class_init (TrackerExtractControllerClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->constructed = tracker_extract_controller_constructed;
	object_class->dispose = tracker_extract_controller_dispose;
	object_class->get_property = tracker_extract_controller_get_property;
	object_class->set_property = tracker_extract_controller_set_property;

	g_object_class_install_property (object_class,
	                                 PROP_DECORATOR,
	                                 g_param_spec_object ("decorator",
	                                                      "Decorator",
	                                                      "Decorator",
	                                                      TRACKER_TYPE_DECORATOR,
	                                                      G_PARAM_STATIC_STRINGS |
	                                                      G_PARAM_READWRITE |
	                                                      G_PARAM_CONSTRUCT_ONLY));
	g_object_class_install_property (object_class,
	                                 PROP_CONNECTION,
	                                 g_param_spec_object ("connection",
	                                                      "Connection",
	                                                      "Connection",
	                                                      G_TYPE_DBUS_CONNECTION,
	                                                      G_PARAM_STATIC_STRINGS |
	                                                      G_PARAM_READWRITE |
	                                                      G_PARAM_CONSTRUCT_ONLY));
}

static void
tracker_extract_controller_init (TrackerExtractController *self)
{
	self->priv = tracker_extract_controller_get_instance_private (self);
}

TrackerExtractController *
tracker_extract_controller_new (TrackerDecorator *decorator,
                                GDBusConnection  *connection)
{
	g_return_val_if_fail (TRACKER_IS_DECORATOR (decorator), NULL);

	return g_object_new (TRACKER_TYPE_EXTRACT_CONTROLLER,
	                     "decorator", decorator,
	                     "connection", connection,
	                     NULL);
}
