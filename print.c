#define _GNU_SOURCE 1

#include "config.h"

#include <string.h>

#include <gtk/gtk.h>
#include <gtk/gtkunixprint.h>

#include <gio/gio.h>

#include "xdg-desktop-portal-dbus.h"

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

#include "print.h"
#include "request.h"

typedef struct {
  Request *request;

  GtkWidget *dialog;

  char *filename;
  int response;

} PrintDialogHandle;

static void
print_dialog_handle_free (gpointer data)
{
  PrintDialogHandle *handle = data;

  g_object_unref (handle->dialog);
  g_free (handle->filename);

  g_free (handle);
}

static void
print_dialog_handle_close (PrintDialogHandle *handle)
{
  gtk_widget_destroy (handle->dialog);
  print_dialog_handle_free (handle);
}

static void
send_response (PrintDialogHandle *handle)
{
  GVariantBuilder opt_builder;

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

  if (handle->request->exported)
    {
      xdp_request_emit_response (XDP_REQUEST (handle->request),
                                 handle->response,
                                 g_variant_builder_end (&opt_builder));
      request_unexport (handle->request);
    }

  print_dialog_handle_close (handle);
}

static void
handle_print_dialog_response (GtkDialog *dialog,
                              gint response,
                              gpointer data)
{
  PrintDialogHandle *handle = data;

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
      {
        GtkPageSetup *setup;
        GtkPrintSettings *settings;
        GtkPrinter *printer;
        GtkPrintJob *job;
        GError *error = NULL;

        setup = gtk_print_unix_dialog_get_page_setup (GTK_PRINT_UNIX_DIALOG (handle->dialog));
        settings = gtk_print_unix_dialog_get_settings (GTK_PRINT_UNIX_DIALOG (handle->dialog));
        printer = gtk_print_unix_dialog_get_selected_printer (GTK_PRINT_UNIX_DIALOG (handle->dialog))
;
        job = gtk_print_job_new ("", //TODO send title along
                                 printer, settings, setup);
        g_clear_object (&settings);

        if (!gtk_print_job_set_source_file (job, handle->filename, &error))
          {
            // TODO report error;
            g_warning ("printing failed: %s\n", error->message);
            g_error_free (error);
          }
        else
          {
            g_print ("send %s to printer\n", handle->filename);
            gtk_print_job_send (job, NULL, NULL, NULL); //TODO: wait ?
          }
        g_object_unref (job);

        handle->response = 0;
      }
      break;
    }

  send_response (handle);
}

static gboolean
handle_close (XdpRequest *object,
              GDBusMethodInvocation *invocation,
              PrintDialogHandle *handle)
{
  print_dialog_handle_close (handle);
  return FALSE;
}

static gboolean
handle_print_file (XdpPrint *object,
                   GDBusMethodInvocation *invocation,
                   const char *arg_handle,
                   const char *arg_app_id,
                   const char *arg_parent_window,
                   const char *arg_title,
                   const char *arg_filename,
                   GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation, arg_app_id, arg_handle);
  GtkWidget *dialog;
  GdkWindow *foreign_parent = NULL;
  PrintDialogHandle *handle;

 #ifdef GDK_WINDOWING_X11
  if (g_str_has_prefix (arg_parent_window, "x11:"))
    {
      int xid;

      if (sscanf (arg_parent_window, "x11:%x", &xid) != 1)
        g_warning ("invalid xid");
      else
        foreign_parent = gdk_x11_window_foreign_new_for_display (gdk_display_get_default (), xid);
    }
#endif
  else
    g_warning ("Unhandled parent window type %s\n", arg_parent_window);

  dialog = gtk_print_unix_dialog_new (arg_title, NULL);
  gtk_print_unix_dialog_set_manual_capabilities (GTK_PRINT_UNIX_DIALOG (dialog), 0);

  handle = g_new0 (PrintDialogHandle, 1);
  handle->request = request;
  handle->dialog = g_object_ref (dialog);
  handle->filename = g_strdup (arg_filename);

  g_signal_connect (request, "handle-close", G_CALLBACK (handle_close), handle);

  g_signal_connect (dialog, "response", G_CALLBACK (handle_print_dialog_response), handle);

  gtk_widget_realize (dialog);

  if (foreign_parent)
    gdk_window_set_transient_for (gtk_widget_get_window (dialog), foreign_parent);

  gtk_widget_show (dialog);

  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_print_complete_print_file (object, invocation);

  return TRUE;
}

gboolean
print_init (GDBusConnection *bus,
            GError **error)
{
  GDBusInterfaceSkeleton *helper;

  helper = G_DBUS_INTERFACE_SKELETON (xdp_print_skeleton_new ());

  g_signal_connect (helper, "handle-print-file", G_CALLBACK (handle_print_file), NULL);

  if (!g_dbus_interface_skeleton_export (helper,
                                         bus,
                                         "/org/freedesktop/portal/desktop",
                                         error))
    return FALSE;

  g_debug ("providing %s", g_dbus_interface_skeleton_get_info (helper)->name);

  return TRUE;
}
