/* AnalogClock: see analog-clock.h. Drawn with colour nodes under rounded clips, and a border node for the ring, so
 * the renderer draws it without paths. */
#include "analog-clock.h"

#include <vibeos.h>

#define VIEWPORT 1176.0         /* the face's drawing, in units across and down */
#define CENTRE   588.0

/* the dial: a ring between radius 483.1 (588 − 104.9) and 509.1 (588 − 78.9) */
#define DIAL_OUTER 509.1
#define DIAL_WIDTH 26.0

/* a hand: a rounded bar on the vertical centre line, from y top to y bottom (the axis is at y 588), so wide */
typedef struct
{
  double top, bottom, width;
} Hand;

static const Hand hour_hand = { 376.2, 648.2, 16 };
static const Hand minute_hand = { 183.9, 647.9, 16 };
static const Hand second_hand = { 187.3, 739.3, 8 };

struct _AnalogClock
{
  GtkWidget parent;
  int size;
  GTimeZone *tz;
  gboolean seconds;
  int hour, minute, second;
  guint tick;
};

G_DEFINE_FINAL_TYPE (AnalogClock, analog_clock, GTK_TYPE_WIDGET)

static gboolean on_tick (gpointer data);

/* the time now, and the next tick: on the next second or, without seconds, on the next minute */
static void
update (AnalogClock *self)
{
  g_autoptr(GDateTime) now = self->tz ? g_date_time_new_now (self->tz) : g_date_time_new_now_local ();
  g_autofree char *label = vibeos_format_time (now);
  int ms = 1000 - g_date_time_get_microsecond (now) / 1000;

  self->hour = g_date_time_get_hour (now) % 12;
  self->minute = g_date_time_get_minute (now);
  self->second = g_date_time_get_second (now);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  gtk_widget_queue_draw (GTK_WIDGET (self));

  g_clear_handle_id (&self->tick, g_source_remove);
  if (!gtk_widget_get_mapped (GTK_WIDGET (self)))
    return;
  if (!self->seconds)
    ms += (59 - self->second) * 1000;
  self->tick = g_timeout_add (ms, on_tick, self);
}

static gboolean
on_tick (gpointer data)
{
  AnalogClock *self = data;

  self->tick = 0;
  update (self);
  return G_SOURCE_REMOVE;
}

void
analog_clock_set_time_zone (AnalogClock *self, GTimeZone *tz)
{
  g_clear_pointer (&self->tz, g_time_zone_unref);
  self->tz = tz ? g_time_zone_ref (tz) : NULL;
  update (self);
}

void
analog_clock_set_seconds (AnalogClock *self, gboolean seconds)
{
  self->seconds = seconds;
  update (self);
}

/* --- drawing */

/* one hand turned @degrees clockwise about the centre, in face units (the snapshot is already scaled) */
static void
draw_hand (GtkSnapshot *snapshot, const Hand *hand, double degrees, const GdkRGBA *color)
{
  graphene_rect_t bar = GRAPHENE_RECT_INIT (CENTRE - hand->width / 2, hand->top, hand->width,
                                            hand->bottom - hand->top);
  GskRoundedRect clip;

  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (CENTRE, CENTRE));
  gtk_snapshot_rotate (snapshot, degrees);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (-CENTRE, -CENTRE));
  gsk_rounded_rect_init_from_rect (&clip, &bar, hand->width / 2);
  gtk_snapshot_push_rounded_clip (snapshot, &clip);
  gtk_snapshot_append_color (snapshot, color, &bar);
  gtk_snapshot_pop (snapshot);
  gtk_snapshot_restore (snapshot);
}

static void
analog_clock_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  AnalogClock *self = CLOCK_ANALOG_CLOCK (widget);
  int width = gtk_widget_get_width (widget), height = gtk_widget_get_height (widget);
  double side = MIN (width, height), scale = side / VIEWPORT;
  GdkRGBA white = vibeos_color (VIBEOS_WHITE), accent = vibeos_color (VIBEOS_SECONDARY);
  GskRoundedRect ring;
  const float widths[4] = { DIAL_WIDTH, DIAL_WIDTH, DIAL_WIDTH, DIAL_WIDTH };
  const GdkRGBA colors[4] = { white, white, white, white };

  if (side <= 0)
    return;
  gtk_snapshot_save (snapshot);
  /* the largest square, in the middle */
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT ((width - side) / 2, (height - side) / 2));
  gtk_snapshot_scale (snapshot, scale, scale);

  gsk_rounded_rect_init_from_rect (&ring, &GRAPHENE_RECT_INIT (CENTRE - DIAL_OUTER, CENTRE - DIAL_OUTER,
                                                               2 * DIAL_OUTER, 2 * DIAL_OUTER), DIAL_OUTER);
  gtk_snapshot_append_border (snapshot, &ring, widths, colors);

  /* the hands from the bottom up: hour, minute, then second on top */
  draw_hand (snapshot, &hour_hand, self->hour * 30.0, &white);
  draw_hand (snapshot, &minute_hand, self->minute * 6.0, &white);
  if (self->seconds)
    draw_hand (snapshot, &second_hand, self->second * 6.0, &accent);
  gtk_snapshot_restore (snapshot);
}

/* --- size: the face's size, or less to fit the width, and as tall as it is wide */

static GtkSizeRequestMode
analog_clock_get_request_mode (GtkWidget *widget)
{
  return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static void
analog_clock_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                      int *min_baseline, int *nat_baseline)
{
  AnalogClock *self = CLOCK_ANALOG_CLOCK (widget);

  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    {
      *min = 0;
      *nat = self->size;
    }
  else
    *min = *nat = for_size < 0 ? self->size : MIN (for_size, self->size);
}

static void
analog_clock_map (GtkWidget *widget)
{
  GTK_WIDGET_CLASS (analog_clock_parent_class)->map (widget);
  /* shown: the time now, and ticking */
  update (CLOCK_ANALOG_CLOCK (widget));
}

static void
analog_clock_unmap (GtkWidget *widget)
{
  AnalogClock *self = CLOCK_ANALOG_CLOCK (widget);

  g_clear_handle_id (&self->tick, g_source_remove);
  GTK_WIDGET_CLASS (analog_clock_parent_class)->unmap (widget);
}

static void
analog_clock_finalize (GObject *object)
{
  AnalogClock *self = CLOCK_ANALOG_CLOCK (object);

  g_clear_handle_id (&self->tick, g_source_remove);
  g_clear_pointer (&self->tz, g_time_zone_unref);
  G_OBJECT_CLASS (analog_clock_parent_class)->finalize (object);
}

static void
analog_clock_class_init (AnalogClockClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->finalize = analog_clock_finalize;
  widget_class->snapshot = analog_clock_snapshot;
  widget_class->get_request_mode = analog_clock_get_request_mode;
  widget_class->measure = analog_clock_measure;
  widget_class->map = analog_clock_map;
  widget_class->unmap = analog_clock_unmap;
  gtk_widget_class_set_css_name (widget_class, "analog-clock");
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_IMG);
}

static void
analog_clock_init (AnalogClock *self)
{
}

GtkWidget *
analog_clock_new (int size)
{
  AnalogClock *self = g_object_new (CLOCK_TYPE_ANALOG_CLOCK, NULL);

  self->size = size;
  update (self);
  return GTK_WIDGET (self);
}
