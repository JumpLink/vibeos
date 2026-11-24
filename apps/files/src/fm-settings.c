/* The Settings page (fm-settings.h): a list of categories, their titles in the accent, and check box rows, the box
 * at the end. */
#include "fm-settings.h"
#include "fm-ui.h"

#include <glib/gi18n.h>
#include <vibeos.h>
#include <spongy-gtk.h>

static void
on_row (VibeosPressable *row, GtkWidget *check)
{
  fm_check_set_active (check, !fm_check_get_active (check));
}

static void
category (GtkWidget *list, const char *title)
{
  GtkWidget *label = fm_label (title, "fm-pref-category", 14, FALSE);

  gtk_box_append (GTK_BOX (list), label);
}

static void
check_row (GtkWidget *list, GSettings *settings, const char *key, const char *title)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *text = fm_label (title, "fm-pref-title", 16, TRUE);
  GtkWidget *check = fm_check_new (FALSE);
  GtkWidget *row;

  gtk_widget_set_hexpand (text, TRUE);
  gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (check, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), text);
  gtk_box_append (GTK_BOX (box), check);
  row = vibeos_pressable_new (box);
  gtk_widget_add_css_class (row, "fm-pref-row");
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (row), VIBEOS_RIPPLE_BOUNDED, 0);
  g_settings_bind (settings, key, check, "active", G_SETTINGS_BIND_DEFAULT);
  g_signal_connect (row, "clicked", G_CALLBACK (on_row), check);
  gtk_accessible_update_property (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
  gtk_box_append (GTK_BOX (list), row);
}

GtkWidget *
fm_settings_page_new (FmShell *shell)
{
  GtkWidget *list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *scroll = spongy_scroll_view_new ();

  gtk_widget_add_css_class (list, "fm-pref-list");
  /* File Manager+'s own categories, with only what applies on a Linux phone */
  category (list, _("View"));
  check_row (list, shell->settings, "show-hidden", _("Show hidden files"));
  check_row (list, shell->settings, "show-thumbnails", _("Show thumbnails"));
  category (list, _("Recycle Bin Settings"));
  check_row (list, shell->settings, "use-recycle-bin", _("Use Recycle bin by default"));
  check_row (list, shell->settings, "recycle-confirmation", _("Show recycle confirmation"));
  category (list, _("Advanced Settings"));
  check_row (list, shell->settings, "show-history", _("Show history"));
  check_row (list, shell->settings, "show-system", _("Show system storage"));
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), list);
  gtk_widget_add_css_class (scroll, "fm-settings");
  return scroll;
}
