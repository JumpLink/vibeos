/* AlarmPage: see alarm-page.h. */
#include "alarm-page.h"
#include "alarm-card.h"
#include "alarms-client.h"
#include "clock-format.h"
#include "clock-tab.h"

#include <glib/gi18n.h>
#include <spongy-gtk.h>

#define FAB_HEIGHT    88        /* the list's room at the bottom, so the last card can scroll out from under the FAB */
#define EMPTY_FADE_MS 300       /* "No Alarms" and the list: one fades out, then the other in */

struct _AlarmPage
{
  GtkWidget parent;
  GSettings *settings;
  NeoSnackbar *snackbar;
  AlarmsClient *client;

  GtkWidget *scroller, *list, *empty;
  gboolean loaded;              /* the first list came: cards after it animate in */
  guint expanded;               /* the open card's alarm, or 0 */
  guint show_when_loaded;       /* an alarm to open once it is in the list (a new one) */
  gboolean showing_empty;
  NeoAnim *empty_fade;
};

static void alarm_page_tab_init (ClockTabInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (AlarmPage, alarm_page, GTK_TYPE_WIDGET,
                               G_IMPLEMENT_INTERFACE (CLOCK_TYPE_TAB, alarm_page_tab_init))

static AlarmCard *
find_card (AlarmPage *self, guint id)
{
  for (GtkWidget *child = gtk_widget_get_first_child (self->list); child; child = gtk_widget_get_next_sibling (child))
    if (ALARM_IS_CARD (child) && gtk_widget_get_sensitive (child) && alarm_card_get_id (ALARM_CARD (child)) == id)
      return ALARM_CARD (child);
  return NULL;
}

/* --- scrolling and expanding */

static gboolean
scroll_to_card (gpointer data)
{
  AlarmCard *card = data;
  GtkWidget *scroller = gtk_widget_get_ancestor (GTK_WIDGET (card), SPONGY_TYPE_SCROLL_VIEW);
  graphene_point_t top;

  /* the card's top (with its margin) at the top of the list, as far as
   * the list can scroll */
  if (scroller && gtk_widget_compute_point (GTK_WIDGET (card), gtk_widget_get_parent (GTK_WIDGET (card)),
                                            &GRAPHENE_POINT_INIT (0, 0), &top))
    {
      GtkAdjustment *adjustment = spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (scroller));
      double max = gtk_adjustment_get_upper (adjustment) - gtk_adjustment_get_page_size (adjustment);

      spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (scroller), CLAMP (top.y, 0, MAX (max, 0)));
    }
  g_object_unref (card);
  return G_SOURCE_REMOVE;
}

static void
expand (AlarmPage *self, AlarmCard *card)
{
  AlarmCard *old = self->expanded ? find_card (self, self->expanded) : NULL;

  /* one card open at a time: the old one closes as the new one opens */
  if (old && old != card)
    alarm_card_set_expanded (old, FALSE, TRUE);
  alarm_card_set_expanded (card, TRUE, TRUE);
  self->expanded = alarm_card_get_id (card);
  /* after the layout has the card where it ends up */
  g_idle_add_full (G_PRIORITY_LOW, scroll_to_card, g_object_ref (card), NULL);
}

static void
on_toggle_expanded (AlarmCard *card, AlarmPage *self)
{
  if (alarm_card_get_expanded (card))
    {
      alarm_card_set_expanded (card, FALSE, TRUE);
      self->expanded = 0;
    }
  else
    expand (self, card);
}

/* --- snackbars */

static void
on_message (AlarmCard *card, const char *text, AlarmPage *self)
{
  neo_snackbar_show (self->snackbar, text, NULL, NULL, NULL, NEO_SNACKBAR_SHORT);
}

static void
undo_delete (gpointer data)
{
  ClockAlarm *alarm = data;

  /* the same alarm again, under a new id: the delete undone */
  alarms_client_add (alarms_client_get (), alarm, NULL, NULL);
}

