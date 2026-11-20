/* ClockPage: see clock-page.h. The main clock, digital or analog, with the date and the next alarm under it, then
 * a card for each city chosen. */
#include "clock-page.h"
#include "alarms-client.h"
#include "analog-clock.h"
#include "cities.h"
#include "clock-format.h"
#include "clock-tab.h"

#include <glib/gi18n.h>
#include <vibeos.h>
#include <spongy-gtk.h>

#define FAB_HEIGHT       88     /* the list's bottom padding, so the last card scrolls out from under the button */
#define ANALOG_SIZE      294    /* the main analog clock: a fixed size, not the half of the width its layout seems to
                                 * ask for (that applies only to a stretched width, and the clock keeps its own) */
#define CITY_ANALOG      56     /* a city card's analog clock: its square less the padding at its start */
#define LEFT_PANE        550    /* a tablet held sideways: the main clock in a pane this wide, the cities beside it */

/* one world clock: a city's card, or Home's */
typedef struct
{
  GtkWidget *widget, *time, *hours;
  GTimeZone *tz;
} Card;

struct _ClockPage
{
  GtkWidget parent;
  GSettings *settings;
  GtkWidget *row, *left, *scroller, *list;   /* the row: the main clock's pane (split only), the scrolled list */
  VibeosLayout *layout;
  gboolean split;               /* the main clock on the left, the cities on the right */
  GtkWidget *frame, *time, *analog, *line, *date, *alarm_icon, *alarm;
  GPtrArray *cards;
  gboolean home_shown;
  guint tick;
};

