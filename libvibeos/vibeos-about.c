/* vibeos-about: see vibeos-about.h. The sheet's first page is built at once; Credits and Legal are built when they
 * are opened, from a copy of the app's sections kept on the sheet. */
#include "vibeos-about.h"
#include "vibeos-card-list.h"
#include "vibeos-list-row.h"
#include "vibeos-sheet.h"

#include <spongy-gtk.h>

/* debian/changelog's newest version, without its epoch: meson.build passes it in */
#ifndef VIBEOS_RELEASE
#define VIBEOS_RELEASE ""
#endif

#define DEVELOPER     "VibeOS Project"
#define WEBSITE       "https://vibeos.top"
#define ISSUES        "https://github.com/pulpoff/vibeos/issues"
#define LICENSE       "https://github.com/pulpoff/vibeos/blob/main/LICENSE"
#define ICON_SIZE     128
#define LINK_ICON     "vibeos-external-link-symbolic"

/* what every app's Credits and Legal say first: title, text, ..., NULL */
static const char *const vibeos_credits[] = {
  "Developed by",
  "The VibeOS Project\n<a href=\"https://github.com/pulpoff/vibeos\">github.com/pulpoff/vibeos</a>",
  "Built on",
  "Debian, GNOME and GTK.",
  NULL,
};

static const char *const vibeos_legal[] = {
  "VibeOS",
  "© 2026 pulpoff\n\n"
  "VibeOS is free software under the <a href=\"" LICENSE "\">MIT License</a>. It comes with absolutely no warranty.\n\n"
  "Some of its icons and images come from the Android Open Source Project and LineageOS, under the Apache License "
  "2.0.",
  NULL,
};

/* the app's sections, kept for the pages: title, text, title, text, ... */
typedef struct {
  GPtrArray *credits, *legal;
} About;

static void
about_free (gpointer data)
{
  About *about = data;

  g_ptr_array_unref (about->credits);
  g_ptr_array_unref (about->legal);
  g_free (about);
}

static GPtrArray *
copy_sections (const VibeosAboutSection *sections)
{
  GPtrArray *copy = g_ptr_array_new_with_free_func (g_free);

  for (const VibeosAboutSection *s = sections; s && s->title; s++)
    {
      g_ptr_array_add (copy, g_strdup (s->title));
      g_ptr_array_add (copy, g_strdup (s->text ? s->text : ""));
    }
  return copy;
}

const char *
vibeos_release (void)
{
  return VIBEOS_RELEASE;
}

/* ---- the parts ------------------------------------------------------------------------------------------------- */

static GtkWidget *
scrolled (GtkWidget *child)
{
  GtkWidget *scroll = spongy_scroll_view_new ();

  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), child);
  return scroll;
}