enum { PICK_RINGTONE, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
on_pick_ringtone (AlarmCard *card, AlarmPage *self)
{
  g_signal_emit (self, signals[PICK_RINGTONE], 0, alarm_card_get_id (card));
}

static void
on_deleted (AlarmCard *card, ClockAlarm *alarm, AlarmPage *self)
{
  static ClockAlarm *deleted;

  /* the snackbar's undo needs the alarm after the card is gone; one can be undone at a time */
  g_clear_pointer (&deleted, clock_alarm_free);
  deleted = g_memdup2 (alarm, sizeof *alarm);
  deleted->label = g_strdup (alarm->label);
  deleted->ringtone = g_strdup (alarm->ringtone);
  if (self->expanded == alarm->id)
    self->expanded = 0;
  alarms_client_delete (self->client, alarm->id);
  neo_snackbar_show (self->snackbar, _("Alarm deleted"), _("undo"), undo_delete, deleted, NEO_SNACKBAR_LONG);
}

/* --- the empty state */

static void
on_empty_fade (NeoAnim *anim, double value, gpointer data)
{
  AlarmPage *self = data;
  /* the run is 0..2: the old view fades out over the first half, the new one in over the second */
  double out = 1 - MIN (value, 1), in = MAX (value - 1, 0);

  gtk_widget_set_opacity (self->showing_empty ? self->scroller : self->empty, out);
  gtk_widget_set_opacity (self->showing_empty ? self->empty : self->scroller, in);
  gtk_widget_set_child_visible (self->empty, self->showing_empty || value < 1);
}

static void
show_empty (AlarmPage *self, gboolean empty, gboolean animate)
{
  if (empty == self->showing_empty)
    return;
  self->showing_empty = empty;
  if (animate)
    neo_anim_start (self->empty_fade, 0, 2, 2 * EMPTY_FADE_MS, 0, SPONGY_ACCELERATE_DECELERATE);
  else
    on_empty_fade (self->empty_fade, 2, self);
}

/* --- the list from the service */

static void
on_card_gone (AlarmCard *card, AlarmPage *self)
{
  gtk_box_remove (GTK_BOX (self->list), GTK_WIDGET (card));
}

static void
on_changed (AlarmsClient *client, AlarmPage *self)
{
  GPtrArray *alarms = alarms_client_get_alarms (client);
  GtkWidget *child, *next, *previous = NULL;

  /* cards whose alarm is gone fade out and close up */
  for (child = gtk_widget_get_first_child (self->list); child; child = next)
    {
      next = gtk_widget_get_next_sibling (child);
      if (gtk_widget_get_sensitive (child) && !alarms_client_find (client, alarm_card_get_id (ALARM_CARD (child))))
        {
          if (self->loaded)
            alarm_card_disappear (ALARM_CARD (child), G_CALLBACK (on_card_gone), self);
          else
            gtk_box_remove (GTK_BOX (self->list), child);
        }
    }

  /* the others in the service's order, the new ones opening their space */
  for (guint i = 0; i < alarms->len; i++)
    {
      ClockAlarm *alarm = g_ptr_array_index (alarms, i);
      AlarmCard *card = find_card (self, alarm->id);

      if (card)
        alarm_card_update (card, alarm);
      else
        {
          card = ALARM_CARD (alarm_card_new (alarm, self->settings));
          g_signal_connect (card, "toggle-expanded", G_CALLBACK (on_toggle_expanded), self);
          g_signal_connect (card, "message", G_CALLBACK (on_message), self);
          g_signal_connect (card, "deleted", G_CALLBACK (on_deleted), self);
          g_signal_connect (card, "pick-ringtone", G_CALLBACK (on_pick_ringtone), self);
          gtk_box_append (GTK_BOX (self->list), GTK_WIDGET (card));
          if (self->loaded)
            alarm_card_appear (card);
        }
      gtk_box_reorder_child_after (GTK_BOX (self->list), GTK_WIDGET (card), previous);
      previous = GTK_WIDGET (card);
    }

  show_empty (self, alarms->len == 0, self->loaded);
  self->loaded = TRUE;
  if (self->show_when_loaded)
    {
      guint id = self->show_when_loaded;

      self->show_when_loaded = 0;
      alarm_page_show_alarm (self, id);
    }
}

void
alarm_page_show_alarm (AlarmPage *self, guint id)
{
  AlarmCard *card;

  if (!self->loaded)
    {
      self->show_when_loaded = id;
      return;
    }
  card = find_card (self, id);
  if (card)
    expand (self, card);
  else
    neo_snackbar_show (self->snackbar, _("Missed alarm has been deleted"), NULL, NULL, NULL, NEO_SNACKBAR_LONG);
}

/* --- adding */

static void
on_added (guint id, gpointer data)
{
  AlarmPage *self = data;

  /* opened and scrolled to as soon as the list has it */
  if (find_card (self, id))
    alarm_page_show_alarm (self, id);
  else
    self->show_when_loaded = id;
  g_object_unref (self);
}

static void
on_new_time (int hour, int minute, gpointer data)
{
  AlarmPage *self = data;
  /* Alarm's defaults: on, vibrating, once, no label, the default sound */
  ClockAlarm alarm = { .hour = hour, .minute = minute, .enabled = TRUE, .vibrate = TRUE, .label = (char *) "" };
  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  g_autoptr(GDateTime) today = g_date_time_new_local (g_date_time_get_year (now), g_date_time_get_month (now),
                                                      g_date_time_get_day_of_month (now), hour, minute, 0);
  g_autoptr(GDateTime) next = g_date_time_compare (today, now) > 0 ? g_date_time_ref (today)
                                                                     : g_date_time_add_days (today, 1);
  g_autofree char *text = clock_alarm_set_text (g_date_time_difference (next, now) / G_TIME_SPAN_SECOND);

  alarms_client_add (self->client, &alarm, on_added, g_object_ref (self));
  neo_snackbar_show (self->snackbar, text, NULL, NULL, NULL, NEO_SNACKBAR_SHORT);
}

void
alarm_page_add_alarm (AlarmPage *self)
{
  g_autoptr(GDateTime) now = g_date_time_new_now_local ();

  neo_time_picker_dialog (GTK_WIDGET (self), g_date_time_get_hour (now), g_date_time_get_minute (now), on_new_time,
                          g_object_ref (self), g_object_unref);
}

/* --- the buttons: the FAB adds an alarm */

static void
alarm_page_get_buttons (ClockTab *tab, ClockButton *buttons)
{
  buttons[CLOCK_FAB] = (ClockButton) { "clock-add-symbolic", _("Add alarm") };
}

static void
alarm_page_clicked (ClockTab *tab, ClockButtonPosition button)
{
  if (button == CLOCK_FAB)
    alarm_page_add_alarm (ALARM_PAGE (tab));
}

static void
alarm_page_tab_init (ClockTabInterface *iface)
{
  iface->get_buttons = alarm_page_get_buttons;
  iface->clicked = alarm_page_clicked;
}

/* --- GObject */

static void
alarm_page_dispose (GObject *object)
{
  AlarmPage *self = ALARM_PAGE (object);

  if (self->client)
    g_signal_handlers_disconnect_by_data (self->client, self);
  g_clear_pointer (&self->empty_fade, neo_anim_free);
  g_clear_pointer (&self->scroller, gtk_widget_unparent);
  g_clear_pointer (&self->empty, gtk_widget_unparent);
  g_clear_object (&self->settings);
  G_OBJECT_CLASS (alarm_page_parent_class)->dispose (object);
}

static void
alarm_page_class_init (AlarmPageClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = alarm_page_dispose;
  gtk_widget_class_set_layout_manager_type (GTK_WIDGET_CLASS (klass), GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "alarm-page");
  signals[PICK_RINGTONE] = g_signal_new ("pick-ringtone", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                         NULL, NULL, G_TYPE_NONE, 1, G_TYPE_UINT);
}

static void
alarm_page_init (AlarmPage *self)
{
  GtkWidget *icon, *text;

  /* the list: cards in from the sides, room for the FAB under the last one */
  self->list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_start (self->list, 16);
  gtk_widget_set_margin_end (self->list, 16);
  gtk_widget_set_margin_bottom (self->list, FAB_HEIGHT);
  gtk_widget_set_valign (self->list, GTK_ALIGN_START);
  self->scroller = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (self->scroller), self->list);
  gtk_widget_set_parent (self->scroller, GTK_WIDGET (self));

