/* Settings (msg-settings.h). */
#include "msg-settings.h"
#include "msg-action-bar.h"

#include <glib/gi18n.h>
#include <neo.h>
#include <spongy-gtk.h>

/* androidx.preference 1.3 (§4.6): the icon's space is kept, so the title starts at 72 dp */
#define ROW_MIN_HEIGHT 48
#define PAD 16          /* the row's start and end, the text block's top and bottom, the widget frame's start */
#define TITLE_START 72
#define SUMMARY_LINES 10
#define SUMMARY_OVER 4  /* the summary TextView's 8.9 dp more than its lines, over and under them (measured) */
#define SUMMARY_UNDER 5

struct _MsgSettings {
  GtkWidget parent;
  GSettings *settings;

  GtkWidget *box;
};

G_DEFINE_FINAL_TYPE (MsgSettings, msg_settings, GTK_TYPE_WIDGET)

enum { CLOSED, N_SIGNALS };
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

/* a tap anywhere on the row flips its switch, as a SwitchPreference does */
static void
on_row_clicked (GtkWidget *row, gpointer data)
{
  NeoSwitch *toggle = data;

  neo_switch_set_active (toggle, !neo_switch_get_active (toggle));
}

/* a SwitchPreferenceCompat: the title (and its summary) in the text block, the switch at the end */
static GtkWidget *
switch_row (MsgSettings *self, const char *title, const char *summary, const char *key)
{
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *toggle = neo_switch_new ();
  GtkWidget *row;

  gtk_box_append (GTK_BOX (text), text_label (title, "msg-pref-title"));
  if (summary)
    {
      GtkWidget *label = text_label (summary, "msg-pref-summary");

      gtk_label_set_lines (GTK_LABEL (label), SUMMARY_LINES);
      gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
      gtk_widget_set_margin_top (label, SUMMARY_OVER);
      gtk_widget_set_margin_bottom (label, SUMMARY_UNDER);
      gtk_box_append (GTK_BOX (text), label);
    }
  gtk_widget_set_hexpand (text, TRUE);
  gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (text, TITLE_START);
  gtk_widget_set_margin_top (text, PAD);
  gtk_widget_set_margin_bottom (text, PAD);
  gtk_box_append (GTK_BOX (content), text);

  /* MaterialSwitch without icons, bound to the setting both ways */
  neo_switch_set_thumb_icons (NEO_SWITCH (toggle), FALSE);
  g_settings_bind (self->settings, key, toggle, "active", G_SETTINGS_BIND_DEFAULT);
  gtk_widget_set_valign (toggle, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (toggle, PAD);
  gtk_widget_set_margin_end (toggle, PAD);
  /* the row takes the taps, the switch only shows them */
  gtk_widget_set_can_target (toggle, FALSE);
  gtk_box_append (GTK_BOX (content), toggle);

  row = neo_pressable_new (content);
  gtk_widget_set_size_request (row, -1, ROW_MIN_HEIGHT);
  gtk_accessible_update_property (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
  g_signal_connect (row, "clicked", G_CALLBACK (on_row_clicked), toggle);
  return row;
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
}

static void
msg_settings_init (MsgSettings *self)
{
}

GtkWidget *
msg_settings_new (void)
{
  MsgSettings *self = g_object_new (MSG_TYPE_SETTINGS, NULL);
  GtkWidget *bar = msg_action_bar_new (TRUE, _("Settings"));
  GtkWidget *scroll = spongy_scroll_view_new ();
  GtkWidget *list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  self->settings = g_settings_new ("de.yesman.Messaging");
  self->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent (self->box, GTK_WIDGET (self));
  gtk_widget_add_css_class (self->box, "msg-page");
  g_signal_connect_swapped (msg_action_bar_get_up (bar), "clicked", G_CALLBACK (on_up), self);
  gtk_box_append (GTK_BOX (self->box), bar);

  /* preferences_application.xml's order, less what msg-settings.h leaves out */
  gtk_box_append (GTK_BOX (list), switch_row (self, _("Outgoing message sounds"), NULL, "send-sound"));
  gtk_box_append (GTK_BOX (list), switch_row (self, _("Swipe to delete"),
                                              _("Swipe to the right to delete a conversation"),
                                              "swipe-right-deletes"));
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), list);
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_box_append (GTK_BOX (self->box), scroll);
  return GTK_WIDGET (self);
}