static void
on_launched (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr (GError) error = NULL;

  if (!gtk_uri_launcher_launch_finish (GTK_URI_LAUNCHER (source), result, &error)
      && !g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
    g_message ("About: %s", error->message);
}

/* a web page, in the browser, over the window `widget` is in */
static void
open_uri (GtkWidget *widget, const char *uri)
{
  g_autoptr (GtkUriLauncher) launcher = gtk_uri_launcher_new (uri);
  GtkRoot *root = gtk_widget_get_root (widget);

  gtk_uri_launcher_launch (launcher, GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL, NULL, on_launched, NULL);
}

static void
on_link_row (VibeosListRow *row, gpointer uri)
{
  open_uri (GTK_WIDGET (row), uri);
}

static GtkWidget *
add_row (VibeosCardList *list, const char *title, const char *end_icon)
{
  GtkWidget *row = vibeos_list_row_new (title, NULL);

  if (end_icon)
    vibeos_list_row_set_end_icon (VIBEOS_LIST_ROW (row), end_icon);
  else
    vibeos_list_row_set_chevron (VIBEOS_LIST_ROW (row));
  vibeos_card_list_append (list, row);
  return row;
}

/* a section's text on its card; its links open in the browser (GtkLabel's own handling) */
static GtkWidget *
text_card (const char *markup)
{
  GtkWidget *label = gtk_label_new (NULL);

  gtk_label_set_markup (GTK_LABEL (label), markup);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
  gtk_widget_add_css_class (label, "vibeos-sheet-text");
  return label;
}

/* Credits or Legal: VibeOS's sections, then the app's, each a header over a card */
static GtkWidget *
sections_page (const char *const *own, GPtrArray *app)
{
  GtkWidget *list = vibeos_card_list_new ();

  vibeos_card_list_set_margins (VIBEOS_CARD_LIST (list), 0, 24);
  for (guint i = 0; own[i] && own[i + 1]; i += 2)
    {
      vibeos_card_list_add_group (VIBEOS_CARD_LIST (list), own[i]);
      vibeos_card_list_append (VIBEOS_CARD_LIST (list), text_card (own[i + 1]));
    }
  for (guint i = 0; i + 1 < app->len; i += 2)
    {
      vibeos_card_list_add_group (VIBEOS_CARD_LIST (list), g_ptr_array_index (app, i));
      vibeos_card_list_append (VIBEOS_CARD_LIST (list), text_card (g_ptr_array_index (app, i + 1)));
    }
  return scrolled (list);
}

static About *
about_of (GtkWidget *row)
{
  return g_object_get_data (G_OBJECT (gtk_widget_get_ancestor (row, VIBEOS_TYPE_SHEET)), "vibeos-about");
}

static void
on_credits (VibeosListRow *row, VibeosSheet *sheet)
{
  vibeos_sheet_push (sheet, "Credits", sections_page (vibeos_credits, about_of (GTK_WIDGET (row))->credits));
}

static void
on_legal (VibeosListRow *row, VibeosSheet *sheet)
{
  vibeos_sheet_push (sheet, "Legal", sections_page (vibeos_legal, about_of (GTK_WIDGET (row))->legal));
}

/* the first page: the app, then the rows, in libadwaita's groups (its website; its issues; credits and legal) */
static GtkWidget *
main_page (VibeosSheet *sheet, const VibeosAboutInfo *info)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *icon = gtk_image_new_from_icon_name (info->icon_name);
  GtkWidget *name = gtk_label_new (info->name);
  GtkWidget *developer = gtk_label_new (DEVELOPER);
  GtkWidget *version = gtk_label_new (vibeos_release ());
  GtkWidget *list = vibeos_card_list_new ();
  VibeosCardList *cards = VIBEOS_CARD_LIST (list);
  GtkWidget *row;

  gtk_image_set_pixel_size (GTK_IMAGE (icon), ICON_SIZE);
  gtk_widget_add_css_class (icon, "vibeos-about-icon");
  gtk_box_append (GTK_BOX (box), icon);

  gtk_label_set_wrap (GTK_LABEL (name), TRUE);
  gtk_label_set_justify (GTK_LABEL (name), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class (name, "vibeos-about-name");
  gtk_accessible_update_property (GTK_ACCESSIBLE (name), GTK_ACCESSIBLE_PROPERTY_LEVEL, 1, -1);
  gtk_box_append (GTK_BOX (box), name);

  gtk_label_set_wrap (GTK_LABEL (developer), TRUE);
  gtk_label_set_justify (GTK_LABEL (developer), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class (developer, "vibeos-about-developer");
  gtk_box_append (GTK_BOX (box), developer);

  /* the version in a pill, as libadwaita's */
  gtk_widget_set_halign (version, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (version, "vibeos-about-version");
  gtk_widget_set_visible (version, *vibeos_release () != '\0');
  gtk_box_append (GTK_BOX (box), version);

  vibeos_card_list_set_margins (cards, 24, 16);
  row = add_row (cards, "Website", LINK_ICON);
  g_signal_connect (row, "clicked", G_CALLBACK (on_link_row), (gpointer) WEBSITE);
  vibeos_card_list_add_group (cards, NULL);
  row = add_row (cards, "Report an Issue", LINK_ICON);
  g_signal_connect (row, "clicked", G_CALLBACK (on_link_row), (gpointer) ISSUES);
  vibeos_card_list_add_group (cards, NULL);
  row = add_row (cards, "Credits", NULL);
  g_signal_connect (row, "clicked", G_CALLBACK (on_credits), sheet);
  row = add_row (cards, "Legal", NULL);
  g_signal_connect (row, "clicked", G_CALLBACK (on_legal), sheet);
  gtk_box_append (GTK_BOX (box), list);

  return scrolled (box);
}

GtkWidget *
vibeos_about_present (GtkWidget *parent, const VibeosAboutInfo *info)
{
  g_autofree char *label = NULL;
  GtkWidget *sheet;
  About *about;

  g_return_val_if_fail (GTK_IS_WIDGET (parent), NULL);
  g_return_val_if_fail (info != NULL && info->name != NULL, NULL);

  sheet = vibeos_sheet_new (NULL);
  label = g_strdup_printf ("About %s", info->name);
  gtk_accessible_update_property (GTK_ACCESSIBLE (sheet), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  about = g_new0 (About, 1);
  about->credits = copy_sections (info->credits);
  about->legal = copy_sections (info->legal);
  g_object_set_data_full (G_OBJECT (sheet), "vibeos-about", about, about_free);
  gtk_widget_add_css_class (sheet, "vibeos-about");
  vibeos_sheet_set_content (VIBEOS_SHEET (sheet), main_page (VIBEOS_SHEET (sheet), info));
  vibeos_sheet_present (VIBEOS_SHEET (sheet), parent);
  return sheet;
}
