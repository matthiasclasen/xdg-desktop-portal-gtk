#define _GNU_SOURCE 1

#include "config.h"

#include <errno.h>
#include <locale.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <gtk/gtk.h>

#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <gio/gunixfdlist.h>

#include "xdg-desktop-portal-dbus.h"

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

#include "screenshotdialog.h"
#include "screenshot.h"
#include "request.h"

static XdpOrgGnomeShellScreenshot *shell;

typedef struct {
  Request *request;

  GtkWidget *dialog;

  int response;
  char *uri;
} ScreenshotDialogHandle;

static void
screenshot_dialog_handle_free (gpointer data)
{
  ScreenshotDialogHandle *handle = data;

  g_object_unref (handle->dialog);
  g_free (handle->uri);

  g_free (handle);
}

static void
screenshot_dialog_handle_close (ScreenshotDialogHandle *handle)
{
  gtk_widget_destroy (handle->dialog);
  screenshot_dialog_handle_free (handle);
}

static void
send_response (ScreenshotDialogHandle *handle)
{
  GVariantBuilder opt_builder;

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&opt_builder, "{sv}", "uri", g_variant_new_string (handle->uri ? handle->uri : ""));

  if (handle->request->exported)
    {
      xdp_request_emit_response (XDP_REQUEST (handle->request),
                                 handle->response,
                                 g_variant_builder_end (&opt_builder));
      request_unexport (handle->request);
    }

  screenshot_dialog_handle_close (handle);
}

static void
screenshot_dialog_done (GtkWidget *widget,
                        int response,
                        gpointer user_data)
{
  ScreenshotDialogHandle *handle = user_data;

  switch (response)
    {
    default:
      g_warning ("Unexpected response: %d", response);
      /* Fall through */
    case GTK_RESPONSE_DELETE_EVENT:
      handle->response = 2;
      g_free (handle->uri);
      handle->uri = NULL;
      break;

    case GTK_RESPONSE_CANCEL:
      handle->response = 1;
      g_free (handle->uri);
      handle->uri = NULL;
      break;

    case GTK_RESPONSE_OK:
      handle->response = 0;
      break;
    }

  send_response (handle);
}

static gboolean
handle_close (XdpRequest *object,
              GDBusMethodInvocation *invocation,
              ScreenshotDialogHandle *handle)
{
  screenshot_dialog_handle_close (handle);
  return FALSE;
}

static gboolean
handle_screenshot (XdpScreenshot *object,
                   GDBusMethodInvocation *invocation,
                   const char *arg_handle,
                   const char *arg_app_id,
                   const char *arg_parent_window,
                   GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation, arg_app_id, arg_handle);
  ScreenshotDialogHandle *handle;
  g_autoptr(GError) error = NULL;
  g_autofree char *filename = NULL;
  gboolean success;
  GtkWidget *dialog;

  xdp_org_gnome_shell_screenshot_call_screenshot_sync (shell,
                                                       FALSE,
                                                       TRUE,
                                                       "Screenshot",
                                                       &success,
                                                       &filename,
                                                       NULL,
                                                       NULL);

  dialog = GTK_WIDGET (screenshot_dialog_new (arg_app_id, filename));

  handle = g_new0 (ScreenshotDialogHandle, 1);
  handle->request = request;
  handle->dialog = g_object_ref (dialog);
  handle->uri = g_strconcat ("file://", filename, NULL);

  g_signal_connect (request, "handle-close", G_CALLBACK (handle_close), handle);

  g_signal_connect (dialog, "done", G_CALLBACK (screenshot_dialog_done), handle);

  gtk_window_present (GTK_WINDOW (dialog));

  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_screenshot_complete_screenshot (object, invocation);

  return TRUE;
}

gboolean
screenshot_init (GDBusConnection *bus,
                 GError **error)
{
  GDBusInterfaceSkeleton *helper;

  helper = G_DBUS_INTERFACE_SKELETON (xdp_screenshot_skeleton_new ());

  g_signal_connect (helper, "handle-screenshot", G_CALLBACK (handle_screenshot), NULL);

  if (!g_dbus_interface_skeleton_export (helper,
                                         bus,
                                         "/org/freedesktop/portal/desktop",
                                         error))
    return FALSE;

  shell = xdp_org_gnome_shell_screenshot_proxy_new_sync (bus,
                                                         G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                                         "org.gnome.Shell.Screenshot",
                                                         "/org/gnome/Shell/Screenshot",
                                                         NULL,
                                                         error);
  if (shell == NULL)
    return FALSE;

  g_debug ("providing %s", g_dbus_interface_skeleton_get_info (helper)->name);

  return TRUE;
}
