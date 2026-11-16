/* The preference rows (cal-prefs.h). */
#include "cal-prefs.h"
#include "cal-dialogs.h"
#include "cal-format.h"
#include "cal-page.h"

#include <glib/gi18n.h>
#include <spongy-gtk.h>
#include <stdlib.h>

/* --- rows: an icon (its space kept without one), the title, the summary under it, an optional widget at the
 * end */

GtkWidget *
cal_pref_icon (const char *icon_name, guint32 color)
{
  GtkWidget *icon = gtk_image_new_from_icon_name (icon_name);

  gtk_image_set_pixel_size (GTK_IMAGE (icon), 24);
  gtk_widget_add_css_class (icon, "cal-pref-icon");
  if (color)
    {
      /* a calendar's circle in its own colour */
      g_autofree char *css = g_strdup_printf ("#%06x", color & 0xffffff);
      GtkCssProvider *provider = gtk_css_provider_new ();
      g_autofree char *rule = g_strdup_printf ("image { color: %s; }", css);

      gtk_css_provider_load_from_string (provider, rule);
      /* a per-widget colour has no non-deprecated form short of drawing the icon itself */
      G_GNUC_BEGIN_IGNORE_DEPRECATIONS
      gtk_style_context_add_provider (gtk_widget_get_style_context (icon), GTK_STYLE_PROVIDER (provider),
                                      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);
      G_GNUC_END_IGNORE_DEPRECATIONS
      g_object_unref (provider);
    }
  return icon;
}

/* the summary shown or not, and its row's padding with it (calendar.css .cal-pref-texts.summary) */
static void
show_summary (GtkWidget *summary, gboolean shown)
{
  GtkWidget *texts = gtk_widget_get_parent (summary);

  gtk_widget_set_visible (summary, shown);
  if (texts && shown)
    gtk_widget_add_css_class (texts, "summary");
  else if (texts)
    gtk_widget_remove_css_class (texts, "summary");
}

CalPref
cal_pref_row (const char *icon_name, guint32 color, const char *title, const char *summary, GtkWidget *end)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *frame = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *texts = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  CalPref pref = { 0 };

  /* the icon frame keeps its space without an icon */
  gtk_widget_add_css_class (frame, "cal-pref-icon-frame");
  if (icon_name)
    gtk_box_append (GTK_BOX (frame), cal_pref_icon (icon_name, color));
  gtk_box_append (GTK_BOX (box), frame);
  pref.title = gtk_label_new (title);
  gtk_label_set_xalign (GTK_LABEL (pref.title), 0);
  gtk_label_set_wrap (GTK_LABEL (pref.title), TRUE);
  gtk_widget_add_css_class (pref.title, "cal-pref-title");
  vibeos_label_set_font_padding (pref.title, 16);
  gtk_box_append (GTK_BOX (texts), pref.title);
  pref.summary = gtk_label_new (summary);
  gtk_label_set_xalign (GTK_LABEL (pref.summary), 0);
  gtk_label_set_wrap (GTK_LABEL (pref.summary), TRUE);
  gtk_widget_add_css_class (pref.summary, "cal-pref-summary");
  gtk_box_append (GTK_BOX (texts), pref.summary);
  show_summary (pref.summary, summary && *summary);
  gtk_widget_set_hexpand (texts, TRUE);
  gtk_widget_set_valign (texts, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (texts, "cal-pref-texts");
  gtk_box_append (GTK_BOX (box), texts);
  if (end)
    {
      gtk_widget_set_valign (end, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class (end, "cal-pref-widget");
      gtk_box_append (GTK_BOX (box), end);
      pref.end = end;
    }
  gtk_widget_add_css_class (box, "cal-pref");
  pref.row = vibeos_pressable_new (box);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (pref.row), VIBEOS_RIPPLE_BOUNDED, 0);
  gtk_accessible_update_property (GTK_ACCESSIBLE (pref.row), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
  return pref;
}

void
cal_pref_set_summary (GtkWidget *summary, const char *text)
{
  gtk_label_set_label (GTK_LABEL (summary), text ? text : "");
  show_summary (summary, text && *text);
}

void
cal_pref_set_summary_markup (GtkWidget *summary, const char *markup)
{
  gtk_label_set_markup (GTK_LABEL (summary), markup ? markup : "");
  show_summary (summary, markup && *markup);
}

/* a category: its title in the primary colour; an account's with its icon. One after other rows has a
 * divider at the top of the space above it; the first on a page ("first") has none. */
GtkWidget *
cal_pref_category (const char *icon_name, const char *title)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *frame = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *label = gtk_label_new (title);

  gtk_widget_add_css_class (frame, "cal-pref-icon-frame");
  gtk_box_append (GTK_BOX (box), frame);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_add_css_class (label, "cal-pref-category-title");
  vibeos_label_set_font_padding (label, 14);
  gtk_box_append (GTK_BOX (box), label);
  gtk_widget_add_css_class (box, "cal-pref-category");
  gtk_accessible_update_property (GTK_ACCESSIBLE (box), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
  if (icon_name)
    {
      /* The icon is taller than the title and adds nothing to the category's height: an overlay over the row,
       * reaching above the title's top and below its bottom. */
      GtkWidget *overlay = gtk_overlay_new ();
      GtkWidget *icon = cal_pref_icon (icon_name, 0);

      gtk_overlay_set_child (GTK_OVERLAY (overlay), box);
      gtk_widget_set_halign (icon, GTK_ALIGN_START);
      gtk_widget_set_valign (icon, GTK_ALIGN_END);
      gtk_widget_add_css_class (icon, "cal-pref-category-icon");
      gtk_overlay_add_overlay (GTK_OVERLAY (overlay), icon);
      return overlay;
    }
  return box;
}

GtkWidget *
cal_pref_page (GtkWidget *window, const char *title, GtkWidget **list)
{
  GtkWidget *page = cal_page_new (window);
  GtkWidget *scroll = spongy_scroll_view_new ();

  gtk_box_append (GTK_BOX (page), cal_page_toolbar (CAL_PAGE (page), title, NULL));
  *list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (*list, "cal-pref-list");
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), *list);
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_box_append (GTK_BOX (page), scroll);
  return page;
}

