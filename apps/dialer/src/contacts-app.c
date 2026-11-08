/* The Contacts app: see contacts-app.h. GNOME Contacts is the one this phone has; it opens at its list. */
#include "contacts-app.h"

#include <gio/gdesktopappinfo.h>
#include <glib/gi18n.h>

void
contacts_app_open (NeoSnackbar *snackbar)
{
  g_autoptr(GDesktopAppInfo) info = g_desktop_app_info_new ("org.gnome.Contacts.desktop");
  g_autoptr(GError) error = NULL;

  if (!info)
    {
      neo_snackbar_show (snackbar, _("No contacts app available"), NULL, NULL, NULL, NEO_SNACKBAR_SHORT);
      return;
    }
  if (!g_app_info_launch (G_APP_INFO (info), NULL, NULL, &error))
    g_warning ("contacts: %s", error->message);
}