static void clock_page_tab_init (ClockTabInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (ClockPage, clock_page, GTK_TYPE_WIDGET,
                               G_IMPLEMENT_INTERFACE (CLOCK_TYPE_TAB, clock_page_tab_init))

enum { SHOW_CITIES, SHOW_SCREENSAVER, N_SIGNALS };
static guint signals[N_SIGNALS];

static gboolean
is_analog (ClockPage *self)
{
  return g_settings_get_enum (self->settings, "clock-style") == 0;
}

/* --- the main clock */

static void
show_next_alarm (ClockPage *self)
{
  gint64 next = alarms_client_get_next_alarm (alarms_client_get ());
  gboolean shown = next > 0;

  /* the next alarm: hidden, icon and all, when no alarm is set */
  gtk_widget_set_visible (self->alarm_icon, shown);
  gtk_widget_set_visible (self->alarm, shown);
  if (shown)
    {
      g_autoptr(GDateTime) time = g_date_time_new_from_unix_local (next);
      g_autofree char *text = clock_alarm_time_text (time, vibeos_clock_uses_24h ());
      g_autofree char *upper = g_utf8_strup (text, -1);

      gtk_label_set_text (GTK_LABEL (self->alarm), upper);
    }
}

static void
show_main_time (ClockPage *self, GDateTime *now, gboolean seconds)
{
  gboolean use_24h = vibeos_clock_uses_24h ();
  g_autofree char *time = NULL, *date = NULL, *upper = NULL;
  int hour = g_date_time_get_hour (now), minute = g_date_time_get_minute (now);

  /* "Hm" / "Hms", or "hma" / "hmsa" with the AM/PM at 0.4 of the size, bold */
  if (seconds && use_24h)
    time = g_strdup_printf ("%02d:%02d:%02d", hour, minute, g_date_time_get_second (now));
  else if (seconds)
    time = g_strdup_printf ("%d:%02d:%02d <span size='40%%' weight='900'>%s</span>", hour % 12 ? hour % 12 : 12,
                            minute, g_date_time_get_second (now), hour < 12 ? "AM" : "PM");
  else
    time = clock_time_markup (hour, minute, use_24h, 0.4);
  gtk_label_set_markup (GTK_LABEL (self->time), time);

  /* the best "EEEMMMd" pattern, in capitals: "SAT, NOV 7" */
  date = g_date_time_format (now, "%a, %b %-d");
  upper = g_utf8_strup (date, -1);
  gtk_label_set_text (GTK_LABEL (self->date), upper);
}

/* the digital time left-aligned with the date under it, or the analog clock centred with the date centred under it; the
 * seconds go to the one shown */
static void
show_style (ClockPage *self)
{
  gboolean analog = is_analog (self);

  gtk_widget_set_visible (self->time, !analog);
  gtk_widget_set_visible (self->analog, analog);
  gtk_widget_set_halign (self->line, analog ? GTK_ALIGN_CENTER : GTK_ALIGN_START);
  analog_clock_set_seconds (CLOCK_ANALOG_CLOCK (self->analog),
                            analog && g_settings_get_boolean (self->settings, "display-clock-seconds"));
}

/* --- the world clocks */

/* "6 hours ahead", "5 hr 30 min behind", "30 minutes ahead", with "Tomorrow, " or "Yesterday, " in front when the day
 * there is another; NULL when the time is the same */
static char *
hours_text (GTimeZone *tz, GDateTime *local)
{
  g_autoptr(GDateTime) there = g_date_time_to_timezone (local, tz);
  gint64 now = g_date_time_to_unix (local);
  int delta = clock_zone_offset (tz, now) - g_date_time_get_utc_offset (local) / G_USEC_PER_SEC;
  int hours = delta / 3600, minutes = delta / 60 % 60;    /* Java's division: both keep the sign */
  gboolean show_minutes = delta % 3600 != 0;
  gboolean ahead = hours > 0 || (hours == 0 && minutes > 0);
  g_autofree char *amount = NULL, *text = NULL;

  if (hours == 0 && !show_minutes)
    return NULL;
  if (show_minutes && hours != 0)
    {
      g_autofree char *h = g_strdup_printf (g_dngettext (NULL, "%d hr", "%d hr", ABS (hours)), ABS (hours));
      g_autofree char *m = g_strdup_printf (g_dngettext (NULL, "%d min", "%d min", ABS (minutes)), ABS (minutes));

      text = g_strdup_printf (ahead ? _("%s %s ahead") : _("%s %s behind"), h, m);
    }
  else
    {
      if (show_minutes)
        amount = g_strdup_printf (g_dngettext (NULL, "%d minute", "%d minutes", ABS (minutes)), ABS (minutes));
      else
        amount = g_strdup_printf (g_dngettext (NULL, "%d hour", "%d hours", ABS (hours)), ABS (hours));
      text = g_strdup_printf (ahead ? _("%s ahead") : _("%s behind"), amount);
    }
  if (g_date_time_get_day_of_week (there) != g_date_time_get_day_of_week (local))
    return g_strdup_printf (ahead ? _("Tomorrow, %s") : _("Yesterday, %s"), text);
  return g_steal_pointer (&text);
}

static void
show_card (Card *card, GDateTime *local)
{
  g_autofree char *hours = hours_text (card->tz, local);

  gtk_widget_set_visible (card->hours, hours != NULL);
  gtk_label_set_text (GTK_LABEL (card->hours), hours ? hours : "");
  if (card->time)
    {
      g_autoptr(GDateTime) there = g_date_time_to_timezone (local, card->tz);
      g_autofree char *markup = clock_time_markup (g_date_time_get_hour (there), g_date_time_get_minute (there),
                                                   vibeos_clock_uses_24h (), 0.3);

      gtk_label_set_markup (GTK_LABEL (card->time), markup);
    }
}

static void
card_free (gpointer data)
{
  Card *card = data;

  g_time_zone_unref (card->tz);
  g_free (card);
}

/* a city's card: the name over the difference at the start, the time (its AM/PM smaller) or an analog clock
 * without its second hand at the end */
static Card *
make_card (ClockPage *self, const char *name, GTimeZone *tz)
{
  Card *card = g_new0 (Card, 1);
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *label = gtk_label_new (name);

  card->tz = g_time_zone_ref (tz);
  card->widget = row;
  gtk_widget_add_css_class (row, "world-card");

  /* the name on one line, the difference under it on up to two */
  gtk_widget_set_hexpand (column, TRUE);
  gtk_widget_set_valign (column, GTK_ALIGN_CENTER);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode (GTK_LABEL (label), TRUE);
  gtk_widget_add_css_class (label, "body");
  vibeos_label_set_font_padding_sp (label, 14);
  gtk_box_append (GTK_BOX (column), label);
  card->hours = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (card->hours), 0);
  gtk_label_set_wrap (GTK_LABEL (card->hours), TRUE);
  gtk_label_set_lines (GTK_LABEL (card->hours), 2);
  gtk_label_set_ellipsize (GTK_LABEL (card->hours), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (card->hours, "secondary-label");
  vibeos_label_set_font_padding_sp (card->hours, 14);
  gtk_widget_set_margin_top (card->hours, gtk_widget_get_margin_top (card->hours) + 3);
  gtk_box_append (GTK_BOX (column), card->hours);
  gtk_box_append (GTK_BOX (row), column);

  if (is_analog (self))
    {
      GtkWidget *analog = analog_clock_new (CITY_ANALOG);

      /* its square less the padding at its start, a little over the card's bottom padding */
      analog_clock_set_time_zone (CLOCK_ANALOG_CLOCK (analog), tz);
      gtk_widget_set_size_request (analog, CITY_ANALOG, 80);
      gtk_widget_set_margin_start (analog, 24);
      gtk_widget_set_margin_bottom (analog, 6);
      gtk_widget_set_valign (analog, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (row), analog);
    }
  else
    {
      card->time = gtk_label_new (NULL);
      gtk_widget_set_margin_start (card->time, 24);
      gtk_widget_set_valign (card->time, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class (card->time, "world-clock-time");
      vibeos_label_set_font_padding_sp (card->time, 48);
      gtk_box_append (GTK_BOX (row), card->time);
    }
  return card;
}

/* the main clock, Home if it shows, then the chosen cities by time */
static void
show_cards (ClockPage *self)
{
  g_auto(GStrv) ids = g_settings_get_strv (self->settings, "selected-cities");
  g_autoptr(GPtrArray) cities = cities_get_selected ((const char *const *) ids);
  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  /* first: finding home may store it, and that change rebuilds the list in the middle of this call */
  gboolean home_shown = clock_show_home_clock (self->settings);

  for (guint i = 0; i < self->cards->len; i++)
    gtk_box_remove (GTK_BOX (self->list), ((Card *) self->cards->pdata[i])->widget);
  g_ptr_array_set_size (self->cards, 0);

  self->home_shown = home_shown;
  if (self->home_shown)
    {
      g_autoptr(GTimeZone) home = clock_home_zone (self->settings);

      g_ptr_array_add (self->cards, make_card (self, _("Home"), home));
    }
  for (guint i = 0; i < cities->len; i++)
    {
      City *city = cities->pdata[i];

      g_ptr_array_add (self->cards, make_card (self, city->data->name, city->tz));
    }
  for (guint i = 0; i < self->cards->len; i++)
    {
      Card *card = self->cards->pdata[i];

      show_card (card, now);
      gtk_box_append (GTK_BOX (self->list), card->widget);
    }
}

/* --- ticking: the digital time each second or minute, the world clocks each minute */

static gboolean on_tick (gpointer data);

static void
update (ClockPage *self)
{
  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  gboolean seconds = !is_analog (self) && g_settings_get_boolean (self->settings, "display-clock-seconds");
  int ms = 1000 - g_date_time_get_microsecond (now) / 1000;

  show_main_time (self, now, seconds);
  /* the home clock comes and goes as the offsets change (a DST switch on one side) */
  if (clock_show_home_clock (self->settings) != self->home_shown)
    show_cards (self);
  else
    for (guint i = 0; i < self->cards->len; i++)
      show_card (self->cards->pdata[i], now);

  g_clear_handle_id (&self->tick, g_source_remove);
  if (!seconds)
    ms += (59 - g_date_time_get_second (now)) * 1000;
  self->tick = g_timeout_add (ms, on_tick, self);
}

static gboolean
on_tick (gpointer data)
{
  ClockPage *self = data;

  self->tick = 0;
  update (self);
  return G_SOURCE_REMOVE;
}

static void
on_settings (GSettings *settings, const char *key, ClockPage *self)
{
  show_style (self);
  show_cards (self);
  if (gtk_widget_get_mapped (GTK_WIDGET (self)))
    update (self);
}

/* --- ClockTab: the FAB opens the cities */

static void
clock_page_get_buttons (ClockTab *tab, ClockButton *buttons)
{
  buttons[CLOCK_FAB] = (ClockButton) { "clock-public-symbolic", _("Cities") };
}

static void
clock_page_clicked (ClockTab *tab, ClockButtonPosition button)
{
  if (button == CLOCK_FAB)
    g_signal_emit (tab, signals[SHOW_CITIES], 0);
}

static void
clock_page_tab_init (ClockTabInterface *iface)
{
  iface->get_buttons = clock_page_get_buttons;
  iface->clicked = clock_page_clicked;
}

/* --- the widget */

/* a tablet held sideways: the main clock moves out of the list into its own pane on the left, and the list beside it
 * has the cities only; otherwise the main clock heads the list */
static void
on_layout_changed (VibeosLayout *layout, ClockPage *self)
{
  gboolean split = vibeos_layout_is_tablet (layout) && vibeos_layout_is_landscape (layout);

  if (split == self->split)
    return;
  self->split = split;
  g_object_ref (self->frame);
  if (split)
    {
      gtk_box_remove (GTK_BOX (self->list), self->frame);
      gtk_box_append (GTK_BOX (self->left), self->frame);
    }
  else
    {
      gtk_box_remove (GTK_BOX (self->left), self->frame);
      gtk_box_prepend (GTK_BOX (self->list), self->frame);
    }
  g_object_unref (self->frame);
  gtk_widget_set_visible (self->left, split);
  gtk_widget_set_margin_start (self->frame, split ? 16 : 24);
}

static void
clock_page_root (GtkWidget *widget)
{
  ClockPage *self = CLOCK_PAGE (widget);

  GTK_WIDGET_CLASS (clock_page_parent_class)->root (widget);
  self->layout = vibeos_layout_get (widget);
  if (self->layout)
    {
      g_object_ref (self->layout);
      g_signal_connect (self->layout, "changed", G_CALLBACK (on_layout_changed), self);
      on_layout_changed (self->layout, self);
    }
}

static void
clock_page_unroot (GtkWidget *widget)
{
  ClockPage *self = CLOCK_PAGE (widget);

  if (self->layout)
    {
      g_signal_handlers_disconnect_by_data (self->layout, self);
      g_clear_object (&self->layout);
    }
  GTK_WIDGET_CLASS (clock_page_parent_class)->unroot (widget);
}

static void
clock_page_map (GtkWidget *widget)
{
  ClockPage *self = CLOCK_PAGE (widget);

  GTK_WIDGET_CLASS (clock_page_parent_class)->map (widget);
  /* the zone, the home clock or the choice may have changed while it was away */
  show_cards (self);
  update (self);
  show_next_alarm (self);
}

static void
clock_page_unmap (GtkWidget *widget)
{
  ClockPage *self = CLOCK_PAGE (widget);

  /* a hidden tab does not tick */
  g_clear_handle_id (&self->tick, g_source_remove);
  GTK_WIDGET_CLASS (clock_page_parent_class)->unmap (widget);
}

static void
clock_page_dispose (GObject *object)
{
  ClockPage *self = CLOCK_PAGE (object);

  g_clear_handle_id (&self->tick, g_source_remove);
  g_signal_handlers_disconnect_by_data (alarms_client_get (), self);
  g_clear_pointer (&self->row, gtk_widget_unparent);
  self->scroller = NULL;
  g_clear_pointer (&self->cards, g_ptr_array_unref);
  if (self->settings)
    g_signal_handlers_disconnect_by_data (self->settings, self);
  g_clear_object (&self->settings);
  G_OBJECT_CLASS (clock_page_parent_class)->dispose (object);
}

static void
clock_page_class_init (ClockPageClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = clock_page_dispose;
  widget_class->map = clock_page_map;
  widget_class->unmap = clock_page_unmap;
  widget_class->root = clock_page_root;
  widget_class->unroot = clock_page_unroot;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "clock-page");
  signals[SHOW_CITIES] = g_signal_new ("show-cities", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                       NULL, G_TYPE_NONE, 0);
  signals[SHOW_SCREENSAVER] = g_signal_new ("show-screensaver", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                            NULL, NULL, NULL, G_TYPE_NONE, 0);
}

/* a long press anywhere on the tab, the main clock or the cities, starts the screen saver. Moving the finger first is a
 * scroll, which takes the touch from it */
static void
on_long_press (GtkGestureLongPress *gesture, double x, double y, ClockPage *self)
{
  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  g_signal_emit (self, signals[SHOW_SCREENSAVER], 0);
}

static void
clock_page_init (ClockPage *self)
{
  GtkGesture *long_press = gtk_gesture_long_press_new ();

  self->cards = g_ptr_array_new_with_free_func (card_free);
  /* a long press of 400 ms, against GTK's 500 */
  gtk_gesture_long_press_set_delay_factor (GTK_GESTURE_LONG_PRESS (long_press), 0.8);
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (long_press), GTK_PHASE_CAPTURE);
  g_signal_connect (long_press, "pressed", G_CALLBACK (on_long_press), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (long_press));

  /* the list: the main clock, then the cards, room under the last */
  self->list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_bottom (self->list, FAB_HEIGHT);
  self->scroller = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (self->scroller), self->list);
  gtk_widget_set_hexpand (self->scroller, TRUE);
  self->left = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_size_request (self->left, LEFT_PANE, -1);
  gtk_widget_set_visible (self->left, FALSE);
  self->row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append (GTK_BOX (self->row), self->left);
  gtk_box_append (GTK_BOX (self->row), self->scroller);
  gtk_widget_set_parent (self->row, GTK_WIDGET (self));

  /* the main clock's frame */
  self->frame = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_start (self->frame, 24);
  gtk_widget_set_margin_end (self->frame, 24);
  gtk_widget_set_margin_bottom (self->frame, 14);
  gtk_box_append (GTK_BOX (self->list), self->frame);

  /* the time: no font padding, left-aligned for the digital style */
  self->time = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (self->time), 0);
  gtk_widget_set_margin_top (self->time, 24);
  /* GTK rounds the line up to 106 px (90 px × 2400/2048 = 105.5), where the line is meant to be 316 device pixels
   * (105.3 px), so the date would sit 2 device pixels low */
  gtk_widget_set_margin_bottom (self->time, -1);
  gtk_widget_add_css_class (self->time, "main-clock");
  gtk_box_append (GTK_BOX (self->frame), self->time);

  /* or the analog clock, centred */
  self->analog = analog_clock_new (ANALOG_SIZE);
  gtk_widget_set_halign (self->analog, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (self->analog, 16);
  gtk_box_append (GTK_BOX (self->frame), self->analog);

  /* the date, then the alarm glyph and the next alarm */
  self->line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  self->date = gtk_label_new (NULL);
  gtk_widget_add_css_class (self->date, "body");
  vibeos_label_set_font_padding_sp (self->date, 14);
  self->alarm_icon = gtk_image_new_from_icon_name ("clock-alarm-small-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (self->alarm_icon), 14);
  gtk_widget_set_margin_start (self->alarm_icon, 6);
  gtk_widget_set_margin_end (self->alarm_icon, 6);
  gtk_widget_add_css_class (self->alarm_icon, "next-alarm");
  self->alarm = gtk_label_new (NULL);
  gtk_widget_add_css_class (self->alarm, "body");
  vibeos_label_set_font_padding_sp (self->alarm, 14);
  gtk_box_append (GTK_BOX (self->line), self->date);
  gtk_box_append (GTK_BOX (self->line), self->alarm_icon);
  gtk_box_append (GTK_BOX (self->line), self->alarm);
  gtk_box_append (GTK_BOX (self->frame), self->line);
}

GtkWidget *
clock_page_new (GSettings *settings)
{
  ClockPage *self = g_object_new (CLOCK_TYPE_PAGE, NULL);
  const char *keys[] = { "clock-style", "display-clock-seconds", "selected-cities", "automatic-home-clock",
                         "home-time-zone" };

  self->settings = g_object_ref (settings);
  for (guint i = 0; i < G_N_ELEMENTS (keys); i++)
    {
      g_autofree char *signal = g_strconcat ("changed::", keys[i], NULL);

      g_signal_connect (settings, signal, G_CALLBACK (on_settings), self);
    }
  g_signal_connect_swapped (alarms_client_get (), "next-alarm-changed", G_CALLBACK (show_next_alarm), self);
  show_style (self);
  return GTK_WIDGET (self);
}