/* --- a switch row: a tap anywhere on the row toggles it */

void
cal_pref_toggle_row (VibeosPressable *row, CalCompatSwitch *toggle)
{
  cal_compat_switch_set_active (toggle, !cal_compat_switch_get_active (toggle));
}

GtkWidget *
cal_pref_switch (GSettings *settings, const char *key, const char *title)
{
  GtkWidget *toggle = cal_compat_switch_new ();
  CalPref pref;

  cal_compat_switch_set_active (CAL_COMPAT_SWITCH (toggle), g_settings_get_boolean (settings, key));
  gtk_accessible_update_property (GTK_ACCESSIBLE (toggle), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
  g_settings_bind (settings, key, toggle, "active", G_SETTINGS_BIND_DEFAULT);
  pref = cal_pref_row (NULL, 0, title, NULL, toggle);
  g_signal_connect (pref.row, "clicked", G_CALLBACK (cal_pref_toggle_row), toggle);
  g_object_set_data (G_OBJECT (pref.row), "cal-pref-summary", pref.summary);
  return pref.row;
}

/* --- a list row: the entry as the summary, a dialog of the entries */

struct _CalListPref {
  GSettings *settings;
  char *key, *title;
  gboolean is_string;
  GPtrArray *labels;            /* the entries */
  GPtrArray *values;            /* the values, as strings */
  GtkWidget *summary;
  CalListChanged changed;
  gpointer changed_data;
};

static void
list_pref_free (gpointer data)
{
  CalListPref *list = data;

  g_object_unref (list->settings);
  g_free (list->key);
  g_free (list->title);
  g_ptr_array_unref (list->labels);
  g_ptr_array_unref (list->values);
  g_free (list);
}

static char *
list_value (CalListPref *list)
{
  return list->is_string ? g_settings_get_string (list->settings, list->key)
                         : g_strdup_printf ("%d", g_settings_get_int (list->settings, list->key));
}

static int
list_index (CalListPref *list)
{
  g_autofree char *value = list_value (list);

  for (guint i = 0; i < list->values->len; i++)
    if (g_str_equal (list->values->pdata[i], value))
      return (int) i;
  return -1;
}

static void
show_list_summary (CalListPref *list)
{
  int index = list_index (list);

  cal_pref_set_summary (list->summary, index >= 0 ? list->labels->pdata[index] : NULL);
}

static void
on_list_chosen (int index, gpointer data)
{
  CalListPref *list = data;

  if (index < 0)
    return;
  if (list->is_string)
    g_settings_set_string (list->settings, list->key, list->values->pdata[index]);
  else
    g_settings_set_int (list->settings, list->key, atoi (list->values->pdata[index]));
  show_list_summary (list);
  if (list->changed)
    list->changed (list, list->changed_data);
}

static void
on_list_row (VibeosPressable *row, CalListPref *list)
{
  cal_list_dialog (GTK_WIDGET (row), list->title, (const char *const *) list->labels->pdata, (int) list->labels->len,
                   list_index (list), on_list_chosen, list);
}

CalListPref *
cal_list_pref_new (GSettings *settings, const char *key, const char *title, gboolean is_string)
{
  CalListPref *list = g_new0 (CalListPref, 1);

  list->settings = g_object_ref (settings);
  list->key = g_strdup (key);
  list->title = g_strdup (title);
  list->is_string = is_string;
  list->labels = g_ptr_array_new_with_free_func (g_free);
  list->values = g_ptr_array_new_with_free_func (g_free);
  return list;
}

void
cal_list_pref_add (CalListPref *list, char *label, char *value)
{
  g_ptr_array_add (list->labels, label);
  g_ptr_array_add (list->values, value);
}

GtkWidget *
cal_list_pref_widget (CalListPref *list)
{
  CalPref pref = cal_pref_row (NULL, 0, list->title, NULL, NULL);

  list->summary = pref.summary;
  show_list_summary (list);
  g_object_set_data_full (G_OBJECT (pref.row), "cal-list", list, list_pref_free);
  g_signal_connect (pref.row, "clicked", G_CALLBACK (on_list_row), list);
  return pref.row;
}

/* a reminder time's label: "None" below 0 */
char *
cal_pref_minutes_label (int minutes)
{
  return minutes < 0 ? g_strdup (_("None")) : cal_format_reminder_minutes (minutes);
}

GtkWidget *
cal_pref_minutes (GSettings *settings, const char *key, const char *title, const int *values, int n_values)
{
  CalListPref *list = cal_list_pref_new (settings, key, title, FALSE);

  for (int i = 0; i < n_values; i++)
    cal_list_pref_add (list, cal_pref_minutes_label (values[i]), g_strdup_printf ("%d", values[i]));
  return cal_list_pref_widget (list);
}

void
cal_list_pref_set_changed (CalListPref *list, CalListChanged changed, gpointer data)
{
  list->changed = changed;
  list->changed_data = data;
}
