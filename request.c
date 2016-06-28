/*
 * Copyright © 2016 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "request.h"

#include <string.h>

static void request_skeleton_iface_init (XdpRequestIface *iface);

G_DEFINE_TYPE_WITH_CODE (Request, request, XDP_TYPE_REQUEST_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_REQUEST, request_skeleton_iface_init))

static void
request_on_signal_response (XdpRequest *object,
                            guint arg_response,
                            GVariant *arg_results)
{
  Request *request = (Request *)object;
  XdpRequestSkeleton *skeleton = XDP_REQUEST_SKELETON (object);
  GList      *connections, *l;
  GVariant   *signal_variant;

  connections = g_dbus_interface_skeleton_get_connections (G_DBUS_INTERFACE_SKELETON (skeleton));

  signal_variant = g_variant_ref_sink (g_variant_new ("(u@a{sv})",
                                                      arg_response,
                                                      arg_results));
  for (l = connections; l != NULL; l = l->next)
    {
      GDBusConnection *connection = l->data;
      g_dbus_connection_emit_signal (connection,
                                     request->sender,
                                     g_dbus_interface_skeleton_get_object_path (G_DBUS_INTERFACE_SKELETON (skeleton)),
                                     "org.freedesktop.impl.portal.Request",
                                     "Response",
                                     signal_variant,
                                     NULL);
    }
  g_variant_unref (signal_variant);
  g_list_free_full (connections, g_object_unref);
}

static gboolean
handle_close (XdpRequest *object,
              GDBusMethodInvocation *invocation)
{
  Request *request = (Request *)object;
  g_autoptr(GError) error = NULL;

  if (request->exported)
    request_unexport (request);

  xdp_request_complete_close (XDP_REQUEST (request), invocation);

  return TRUE;
}

static void
request_skeleton_iface_init (XdpRequestIface *iface)
{
  iface->handle_close = handle_close;
  iface->response = request_on_signal_response;
}

static void
request_init (Request *request)
{
}

static void
request_finalize (GObject *object)
{
  Request *request = (Request *)object;

  g_free (request->app_id);
  g_free (request->sender);
  g_free (request->id);

  G_OBJECT_CLASS (request_parent_class)->finalize (object);
}

static void
request_class_init (RequestClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize  = request_finalize;
}

Request *
request_from_invocation (GDBusMethodInvocation *invocation,
                         const char *app_id,
                         const char *id)
{
  Request *request;

  request = g_object_new (request_get_type (), NULL);
  request->app_id = g_strdup (app_id);
  request->sender = g_strdup (g_dbus_method_invocation_get_sender (invocation));
  request->id = g_strdup (id);

  g_object_set_data_full (G_OBJECT (invocation), "request", request, g_object_unref);

  return request;
}

void
request_export (Request *request,
                GDBusConnection *connection)
{
  g_autoptr(GError) error = NULL;

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (request),
                                         connection,
                                         request->id,
                                         &error))
    {
      g_warning ("error exporting request: %s\n", error->message);
      g_clear_error (&error);
    }

  g_object_ref (request);
  request->exported = TRUE;
}

void
request_unexport (Request *request)
{
  request->exported = FALSE;
  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (request));
  g_object_unref (request);
}
