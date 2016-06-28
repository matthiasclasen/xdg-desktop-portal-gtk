#define _GNU_SOURCE 1

#include "config.h"

#include "appchooser.h"
#include "request.h"

#include <string.h>

#include <gtk/gtk.h>

#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <gio/gunixfdlist.h>

#include "xdg-desktop-portal-dbus.h"

#include "appchooserdialog.h"

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

typedef struct {
  Request *request;
  GtkWidget *dialog;

  char *chosen;
  int response;

} AppDialogHandle;

static void
app_dialog_handle_free (gpointer data)
{
  AppDialogHandle *handle = data;

  g_object_unref (handle->dialog);
  g_free (handle->chosen);

  g_free (handle);
}

static void
app_dialog_handle_close (AppDialogHandle *handle)
{
  gtk_widget_destroy (handle->dialog);
  app_dialog_handle_free (handle);
}

static void
send_response (AppDialogHandle *handle)
{
  GVariantBuilder opt_builder;

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&opt_builder, "{sv}", "chosen", g_variant_new_string (handle->chosen));

  if (handle->request->exported)
    {
      xdp_request_emit_response (XDP_REQUEST (handle->request),
                                 handle->response,
                                 g_variant_builder_end (&opt_builder));
      request_unexport (handle->request);
    }

  app_dialog_handle_close (handle);
}

static void
handle_app_chooser_done (GtkDialog *dialog,
                         GAppInfo *info,
                         gpointer data)
{
  AppDialogHandle *handle = data;

  if (info != NULL)
    {
      handle->response = 0;
      handle->chosen = g_strdup (g_app_info_get_id (info));
    }
  else
    {
      handle->response = 1;
      handle->chosen = NULL;
    }

  send_response (handle);
}

static gboolean
handle_close (XdpRequest *object,
              GDBusMethodInvocation *invocation,
              AppDialogHandle *handle)
{
  app_dialog_handle_close (handle);
  return FALSE;
}

static gboolean
handle_choose_application (XdpAppChooser *object,
                           GDBusMethodInvocation *invocation,
                           const char *arg_handle,
                           const char *arg_app_id,
                           const char *arg_parent_window,
                           const char **choices,
                           GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation, arg_app_id, arg_handle);
  GtkWidget *dialog;
  AppDialogHandle *handle;
  const char *cancel_label;
  const char *accept_label;
  const char *title;
  const char *heading;
  const char *latest_chosen_id;

  if (!g_variant_lookup (arg_options, "accept_label", "&s", &accept_label))
    accept_label = "_Select";
  if (!g_variant_lookup (arg_options, "title", "&s", &title))
    title = "Open With";
  if (!g_variant_lookup (arg_options, "heading", "&s", &heading))
    heading = "Select application";
  if (!g_variant_lookup (arg_options, "latest-choice", "&s", &latest_chosen_id))
    latest_chosen_id = NULL;

  dialog = GTK_WIDGET (app_chooser_dialog_new (choices,
                                               latest_chosen_id,
                                               cancel_label,
                                               accept_label,
                                               title,
                                               heading));

  handle = g_new0 (AppDialogHandle, 1);
  handle->request = request;
  handle->dialog = g_object_ref (dialog);

  g_signal_connect (request, "handle-close",
                    G_CALLBACK (handle_close), handle);

  g_signal_connect (dialog, "done",
                    G_CALLBACK (handle_app_chooser_done), handle);

  gtk_window_present (GTK_WINDOW (dialog));

  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_app_chooser_complete_choose_application (object, invocation);

  return TRUE;
}

gboolean
app_chooser_init (GDBusConnection *bus,
                  GError **error)
{
  GDBusInterfaceSkeleton *helper;

  helper = G_DBUS_INTERFACE_SKELETON (xdp_app_chooser_skeleton_new ());

  g_signal_connect (helper, "handle-choose-application", G_CALLBACK (handle_choose_application), NULL);

  if (!g_dbus_interface_skeleton_export (helper,
                                         bus,
                                         "/org/freedesktop/portal/desktop",
                                         error))
    return FALSE;

  g_debug ("providing %s", g_dbus_interface_skeleton_get_info (helper)->name);

  return TRUE;
}