  /* the empty state: the picture over "No Alarms", both faint */
  self->empty = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign (self->empty, GTK_ALIGN_CENTER);
  gtk_widget_set_halign (self->empty, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_bottom (self->empty, FAB_HEIGHT);
  gtk_widget_add_css_class (self->empty, "no-alarms");
  icon = gtk_image_new_from_icon_name ("clock-noalarms-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (icon), 120);
  text = gtk_label_new (_("No Alarms"));
  gtk_box_append (GTK_BOX (self->empty), icon);
  gtk_box_append (GTK_BOX (self->empty), text);
  gtk_widget_set_parent (self->empty, GTK_WIDGET (self));
  gtk_widget_set_child_visible (self->empty, FALSE);
  self->empty_fade = neo_anim_new (GTK_WIDGET (self), on_empty_fade, self);
}

GtkWidget *
alarm_page_new (GSettings *settings, NeoSnackbar *snackbar)
{
  AlarmPage *self = g_object_new (ALARM_TYPE_PAGE, NULL);

  self->settings = g_object_ref (settings);
  self->snackbar = snackbar;
  self->client = alarms_client_get ();
  g_signal_connect (self->client, "changed", G_CALLBACK (on_changed), self);
  if (alarms_client_get_alarms (self->client)->len)
    on_changed (self->client, self);
  return GTK_WIDGET (self);
}
