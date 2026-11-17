/* Settings (msg-settings.h). */
#include "msg-settings.h"
#include "msg-action-bar.h"
#include "msg-card.h"

#include <glib/gi18n.h>
#include <vibeos.h>
#include <spongy-gtk.h>

/* the rows, on segmented cards */
#define SIDE 12         /* the cards' margin from the sides */
#define ROW_MIN_HEIGHT 55
#define PAD 16          /* the text's inset in its card, the switch's from the card's end */
#define TEXT_PAD 15     /* over and under the text */
#define SUMMARY_LINES 10
#define LIST_TOP 0
#define LIST_BOTTOM 16

struct _MsgSettings {
  GtkWidget parent;
  GSettings *settings;

  GtkWidget *box;
};

G_DEFINE_FINAL_TYPE (MsgSettings, msg_settings, GTK_TYPE_WIDGET)

enum { CLOSED, SHOW_ADVANCED, N_SIGNALS };
static guint signals[N_SIGNALS];

static GtkWidget *
text_label (const char *text, const char *css_class)
{
  GtkWidget *label = gtk_label_new (text);

  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
  gtk_widget_add_css_class (label, css_class);
  return label;
}

/* a tap anywhere on the row flips its switch */
static void
on_row_clicked (GtkWidget *row, gpointer data)
{
  VibeosSwitch *toggle = data;

  vibeos_switch_set_active (toggle, !vibeos_switch_get_active (toggle));
}

/* a settings row: the title (and its summary) in the text block, then @widget at the end, if any; the row takes the
 * taps */
static GtkWidget *
pref_row (const char *title, const char *summary, GtkWidget *widget)
{
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *row;

  gtk_box_append (GTK_BOX (text), text_label (title, "msg-pref-title"));
  if (summary)
    {
      GtkWidget *label = text_label (summary, "msg-pref-summary");

      gtk_label_set_lines (GTK_LABEL (label), SUMMARY_LINES);
      gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
      gtk_box_append (GTK_BOX (text), label);
    }
  gtk_widget_set_hexpand (text, TRUE);
  gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (text, PAD);
  gtk_widget_set_margin_top (text, TEXT_PAD);
  gtk_widget_set_margin_bottom (text, TEXT_PAD);
  /* no widget: the text block runs to the row's end padding */
  if (!widget)
    gtk_widget_set_margin_end (text, PAD);
  gtk_box_append (GTK_BOX (content), text);
  if (widget)
    gtk_box_append (GTK_BOX (content), widget);

  gtk_widget_set_size_request (content, -1, ROW_MIN_HEIGHT);
  row = msg_card_row_new (content);
  gtk_accessible_update_property (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
  return row;
}

/* a switch row: the title (and its summary) in the text block, the switch at the end */
static GtkWidget *
switch_row (MsgSettings *self, const char *title, const char *summary, const char *key)
{
  GtkWidget *toggle = vibeos_switch_new ();
  GtkWidget *row;

  /* a switch with its thumb's ✓ and ✕, bound to the setting both ways */
  vibeos_switch_set_thumb_icons (VIBEOS_SWITCH (toggle), TRUE);
  g_settings_bind (self->settings, key, toggle, "active", G_SETTINGS_BIND_DEFAULT);
  gtk_widget_set_valign (toggle, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (toggle, PAD);
  gtk_widget_set_margin_end (toggle, PAD);
  /* the row takes the taps, the switch only shows them */
  gtk_widget_set_can_target (toggle, FALSE);

  row = pref_row (title, summary, toggle);
  g_signal_connect (row, "clicked", G_CALLBACK (on_row_clicked), toggle);
  return row;
}

static void
on_advanced_clicked (GtkWidget *row, gpointer data)
{
  g_signal_emit (data, signals[SHOW_ADVANCED], 0);
}

static void
on_up (MsgSettings *self)
{
  g_signal_emit (self, signals[CLOSED], 0);
}

static void
msg_settings_dispose (GObject *object)
{
  MsgSettings *self = MSG_SETTINGS (object);

  g_clear_pointer (&self->box, gtk_widget_unparent);
  g_clear_object (&self->settings);
  G_OBJECT_CLASS (msg_settings_parent_class)->dispose (object);
}

static void
msg_settings_class_init (MsgSettingsClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = msg_settings_dispose;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "msg-settings");
  signals[CLOSED] = g_signal_new ("closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 0);
  signals[SHOW_ADVANCED] = g_signal_new ("show-advanced", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                         NULL, G_TYPE_NONE, 0);
}

static void
msg_settings_init (MsgSettings *self)
{
}

/* a settings page: @title's bar over a scrolling group of cards, which @list is set to */
static MsgSettings *
settings_page (const char *title, GtkWidget **list)
{
  MsgSettings *self = g_object_new (MSG_TYPE_SETTINGS, NULL);
  GtkWidget *bar = msg_action_bar_new (TRUE, title);
  GtkWidget *scroll = spongy_scroll_view_new ();

  *list = msg_card_group_new ();
  gtk_widget_set_margin_start (*list, SIDE);
  gtk_widget_set_margin_end (*list, SIDE);
  gtk_widget_set_margin_top (*list, LIST_TOP);
  gtk_widget_set_margin_bottom (*list, LIST_BOTTOM);
  self->settings = g_settings_new ("vibeos.Messaging");
  self->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent (self->box, GTK_WIDGET (self));
  gtk_widget_add_css_class (self->box, "msg-settings-page");
  gtk_widget_add_css_class (bar, "msg-page-bar");
  g_signal_connect_swapped (msg_action_bar_get_up (bar), "clicked", G_CALLBACK (on_up), self);
  gtk_box_append (GTK_BOX (self->box), bar);
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), *list);
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_box_append (GTK_BOX (self->box), scroll);
  return self;
}

GtkWidget *
msg_settings_new (void)
{
  GtkWidget *list, *advanced;
  MsgSettings *self = settings_page (_("Messages settings"), &list);

  /* what this app acts on, less what msg-settings.h leaves out */
  msg_card_group_append (list, switch_row (self, _("Hear outgoing message sounds"), NULL, "send-sound"));
  msg_card_group_append (list, switch_row (self, _("Swipe to delete"),
                                           _("Swipe to the right to delete a conversation"),
                                           "swipe-right-deletes"));
  /* the sub-page: the SIM's settings */
  advanced = pref_row (_("Advanced"), _("SMS delivery reports"), NULL);
  g_signal_connect (advanced, "clicked", G_CALLBACK (on_advanced_clicked), self);
  msg_card_group_append (list, advanced);
  return GTK_WIDGET (self);
}

GtkWidget *
msg_settings_new_advanced (void)
{
  GtkWidget *list;
  MsgSettings *self = settings_page (_("Advanced"), &list);

  /* the SIM's settings, less their MMS ones (msg-settings.h) */
  msg_card_group_append (list, switch_row (self, _("SMS delivery reports"),
                                           _("Request a delivery report for each SMS you send"),
                                           "delivery-reports"));
  return GTK_WIDGET (self);
}
