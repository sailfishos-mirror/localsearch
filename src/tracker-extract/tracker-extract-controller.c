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

typedef struct _TrackerExtractControllerPrivate TrackerExtractControllerPrivate;

struct _TrackerExtractControllerPrivate {
	TrackerDecorator *decorator;
	GCancellable *cancellable;
	GDBusConnection *connection;
	GDBusProxy *index_proxy;
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

static void tracker_extract_controller_initable_iface_init (GInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE (TrackerExtractController,
                         tracker_extract_controller,
                         G_TYPE_OBJECT,
                         G_ADD_PRIVATE (TrackerExtractController)
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, tracker_extract_controller_initable_iface_init))

static void
update_graphs_from_proxy (TrackerExtractController *controller,
                          GDBusProxy               *proxy)
{
	TrackerExtractControllerPrivate *priv;
	const gchar **graphs = NULL;
	GVariant *v;

	priv = tracker_extract_controller_get_instance_private (controller);

	v = g_dbus_proxy_get_cached_property (proxy, "Graphs");
	if (v)
		graphs = g_variant_get_strv (v, NULL);

	tracker_decorator_set_priority_graphs (priv->decorator, graphs);
	g_free (graphs);
}

static void
proxy_properties_changed_cb (GDBusProxy *proxy,
                             GVariant   *changed_properties,
                             GStrv       invalidated_properties,
                             gpointer    user_data)
{
	update_graphs_from_proxy (user_data, proxy);
}

static gboolean
tracker_extract_controller_initable_init (GInitable     *initable,
                                          GCancellable  *cancellable,
                                          GError       **error)
{
	TrackerExtractController *controller;
	TrackerExtractControllerPrivate *priv;
	g_autoptr (GDBusNodeInfo) introspection_data = NULL;
	GDBusConnection *conn;
	GDBusInterfaceVTable interface_vtable = {
		NULL, NULL, NULL
	};

	controller = TRACKER_EXTRACT_CONTROLLER (initable);
	priv = tracker_extract_controller_get_instance_private (controller);
	conn = priv->connection;

	priv->index_proxy = g_dbus_proxy_new_sync (conn,
	                                           G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
	                                           NULL,
	                                           "org.freedesktop.Tracker3.Miner.Files.Control",
	                                           "/org/freedesktop/Tracker3/Miner/Files/Proxy",
	                                           "org.freedesktop.Tracker3.Miner.Files.Proxy",
	                                           NULL,
	                                           error);
	if (!priv->index_proxy)
		return FALSE;

	g_signal_connect (priv->index_proxy, "g-properties-changed",
	                  G_CALLBACK (proxy_properties_changed_cb), controller);
	update_graphs_from_proxy (controller, priv->index_proxy);

	introspection_data = g_dbus_node_info_new_for_xml (introspection_xml, error);
	if (!introspection_data)
		return FALSE;

	priv->object_id =
		g_dbus_connection_register_object (priv->connection,
						   OBJECT_PATH,
		                                   introspection_data->interfaces[0],
		                                   &interface_vtable,
		                                   initable,
		                                   NULL, error);
	if (priv->object_id == 0)
		return FALSE;

	return TRUE;
}

static void
tracker_extract_controller_initable_iface_init (GInitableIface *iface)
{
	iface->init = tracker_extract_controller_initable_init;
}

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
	TrackerExtractControllerPrivate *priv;

	priv = tracker_extract_controller_get_instance_private (self);

	G_OBJECT_CLASS (tracker_extract_controller_parent_class)->constructed (object);

	g_assert (priv->decorator != NULL);

	g_signal_connect (priv->decorator, "raise-error",
	                  G_CALLBACK (decorator_raise_error_cb), object);
}

static void
tracker_extract_controller_get_property (GObject    *object,
                                         guint       param_id,
                                         GValue     *value,
                                         GParamSpec *pspec)
{
	TrackerExtractController *self = (TrackerExtractController *) object;
	TrackerExtractControllerPrivate *priv;

	priv = tracker_extract_controller_get_instance_private (self);

	switch (param_id) {
	case PROP_DECORATOR:
		g_value_set_object (value, priv->decorator);
		break;
	case PROP_CONNECTION:
		g_value_set_object (value, priv->connection);
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
	TrackerExtractControllerPrivate *priv;

	priv = tracker_extract_controller_get_instance_private (self);

	switch (param_id) {
	case PROP_DECORATOR:
		g_assert (priv->decorator == NULL);
		priv->decorator = g_value_dup_object (value);
		break;
	case PROP_CONNECTION:
		priv->connection = g_value_dup_object (value);
		break;
	}
}

static void
tracker_extract_controller_dispose (GObject *object)
{
	TrackerExtractController *self = (TrackerExtractController *) object;
	TrackerExtractControllerPrivate *priv;

	priv = tracker_extract_controller_get_instance_private (self);

	if (priv->connection && priv->object_id) {
		g_dbus_connection_unregister_object (priv->connection, priv->object_id);
		priv->object_id = 0;
	}

	g_clear_object (&priv->decorator);
	g_clear_object (&priv->index_proxy);

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
}

TrackerExtractController *
tracker_extract_controller_new (TrackerDecorator  *decorator,
                                GDBusConnection   *connection,
                                GError           **error)
{
	g_return_val_if_fail (TRACKER_IS_DECORATOR (decorator), NULL);

	return g_initable_new (TRACKER_TYPE_EXTRACT_CONTROLLER,
	                       NULL, error,
	                       "decorator", decorator,
	                       "connection", connection,
	                       NULL);
}
