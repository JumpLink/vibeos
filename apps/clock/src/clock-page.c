/* ClockPage: see clock-page.h. Measured on still:clock-main: the time 72…1008 × 308…696 px, the date
 * 72…286 × 696…753 px (the frame's 24 dp margins, the time's 24 dp paddingTop). */
#include "clock-page.h"
#include "alarms-client.h"
#include "clock-format.h"

#include <glib/gi18n.h>
#include <neo.h>

#define MAIN_CLOCK_SIZE 90      /* main_clock_frame: display_time at 90 sp */

struct _ClockPage
{
  GtkWidget parent;
  GSettings *settings;
  GtkWidget *frame, *time, *date, *alarm_icon, *alarm;
  guint tick;
};

G_DEFINE_FINAL_TYPE (ClockPage, clock_page, GTK_TYPE_WIDGET)

static void
show_next_alarm (ClockPage *self)
{
  gint64 next = alarms_client_get_next_alarm (alarms_client_get ());
  gboolean shown = next > 0;

  /* Utils.refreshAlarm: hidden, icon and all, when no alarm is set */
  gtk_widget_set_visible (self->alarm_icon, shown);
  gtk_widget_set_visible (self->alarm, shown);
  if (shown)
    {
      g_autoptr(GDateTime) time = g_date_time_new_from_unix_local (next);
      g_autofree char *text = clock_alarm_time_text (time, neo_clock_uses_24h ());
      g_autofree char *upper = g_utf8_strup (text, -1);

      gtk_label_set_text (GTK_LABEL (self->alarm), upper);
    }
}

static gboolean on_tick (gpointer data);

static void
show_time (ClockPage *self)
{
  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  gboolean seconds = g_settings_get_boolean (self->settings, "display-clock-seconds");
  gboolean use_24h = neo_clock_uses_24h ();
  g_autofree char *time = NULL, *date = NULL, *upper = NULL;
  int hour = g_date_time_get_hour (now), minute = g_date_time_get_minute (now);

  /* "Hm" / "Hms", or "hma" / "hmsa" with the AM/PM at 0.4 of the size, bold (Utils.setClockStyle) */
  if (seconds && use_24h)
    time = g_strdup_printf ("%02d:%02d:%02d", hour, minute, g_date_time_get_second (now));
  else if (seconds)
    time = g_strdup_printf ("%d:%02d:%02d <span size='40%%' weight='900'>%s</span>", hour % 12 ? hour % 12 : 12,
                            minute, g_date_time_get_second (now), hour < 12 ? "AM" : "PM");
  else
    time = clock_time_markup (hour, minute, use_24h, 0.4);
  gtk_label_set_markup (GTK_LABEL (self->time), time);

  /* the best "EEEMMMd" pattern, in capitals: "SAT, NOV 7" */
  date = g_date_time_format (now, "%a, %b %-d");
  upper = g_utf8_strup (date, -1);
  gtk_label_set_text (GTK_LABEL (self->date), upper);

  /* the next update on the next second, or minute, boundary */
  g_clear_handle_id (&self->tick, g_source_remove);
  {
    int ms = 1000 - (int) (g_date_time_get_microsecond (now) / 1000);

    if (!seconds)
      ms += (59 - g_date_time_get_second (now)) * 1000;
    self->tick = g_timeout_add (ms, on_tick, self);
  }
}

static gboolean
on_tick (gpointer data)
{
  ClockPage *self = data;

  self->tick = 0;
  show_time (self);
  return G_SOURCE_REMOVE;
}

static void
on_settings (GSettings *settings, const char *key, ClockPage *self)
{
  show_time (self);
}

static void
clock_page_map (GtkWidget *widget)
{
  ClockPage *self = CLOCK_PAGE (widget);

  GTK_WIDGET_CLASS (clock_page_parent_class)->map (widget);
  show_time (self);
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
  g_clear_pointer (&self->frame, gtk_widget_unparent);
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
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "clock-page");
}

static void
clock_page_init (ClockPage *self)
{
  GtkWidget *line;

  /* main_clock_frame: 24 dp from the sides, 14 dp under it */
  self->frame = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_start (self->frame, 24);
  gtk_widget_set_margin_end (self->frame, 24);
  gtk_widget_set_valign (self->frame, GTK_ALIGN_START);
  gtk_widget_set_parent (self->frame, GTK_WIDGET (self));

  /* the time: 24 dp paddingTop, includeFontPadding off, left-aligned for the digital style */
  self->time = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (self->time), 0);
  gtk_widget_set_margin_top (self->time, 24);
  /* GTK rounds the line up to 106 dp (90 px × 2400/2048 = 105.5); Android's is 316 px, 105.3 dp, so the date
   * would sit 2 device pixels low */
  gtk_widget_set_margin_bottom (self->time, -1);
  gtk_widget_add_css_class (self->time, "main-clock");
  gtk_box_append (GTK_BOX (self->frame), self->time);

  /* the date, and the next alarm with the alarm glyph 6 dp either side of it */
  line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_margin_bottom (line, 14);
  self->date = gtk_label_new (NULL);
  gtk_widget_add_css_class (self->date, "body");
  neo_label_set_font_padding (self->date, 14);
  self->alarm_icon = gtk_image_new_from_icon_name ("clock-alarm-small-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (self->alarm_icon), 14);
  gtk_widget_set_margin_start (self->alarm_icon, 6);
  gtk_widget_set_margin_end (self->alarm_icon, 6);
  gtk_widget_add_css_class (self->alarm_icon, "next-alarm");
  self->alarm = gtk_label_new (NULL);
  gtk_widget_add_css_class (self->alarm, "body");
  neo_label_set_font_padding (self->alarm, 14);
  gtk_box_append (GTK_BOX (line), self->date);
  gtk_box_append (GTK_BOX (line), self->alarm_icon);
  gtk_box_append (GTK_BOX (line), self->alarm);
  gtk_box_append (GTK_BOX (self->frame), line);
}

GtkWidget *
clock_page_new (GSettings *settings)
{
  ClockPage *self = g_object_new (CLOCK_TYPE_PAGE, NULL);

  self->settings = g_object_ref (settings);
  g_signal_connect (settings, "changed::display-clock-seconds", G_CALLBACK (on_settings), self);
  g_signal_connect_swapped (alarms_client_get (), "next-alarm-changed", G_CALLBACK (show_next_alarm), self);
  return GTK_WIDGET (self);
}
