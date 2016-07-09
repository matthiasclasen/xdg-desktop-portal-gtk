#define _GNU_SOURCE 1

#include "config.h"

#include <errno.h>
#include <locale.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <glib/gi18n.h>

#include <gtk/gtk.h>

#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <gio/gunixfdlist.h>

#include "xdg-desktop-portal-dbus.h"
#include "shell-dbus.h"

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

#include "request.h"
#include "utils.h"

typedef struct {
  XdpImplAccess *impl;
  GDBusMethodInvocation *invocation;
  Request *request;

  GtkWidget *dialog;

  int response;
} AccessDialogHandle;

static void
access_dialog_handle_free (gpointer data)
{
  AccessDialogHandle *handle = data;

  g_object_unref (handle->request);
  g_object_unref (handle->dialog);

  g_free (handle);
}

static void
access_dialog_handle_close (AccessDialogHandle *handle)
{
  gtk_widget_destroy (handle->dialog);
  access_dialog_handle_free (handle);
}

static void
send_response (AccessDialogHandle *handle)
{
  GVariantBuilder opt_builder;

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

  if (handle->request->exported)
    request_unexport (handle->request);

  xdp_impl_access_complete_access_dialog (handle->impl,
                                          handle->invocation,
                                          handle->response,
                                          g_variant_builder_end (&opt_builder));

  access_dialog_handle_close (handle);
}

static void
access_dialog_response (GtkWidget *widget,
                        int response,
                        gpointer user_data)
{
  AccessDialogHandle *handle = user_data;

  switch (response)
    {
    default:
      g_warning ("Unexpected response: %d", response);
      /* Fall through */
    case GTK_RESPONSE_DELETE_EVENT:
      handle->response = 2;
      break;

    case GTK_RESPONSE_CANCEL:
      handle->response = 1;
      break;

    case GTK_RESPONSE_OK:
      handle->response = 0;
      break;
    }

  send_response (handle);
}

static gboolean
handle_close (XdpImplRequest *object,
              GDBusMethodInvocation *invocation,
              AccessDialogHandle *handle)
{
  GVariantBuilder opt_builder;

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  xdp_impl_access_complete_access_dialog (handle->impl,
                                          handle->invocation,
                                          2,
                                          g_variant_builder_end (&opt_builder));
  access_dialog_handle_close (handle);
  return FALSE;
}

static gboolean
handle_access_dialog (XdpImplAccess *object,
                      GDBusMethodInvocation *invocation,
                      const char *arg_handle,
                      const char *arg_app_id,
                      const char *arg_parent_window,
                      const char *arg_title,
                      const char *arg_subtitle,
                      const char *arg_body,
                      GVariant *arg_options)
{
  g_autoptr(Request) request = NULL;
  const char *sender;
  AccessDialogHandle *handle;
  g_autoptr(GError) error = NULL;
  g_autofree char *filename = NULL;
  gboolean modal;
  GtkWidget *dialog;
  GdkWindow *foreign_parent = NULL;
  const char *deny_label;
  const char *grant_label;
  const char *icon;

  sender = g_dbus_method_invocation_get_sender (invocation);

  request = request_new (sender, arg_app_id, arg_handle);

  if (!g_variant_lookup (arg_options, "modal", "b", &modal))
    modal = TRUE;

  if (!g_variant_lookup (arg_options, "deny_label", "&s", &deny_label))
    deny_label = _("Deny Access");

  if (!g_variant_lookup (arg_options, "grant_label", "&s", &grant_label))
    grant_label = _("Grant Access");

  if (!g_variant_lookup (arg_options, "icon", "&s", &icon))
    icon = NULL;

  dialog = gtk_message_dialog_new (NULL,
                                   0,
                                   GTK_MESSAGE_QUESTION,
                                   GTK_BUTTONS_NONE,
                                   arg_title,
                                   NULL);
  gtk_window_set_modal (GTK_WINDOW (dialog), modal);
  gtk_dialog_add_button (GTK_DIALOG (dialog), deny_label, GTK_RESPONSE_CANCEL);
  gtk_dialog_add_button (GTK_DIALOG (dialog), grant_label, GTK_RESPONSE_OK);
  gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog), arg_subtitle);
  if (!g_str_equal (arg_body, ""))
    {
      GtkWidget *area;
      GtkWidget *body_label;

      area = gtk_message_dialog_get_message_area (GTK_MESSAGE_DIALOG (dialog));
      body_label = gtk_label_new (arg_body);
      gtk_label_set_line_wrap (GTK_LABEL (body_label), TRUE);
      gtk_widget_show (body_label);
      gtk_container_add (GTK_CONTAINER (area), body_label);
    }
  if (icon != NULL)
    {
      GtkWidget *image;

      image = gtk_image_new_from_icon_name (icon, GTK_ICON_SIZE_DIALOG);
      gtk_widget_show (image);
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
      gtk_message_dialog_set_image (GTK_MESSAGE_DIALOG (dialog), image);
G_GNUC_END_IGNORE_DEPRECATIONS
    }

#ifdef GDK_WINDOWING_X11
  if (g_str_has_prefix (arg_parent_window, "x11:"))
    {
      int xid;

      if (sscanf (arg_parent_window, "x11:%x", &xid) != 1)
        g_warning ("invalid xid");
      else
        foreign_parent = gdk_x11_window_foreign_new_for_display (gtk_widget_get_display (dialog), xid);
    }
#endif
  else
    g_warning ("Unhandled parent window type %s", arg_parent_window);

  handle = g_new0 (AccessDialogHandle, 1);
  handle->impl = object;
  handle->invocation = invocation;
  handle->request = g_object_ref (request);
  handle->dialog = g_object_ref (dialog);

  g_signal_connect (request, "handle-close", G_CALLBACK (handle_close), handle);

  g_signal_connect (dialog, "response", G_CALLBACK (access_dialog_response), handle);

  gtk_widget_realize (dialog);

  if (foreign_parent)
    gdk_window_set_transient_for (gtk_widget_get_window (dialog), foreign_parent);

  gtk_widget_show (dialog);

  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  return TRUE;
}

gboolean
access_init (GDBusConnection *bus,
             GError **error)
{
  GDBusInterfaceSkeleton *helper;

  helper = G_DBUS_INTERFACE_SKELETON (xdp_impl_access_skeleton_new ());

  g_signal_connect (helper, "handle-access-dialog", G_CALLBACK (handle_access_dialog), NULL);

  if (!g_dbus_interface_skeleton_export (helper,
                                         bus,
                                         DESKTOP_PORTAL_OBJECT_PATH,
                                         error))
    return FALSE;

  g_debug ("providing %s", g_dbus_interface_skeleton_get_info (helper)->name);

  return TRUE;
}
