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

#include "filechooser.h"
#include "request.h"

typedef struct {
  Request *request;
  GtkWidget *dialog;
  GtkFileChooserAction action;
  gboolean multiple;

  int response;
  GSList *uris;

  gboolean allow_write;

  GSList *choices;
} FileDialogHandle;

static void
file_dialog_handle_free (gpointer data)
{
  FileDialogHandle *handle = data;

  g_object_unref (handle->dialog);
  g_slist_free_full (handle->uris, g_free);
  g_slist_free_full (handle->choices, g_free);

  g_free (handle);
}

static void
file_dialog_handle_close (FileDialogHandle *handle)
{
  gtk_widget_destroy (handle->dialog);
  file_dialog_handle_free (handle);
}

static void
add_choices (FileDialogHandle *handle,
             GVariantBuilder *builder)
{
  GVariantBuilder choices;
  const char *id;
  const char *selected;
  GSList *l;

  g_variant_builder_init (&choices, G_VARIANT_TYPE ("a(ss)"));
  for (l = handle->choices; l; l = l->next)
    {
      id = l->data;
      selected = gtk_file_chooser_get_choice (GTK_FILE_CHOOSER (handle->dialog), id);
      g_variant_builder_add (&choices, "(ss)", id, selected);
    }

  g_variant_builder_add (builder, "{sv}", "choices", g_variant_builder_end (&choices));
}

static void
send_response (FileDialogHandle *handle)
{
  GVariantBuilder uri_builder;
  GVariantBuilder opt_builder;
  GSList *l;

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

  g_variant_builder_init (&uri_builder, G_VARIANT_TYPE_STRING_ARRAY);
  for (l = handle->uris; l; l = l->next)
    g_variant_builder_add (&uri_builder, "s", l->data);

  g_variant_builder_add (&opt_builder, "{sv}", "uris", g_variant_builder_end (&uri_builder));
  g_variant_builder_add (&opt_builder, "{sv}", "writable", g_variant_new_variant (g_variant_new_boolean (handle->allow_write)));

  add_choices (handle, &opt_builder);

  if (handle->request->exported)
    {
      xdp_request_emit_response (XDP_REQUEST (handle->request),
                                 handle->response,
                                 g_variant_builder_end (&opt_builder));
      request_unexport (handle->request);
    }

  file_dialog_handle_close (handle);
}

GtkFileFilter *
gtk_file_filter_from_gvariant (GVariant *variant)
{
  GtkFileFilter *filter;
  GVariantIter *iter;
  const char *name;
  int type;
  char *tmp;

  filter = gtk_file_filter_new ();

  g_variant_get (variant, "(&sa(us))", &name, &iter);

  gtk_file_filter_set_name (filter, name);

  while (g_variant_iter_next (iter, "(u&s)", &type, &tmp))
    {
      switch (type)
        {
        case 0:
          gtk_file_filter_add_pattern (filter, tmp);
          break;
        case 1:
          gtk_file_filter_add_mime_type (filter, tmp);
          break;
        default:
          break;
       }
    }
  g_variant_iter_free (iter);

  return filter;
}

static void
file_chooser_response (GtkWidget *widget,
                       int response,
                       gpointer user_data)
{
  FileDialogHandle *handle = user_data;

  switch (response)
    {
    default:
      g_warning ("Unexpected response: %d", response);
      /* Fall through */
    case GTK_RESPONSE_DELETE_EVENT:
      handle->response = 2;
      handle->uris = NULL;
      break;

    case GTK_RESPONSE_CANCEL:
      handle->response = 1;
      handle->uris = NULL;
      break;

    case GTK_RESPONSE_OK:
      handle->response = 0;
      handle->uris = gtk_file_chooser_get_uris (GTK_FILE_CHOOSER (widget));
      break;
    }

  send_response (handle);
}

static void
read_only_toggled (GtkToggleButton *button, gpointer user_data)
{
  FileDialogHandle *handle = user_data;

  handle->allow_write = !gtk_toggle_button_get_active (button);
}

static void
deserialize_choice (GVariant *choice,
                    FileDialogHandle *handle)
{
  const char *id;
  const char *label;
  const char *selected;
  GVariant *choices;
  int i;

  g_variant_get (choice, "(&s&s@a(ss)&s)", &id, &label, &choices, &selected);

  if (g_variant_n_children (choices) > 0)
    {
      char **options;
      char **labels;

      options = g_new (char *, g_variant_n_children (choices) + 1);
      labels = g_new (char *, g_variant_n_children (choices) + 1);

      for (i = 0; i < g_variant_n_children (choices); i++)
        g_variant_get_child (choices, i, "(&s&s)", &options[i], &labels[i]);

      gtk_file_chooser_add_choice (GTK_FILE_CHOOSER (handle->dialog),
                                   id, label,
                                   (const char **)options,
                                   (const char **)labels);

      g_free (options);
      g_free (labels);
    }
  else
    {
      gtk_file_chooser_add_choice (GTK_FILE_CHOOSER (handle->dialog),
                                   id, label, NULL, NULL);
    }

  gtk_file_chooser_set_choice (GTK_FILE_CHOOSER (handle->dialog), id, selected);
  handle->choices = g_slist_prepend (handle->choices, g_strdup (id));
}

static gboolean
handle_close (XdpRequest *object,
              GDBusMethodInvocation *invocation,
              FileDialogHandle *handle)
{
  file_dialog_handle_close (handle);
  return FALSE;
}

