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
#include "shell-dbus.h"

#include "gnome-accounts.h"
#include "request.h"
#include "utils.h"

static gboolean
handle_get_accounts (XdpImplGnomeAccounts *object,
                     GDBusMethodInvocation *invocation,
                     const char *arg_handle,
                     const char *arg_app_id,
                     const char *arg_window,
                     GVariant *arg_options,
                     gpointer data)
{
  guint response = 0;
  GVariantBuilder opt_builder;

g_print ("handle_get_accounts\n");
  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  xdp_impl_gnome_accounts_complete_get_accounts (object,
                                                 invocation,
                                                 response,
                                                 g_variant_builder_end (&opt_builder));

  return TRUE;
}

static gboolean
handle_add_account (XdpImplGnomeAccounts *object,
                    GDBusMethodInvocation *invocation,
                    const char *arg_handle,
                    const char *arg_app_id,
                    const char *arg_window,
                    const char *arg_provider,
                    GVariant *arg_options,
                    gpointer data)
{
  guint response = 0;
  GVariantBuilder opt_builder;

g_print ("handle_add_account\n");
  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  xdp_impl_gnome_accounts_complete_add_account (object,
                                                invocation,
                                                response,
                                                g_variant_builder_end (&opt_builder));

  return TRUE;
}

static gboolean
handle_ensure_credentials (XdpImplGnomeAccounts *object,
                           GDBusMethodInvocation *invocation,
                           const char *arg_handle,
                           const char *arg_app_id,
                           const char *arg_window,
                           const char *arg_provider,
                           GVariant *arg_options,
                           gpointer data)
{
  guint response = 0;
  GVariantBuilder opt_builder;

g_print ("handle_ensure_credentials\n");
  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  xdp_impl_gnome_accounts_complete_ensure_credentials (object,
                                                       invocation,
                                                       response,
                                                       g_variant_builder_end (&opt_builder));

  return TRUE;
}

gboolean
gnome_accounts_init (GDBusConnection *bus,
                     GError **error)
{
  GDBusInterfaceSkeleton *helper;

  helper = G_DBUS_INTERFACE_SKELETON (xdp_impl_gnome_accounts_skeleton_new ());

  g_signal_connect (helper, "handle-get-accounts", G_CALLBACK (handle_get_accounts), NULL);
  g_signal_connect (helper, "handle-add-account", G_CALLBACK (handle_add_account), NULL);
  g_signal_connect (helper, "handle-ensure-credentials", G_CALLBACK (handle_ensure_credentials), NULL);

  if (!g_dbus_interface_skeleton_export (helper,
                                         bus,
                                         DESKTOP_PORTAL_OBJECT_PATH,
                                         error))
    return FALSE;

  g_debug ("providing %s", g_dbus_interface_skeleton_get_info (helper)->name);

  return TRUE;
}
