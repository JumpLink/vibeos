/* Share (fm-share.h). */
#include "fm-share.h"
#include "fm-window.h"

#include <fcntl.h>
#include <gio/gunixfdlist.h>
#include <glib/gi18n.h>
#include <unistd.h>

#define PORTAL_NAME  "org.freedesktop.portal.Desktop"
#define PORTAL_PATH  "/org/freedesktop/portal/desktop"
#define EMAIL_IFACE  "org.freedesktop.portal.Email"

static gboolean available;

static void
on_version (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);

  available = reply != NULL;
  if (!available)
    g_debug ("No Email portal, so no Share: %s", error->message);
}

static void
on_bus (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GDBusConnection) bus = g_bus_get_finish (result, NULL);

  /* the portal exports the interface only when a backend implements it */
  if (bus)
    g_dbus_connection_call (bus, PORTAL_NAME, PORTAL_PATH, "org.freedesktop.DBus.Properties", "Get",
                            g_variant_new ("(ss)", EMAIL_IFACE, "version"), G_VARIANT_TYPE ("(v)"),
                            G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_version, NULL);
}

void
fm_share_init (void)
{
  g_bus_get (G_BUS_TYPE_SESSION, NULL, on_bus, NULL);
}

gboolean
fm_share_possible (GPtrArray *files)
{
  if (!available || !files->len)
    return FALSE;
  for (guint i = 0; i < files->len; i++)
    {
      g_autofree char *path = g_file_get_path (files->pdata[i]);

      if (!path || g_file_test (path, G_FILE_TEST_IS_DIR))
        return FALSE;
    }
  return TRUE;
}

static void
on_composed (GObject *source, GAsyncResult *result, gpointer data)
{
  GtkWidget *window = data;
  FmShell *shell = g_object_get_data (G_OBJECT (window), "fm-shell");
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_with_unix_fd_list_finish (G_DBUS_CONNECTION (source), NULL,
                                                                                 result, &error);

  if (!reply)
    {
      g_warning ("The Email portal did not take the files: %s", error->message);
      if (shell)
        fm_shell_toast (shell, _("Could not share the files"));
    }
  g_object_unref (window);
}

void
fm_share_files (GtkWidget *parent, GPtrArray *files)
{
  GtkWidget *window = GTK_WIDGET (gtk_widget_get_root (parent));
  FmShell *shell = window ? g_object_get_data (G_OBJECT (window), "fm-shell") : NULL;
  g_autoptr(GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  g_autoptr(GUnixFDList) fds = g_unix_fd_list_new ();
  GVariantBuilder handles, options;

  if (!bus || !window)
    return;
  g_variant_builder_init (&handles, G_VARIANT_TYPE ("ah"));
  for (guint i = 0; i < files->len; i++)
    {
      g_autofree char *path = g_file_get_path (files->pdata[i]);
      g_autoptr(GError) error = NULL;
      int fd = path ? open (path, O_RDONLY | O_CLOEXEC) : -1;
      int handle;

      if (fd < 0)
        {
          g_warning ("Could not open %s to share it", path ? path : "a file");
          if (shell)
            fm_shell_toast (shell, _("Could not share the files"));
          return;
        }
      handle = g_unix_fd_list_append (fds, fd, &error);
      close (fd);
      if (handle < 0)
        {
          g_warning ("Could not pass %s to the portal: %s", path, error->message);
          return;
        }
      g_variant_builder_add (&handles, "h", handle);
    }
  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&options, "{sv}", "attachment_fds", g_variant_builder_end (&handles));
  g_dbus_connection_call_with_unix_fd_list (bus, PORTAL_NAME, PORTAL_PATH, EMAIL_IFACE, "ComposeEmail",
                                            g_variant_new ("(sa{sv})", "", &options), G_VARIANT_TYPE ("(o)"),
                                            G_DBUS_CALL_FLAGS_NONE, -1, fds, NULL, on_composed,
                                            g_object_ref (window));
}