static gboolean
handle_open (XdpFileChooser *object,
             GDBusMethodInvocation *invocation,
             const char *arg_handle,
             const char *arg_app_id,
             const char *arg_parent_window,
             const char *arg_title,
             GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation, arg_app_id, arg_handle);
  const gchar *method_name;
  GtkFileChooserAction action;
  gboolean multiple;
  GtkWidget *dialog;
  GdkWindow *foreign_parent = NULL;
  GtkWidget *fake_parent;
  FileDialogHandle *handle;
  const char *cancel_label;
  const char *accept_label;
  GVariantIter *iter;
  const char *current_name;
  const char *path;
  g_autoptr (GVariant) choices = NULL;

  method_name = g_dbus_method_invocation_get_method_name (invocation);

  fake_parent = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  g_object_ref_sink (fake_parent);

  action = GTK_FILE_CHOOSER_ACTION_OPEN;
  multiple = FALSE;

  if (strcmp (method_name, "SaveFile") == 0)
    action = GTK_FILE_CHOOSER_ACTION_SAVE;
  else if (strcmp (method_name, "OpenFiles") == 0)
    multiple = TRUE;

  if (!g_variant_lookup (arg_options, "accept_label", "&s", &accept_label))
    accept_label = "_Open";

  cancel_label = "_Cancel";

  dialog = gtk_file_chooser_dialog_new (arg_title, GTK_WINDOW (fake_parent), action,
                                        cancel_label, GTK_RESPONSE_CANCEL,
                                        accept_label, GTK_RESPONSE_OK,
                                        NULL);
  gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);
  gtk_file_chooser_set_select_multiple (GTK_FILE_CHOOSER (dialog), multiple);

  handle = g_new0 (FileDialogHandle, 1);
  handle->request = request;
  handle->dialog = g_object_ref (dialog);
  handle->action = action;
  handle->multiple = multiple;
  handle->choices = NULL;

  g_signal_connect (request, "handle-close", G_CALLBACK (handle_close), handle);

  g_signal_connect (dialog, "response", G_CALLBACK (file_chooser_response), handle);

  choices = g_variant_lookup_value (arg_options, "choices", G_VARIANT_TYPE ("a(ssa(ss)s)"));
  if (choices)
    {
      int i;

      for (i = 0; i < g_variant_n_children (choices); i++)
        deserialize_choice (g_variant_get_child_value (choices, i), handle);
    }

  if (g_variant_lookup (arg_options, "filters", "a(sa(us))", &iter))
    {
      GVariant *variant;

      while (g_variant_iter_next (iter, "@(sa(us))", &variant))
        {
          GtkFileFilter *filter;

          filter = gtk_file_filter_from_gvariant (variant);
          gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (dialog), filter);
          g_variant_unref (variant);
        }
      g_variant_iter_free (iter);
    }
  if (strcmp (method_name, "SaveFile") == 0)
    {
      if (g_variant_lookup (arg_options, "current_name", "&s", &current_name))
        gtk_file_chooser_set_current_name (GTK_FILE_CHOOSER (dialog), current_name);
      /* TODO: is this useful ?
       * In a sandboxed situation, the current folder and current file
       * are likely in the fuse filesystem
       */
      if (g_variant_lookup (arg_options, "current_folder", "&ay", &path))
        gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (dialog), path);
      if (g_variant_lookup (arg_options, "current_file", "&ay", &path))
        gtk_file_chooser_select_filename (GTK_FILE_CHOOSER (dialog), path);
    }

  g_object_unref (fake_parent);

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
    g_warning ("Unhandled parent window type %s\n", arg_parent_window);

  gtk_file_chooser_set_do_overwrite_confirmation (GTK_FILE_CHOOSER (dialog), TRUE);

  if (action == GTK_FILE_CHOOSER_ACTION_OPEN)
    {
      GtkWidget *readonly;

      readonly = gtk_check_button_new_with_label ("Open files read-only");
      gtk_widget_show (readonly);

      g_signal_connect (readonly, "toggled",
                        G_CALLBACK (read_only_toggled), handle);

      gtk_file_chooser_set_extra_widget (GTK_FILE_CHOOSER (dialog), readonly);
    }

  gtk_widget_realize (dialog);

  if (foreign_parent)
    gdk_window_set_transient_for (gtk_widget_get_window (dialog), foreign_parent);

  gtk_widget_show (dialog);

  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_file_chooser_complete_open_file (object, invocation);

  return TRUE;
}

gboolean
file_chooser_init (GDBusConnection *bus,
                   GError **error)
{
  GDBusInterfaceSkeleton *helper;

  helper = G_DBUS_INTERFACE_SKELETON (xdp_file_chooser_skeleton_new ());

  g_signal_connect (helper, "handle-open-file", G_CALLBACK (handle_open), NULL);
  g_signal_connect (helper, "handle-open-files", G_CALLBACK (handle_open), NULL);
  g_signal_connect (helper, "handle-save-file", G_CALLBACK (handle_open), NULL);

  if (!g_dbus_interface_skeleton_export (helper,
                                         bus,
                                         "/org/freedesktop/portal/desktop",
                                         error))
    return FALSE;

  g_debug ("providing %s", g_dbus_interface_skeleton_get_info (helper)->name);

  return TRUE;
}
