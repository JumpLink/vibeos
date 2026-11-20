/* The ringing alarm: see ringing-window.h.
 *
 * The layout is a 3 × 3 grid: the clock and the label each span a row; the third row takes the rest of the height and
 * holds, in three equal columns, Snooze, the handle and Dismiss, with the pulse centred on the handle and the hint at
 * its foot. Snooze is always on the left (the layout is forced left-to-right). The view draws all of it itself, as one
 * surface, because the animations below run across all of its parts.
 *
 * The three button animations are driven by hand: each has a fraction 0…1 that the pointer sets, and the value it
 * shows is that fraction through an accelerate-decelerate curve. */
#include "ringing-window.h"
#include "alarms-client.h"
#include "clock-draw.h"
#include "clock-format.h"

#include <glib/gi18n.h>
#include <math.h>
#include <vibeos.h>

#define CLOCK_TOP       64      /* over the clock: an action bar's height */
#define CLOCK_SIZE      70
#define TITLE_SIZE      20
#define HINT_SIZE       16      /* "Swipe right to dismiss" */
#define HINT_BOTTOM     40      /* the hint, up from the bottom */
#define ALERT_SIZE      36      /* "Snoozed" or "Alarm off", once the alarm is handled */
#define BUTTON          80      /* Snooze and Dismiss, on white discs */
#define HANDLE          56      /* the alarm handle */
#define HANDLE_PAD_X    16      /* the handle's touch area, past it at the sides */
#define HANDLE_PAD_Y    48      /* ... and above and below */
#define PULSE_RADIUS    128     /* how far the pulse round the handle grows */
#define BUTTON_SCALE    0.7     /* Snooze and Dismiss's scale at rest */
#define ICON_ALPHA      165     /* their icons' opacity at rest, of 255 */
#define TOUCH_SLOP      8       /* how far a tap may stray off a button and still count */

#define PULSE_MS        1000
#define BOUNCE_MS       500
#define HINT_FADE_MS    300     /* the hint fading in or out */
#define REVERSE_MS      300     /* the same, for the button animators run back */
#define REVEAL_MS       500
#define ALERT_FADE_MS   500
#define CLOSE_DELAY_MS  2000

/* font padding: the line box adds Roboto's head yMax/yMin beyond hhea (vibeos_label_set_font_padding) */
#define PAD_TOP(size)    ((size) * 263 / 2048.0)
#define PAD_BOTTOM(size) ((size) * 55 / 2048.0)

#define RINGING_TYPE_VIEW (ringing_view_get_type ())
G_DECLARE_FINAL_TYPE (RingingView, ringing_view, RINGING, VIEW, GtkWidget)

typedef enum { TARGET_NONE, TARGET_HANDLE, TARGET_SNOOZE, TARGET_DISMISS } Target;

struct _RingingView
{
  GtkWidget parent;
  GSettings *settings;
  guint instance;               /* the ringing alarm instance */
  PangoLayout *clock, *title, *hint, *alert_title, *alert_info;
  guint minute;                 /* the clock's update at the next minute */

  /* the swipe: how far the pointer is toward Snooze and toward Dismiss, 0…1, and what the touch began on */
  double snooze, dismiss;
  Target target;
  VibeosAnim *reverse;             /* both fractions back to 0 after a swipe let go short of a button */
  double reverse_from;

  /* the pulse: a white disc growing from the handle and fading, once a second, while nothing touches it */
  VibeosAnim *pulse;
  double pulse_value;
  gboolean pulse_repeat;
  guint pulses;

  /* the hint: the handle nudged toward a button and back, and the line saying which way to swipe */
  VibeosAnim *bounce, *hint_fade;
  double bounce_from, bounce_to, shift, hint_alpha;
  gboolean hint_shown;

  /* the end: a grey circle out of the chosen button, then the alert text, then the window closes */
  gboolean handled, content_gone, alert_info_shown;
  VibeosAnim *reveal, *fade;
  double reveal_x, reveal_y, reveal_from, reveal_to, reveal_radius, reveal_alpha;
  guint close;
};

G_DEFINE_FINAL_TYPE (RingingView, ringing_view, GTK_TYPE_WIDGET)

/* where the parts are, for a view of this size */
typedef struct
{
  double clock_y, title_y, hint_y;
  double cy;                    /* the middle of the third row: the pulse, the buttons and the handle */
  double snooze_x, handle_x, dismiss_x;
} Geometry;

static double
layout_height (PangoLayout *layout)
{
  int width, height;

  pango_layout_get_size (layout, &width, &height);
  return height / (double) PANGO_SCALE;
}

static double
layout_width (PangoLayout *layout)
{
  int width, height;

  pango_layout_get_size (layout, &width, &height);
  return width / (double) PANGO_SCALE;
}

static void
measure_geometry (RingingView *self, Geometry *g)
{
  double width = gtk_widget_get_width (GTK_WIDGET (self)), height = gtk_widget_get_height (GTK_WIDGET (self));
  double row;

  /* row 0: the clock, without font padding; row 1: the label, with it */
  g->clock_y = CLOCK_TOP;
  g->title_y = g->clock_y + layout_height (self->clock) + PAD_TOP (TITLE_SIZE);
  row = g->title_y + layout_height (self->title) + PAD_BOTTOM (TITLE_SIZE);
  /* row 2: the rest; three columns of equal weight */
  g->cy = (row + height) / 2;
  g->snooze_x = width / 6;
  g->handle_x = width / 2;
  g->dismiss_x = width * 5 / 6;
  g->hint_y = height - HINT_BOTTOM - PAD_BOTTOM (HINT_SIZE) - layout_height (self->hint);
}

/* accelerate-decelerate: the animations' default curve */
static double
accel_decel (double t)
{
  return spongy_curve (SPONGY_ACCELERATE_DECELERATE, t);
}

/* decelerate-accelerate: 0.5 + 4 (t − 0.5)³ */
static double
decel_accel (double t)
{
  return 0.5 + 4 * pow (t - 0.5, 3);
}

/* colours: the channels are mixed in linear light (gamma 2.2), alpha as it is */
static GdkRGBA
argb_evaluate (const GdkRGBA *a, const GdkRGBA *b, double t)
{
#define MIX(c) pow (pow (a->c, 2.2) + (pow (b->c, 2.2) - pow (a->c, 2.2)) * t, 1 / 2.2)
  return (GdkRGBA) { MIX (red), MIX (green), MIX (blue), a->alpha + (b->alpha - a->alpha) * t };
#undef MIX
}

static double
fraction (double x0, double x1, double x)
{
  return CLAMP ((x - x0) / (x1 - x0), 0, 1);
}

/* the handle shrinks by whichever button is nearer to being reached */
static void
set_fractions (RingingView *self, double snooze, double dismiss)
{
  self->snooze = snooze;
  self->dismiss = dismiss;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* ---- text ----------------------------------------------------------------------------------------------------- */

static PangoLayout *
new_layout (RingingView *self, double size, int weight)
{
  PangoLayout *layout = gtk_widget_create_pango_layout (GTK_WIDGET (self), NULL);
  g_autoptr(PangoFontDescription) font = pango_font_description_from_string ("VibeOS Roboto");

  pango_font_description_set_absolute_size (font, size * PANGO_SCALE);
  /* a weight from 700 up is Bold text's, which draws the regular ones 300 heavier */
  pango_font_description_set_weight (font, weight >= 700 ? vibeos_font_weight (weight - 300) : weight);
  pango_layout_set_font_description (layout, font);
  return layout;
}

static gboolean on_minute (gpointer data);

/* the clock: "h:mm" with a small AM/PM, or "HH:mm", each minute */
static void
show_time (RingingView *self)
{
  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *markup = clock_time_markup (g_date_time_get_hour (now), g_date_time_get_minute (now),
                                               vibeos_clock_uses_24h (), 0.4);

  pango_layout_set_markup (self->clock, markup, -1);
  g_clear_handle_id (&self->minute, g_source_remove);
  self->minute = g_timeout_add ((60 - g_date_time_get_second (now)) * 1000
                                - g_date_time_get_microsecond (now) / 1000, on_minute, self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static gboolean
on_minute (gpointer data)
{
  RingingView *self = data;

  self->minute = 0;
  show_time (self);
  return G_SOURCE_REMOVE;
}

/* ---- the pulse ------------------------------------------------------------------------------------------------ */

static void
on_pulse (VibeosAnim *anim, double value, gpointer data)
{
  RingingView *self = data;

  self->pulse_value = value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  /* the pulse repeats */
  if (!vibeos_anim_is_running (anim) && self->pulse_repeat && gtk_widget_get_mapped (GTK_WIDGET (self)))
    {
      self->pulses++;
      vibeos_anim_start (anim, 0, 1, PULSE_MS, 0, SPONGY_FAST_OUT_SLOW_IN);
    }
}

static void
start_pulse (RingingView *self)
{
  self->pulse_repeat = TRUE;
  if (!vibeos_anim_is_running (self->pulse))
    {
      self->pulses = 0;
      vibeos_anim_start (self->pulse, 0, 1, PULSE_MS, 0, SPONGY_FAST_OUT_SLOW_IN);
    }
}

/* on a touch the pulse stops repeating: the first pulse runs to its end, but a later one is past its last iteration
 * already and ends at once, where it is invisible */
static void
stop_pulse (RingingView *self)
{
  self->pulse_repeat = FALSE;
  if (self->pulses > 0)
    vibeos_anim_finish (self->pulse);
}

/* ---- the hint ------------------------------------------------------------------------------------------------- */

static void
on_bounce (VibeosAnim *anim, double value, gpointer data)
{
  RingingView *self = data;
  double t = decel_accel (value);

  /* three keyframes, current → toward the button → home, the curve over all of them */
  self->shift = t < 0.5 ? self->bounce_from + (self->bounce_to - self->bounce_from) * t * 2
                        : self->bounce_to * (1 - (t - 0.5) * 2);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_hint_fade (VibeosAnim *anim, double value, gpointer data)
{
  RingingView *self = data;

  self->hint_alpha = value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* the hint for Snooze or Dismiss: the handle travels the gap between its glyph and the button, and back */
static void
hint (RingingView *self, Target button)
{
  Geometry g;
  double left, right, x, to;

  measure_geometry (self, &g);
  left = g.handle_x - HANDLE / 2.0;
  right = g.handle_x + HANDLE / 2.0;
  x = button == TARGET_SNOOZE ? g.snooze_x : g.dismiss_x;
  to = MAX (x - BUTTON / 2.0 - right, 0) + MIN (x + BUTTON / 2.0 - left, 0);

  self->bounce_from = self->shift;
  self->bounce_to = to;
  vibeos_anim_start (self->bounce, 0, 1, BOUNCE_MS, 0, SPONGY_LINEAR);
  pango_layout_set_text (self->hint, to < 0 ? _("Swipe left to snooze") : _("Swipe right to dismiss"), -1);
  if (!self->hint_shown)
    {
      self->hint_shown = TRUE;
      vibeos_anim_start (self->hint_fade, 0, 1, HINT_FADE_MS, 0, SPONGY_ACCELERATE_DECELERATE);
    }
}

/* ---- snooze, dismiss ------------------------------------------------------------------------------------------ */

static gboolean
on_close (gpointer data)
{
  RingingView *self = data;
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));

  self->close = 0;
  if (root)
    gtk_window_destroy (GTK_WINDOW (root));
  return G_SOURCE_REMOVE;
}

static void
on_fade (VibeosAnim *anim, double value, gpointer data)
{
  RingingView *self = data;

  self->reveal_alpha = value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  if (!vibeos_anim_is_running (anim) && !self->close)
    self->close = g_timeout_add (CLOSE_DELAY_MS, on_close, self);
}

static void
on_reveal (VibeosAnim *anim, double value, gpointer data)
{
  RingingView *self = data;

  self->reveal_radius = self->reveal_from + (self->reveal_to - self->reveal_from) * value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  /* covered: the alert text comes up behind the circle, the rest goes, and the circle fades off it */
  if (!vibeos_anim_is_running (anim))
    {
      self->content_gone = TRUE;
      vibeos_anim_start (self->fade, 1, 0, ALERT_FADE_MS, 0, SPONGY_ACCELERATE_DECELERATE);
    }
}

/* the alert: a #444444 circle centred on the button grows from its half-size to the farthest corner */
static void
reveal (RingingView *self, Target button, const char *title, const char *info)
{
  Geometry g;
  double width = gtk_widget_get_width (GTK_WIDGET (self)), height = gtk_widget_get_height (GTK_WIDGET (self));

  measure_geometry (self, &g);
  self->reveal_x = button == TARGET_SNOOZE ? g.snooze_x : g.dismiss_x;
  self->reveal_y = g.cy;
  self->reveal_from = BUTTON / 2.0;
  self->reveal_to = hypot (MAX (self->reveal_x, width - self->reveal_x), MAX (self->reveal_y, height - self->reveal_y));
  self->reveal_alpha = 1;
  pango_layout_set_text (self->alert_title, title, -1);
  self->alert_info_shown = info != NULL;
  pango_layout_set_text (self->alert_info, info ? info : "", -1);
  vibeos_anim_start (self->reveal, 0, 1, REVEAL_MS, 0, SPONGY_LINEAR_OUT_SLOW_IN);
}

static void
snooze (RingingView *self)
{
  int minutes = g_settings_get_int (self->settings, "snooze-duration");
  g_autofree char *info = g_strdup_printf (ngettext ("%d min", "%d min", minutes), minutes);

  self->handled = TRUE;
  vibeos_anim_stop (self->reverse);
  set_fractions (self, 1, 0);
  reveal (self, TARGET_SNOOZE, _("Snoozed"), info);
  alarms_client_snooze (alarms_client_get (), self->instance);
}

static void
dismiss (RingingView *self)
{
  self->handled = TRUE;
  vibeos_anim_stop (self->reverse);
  set_fractions (self, 0, 1);
  reveal (self, TARGET_DISMISS, _("Alarm off"), NULL);
  alarms_client_dismiss (alarms_client_get (), self->instance);
}

/* ---- touch ---------------------------------------------------------------------------------------------------- */

static void
on_reverse (VibeosAnim *anim, double value, gpointer data)
{
  RingingView *self = data;

  /* reversed: each animation plays back from where it is, at its own speed */
  set_fractions (self, self->snooze > 0 ? value : 0, self->dismiss > 0 ? value : 0);
}

static Target
target_at (RingingView *self, double x, double y)
{
  Geometry g;
  double button = BUTTON * BUTTON_SCALE / 2;

  measure_geometry (self, &g);
  /* the handle's glyph and its padding (88 × 152), where a hint has pushed it */
  if (fabs (x - g.handle_x - self->shift) <= HANDLE / 2.0 + HANDLE_PAD_X && fabs (y - g.cy) <= HANDLE / 2.0 + HANDLE_PAD_Y)
    return TARGET_HANDLE;
  /* the buttons at their resting size */
  if (fabs (y - g.cy) <= button + TOUCH_SLOP)
    {
      if (fabs (x - g.snooze_x) <= button + TOUCH_SLOP)
        return TARGET_SNOOZE;
      if (fabs (x - g.dismiss_x) <= button + TOUCH_SLOP)
        return TARGET_DISMISS;
    }
  return TARGET_NONE;
}

/* the fractions are where the pointer is between the handle glyph's edge and the far edge of each button */
static void
track (RingingView *self, double x)
{
  Geometry g;

  measure_geometry (self, &g);
  set_fractions (self, fraction (g.handle_x - HANDLE / 2.0, g.snooze_x + BUTTON / 2.0, x),
                 fraction (g.handle_x + HANDLE / 2.0, g.dismiss_x - BUTTON / 2.0, x));
}

static void
on_drag_begin (GtkGestureDrag *gesture, double x, double y, RingingView *self)
{
  self->target = self->handled ? TARGET_NONE : target_at (self, x, y);
  if (self->target == TARGET_NONE)
    {
      gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_DENIED);
      return;
    }
  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  if (self->target == TARGET_HANDLE)
    {
      vibeos_anim_stop (self->reverse);
      stop_pulse (self);
      track (self, x);
    }
}

static void
on_drag_update (GtkGestureDrag *gesture, double dx, double dy, RingingView *self)
{
  double x, y;

  if (self->target != TARGET_HANDLE || self->handled)
    return;
  gtk_gesture_drag_get_start_point (gesture, &x, &y);
  track (self, x + dx);
}

static void
on_drag_end (GtkGestureDrag *gesture, double dx, double dy, RingingView *self)
{
  Target target = self->target;
  double x, y;

  self->target = TARGET_NONE;
  if (self->handled)
    return;
  gtk_gesture_drag_get_start_point (gesture, &x, &y);
  x += dx;
  y += dy;

  /* a tap on a button only hints: they act on a tap only with accessibility on */
  if (target == TARGET_SNOOZE || target == TARGET_DISMISS)
    {
      if (target_at (self, x, y) == target)
        hint (self, target);
      return;
    }
  if (target != TARGET_HANDLE)
    return;

  track (self, x);
  if (self->snooze == 1)
    snooze (self);
  else if (self->dismiss == 1)
    dismiss (self);
  else
    {
      Geometry g;

      measure_geometry (self, &g);
      if (self->snooze > 0 || self->dismiss > 0)
        {
          double from = MAX (self->snooze, self->dismiss);

          vibeos_anim_start (self->reverse, from, 0, (guint) (from * REVERSE_MS), 0, SPONGY_LINEAR);
        }
      else if (fabs (y - g.cy) <= HANDLE / 2.0 + HANDLE_PAD_Y)
        hint (self, TARGET_DISMISS);
      start_pulse (self);
    }
}

static void
on_drag_cancel (GtkGesture *gesture, GdkEventSequence *sequence, RingingView *self)
{
  /* a cancelled touch: the animations reset */
  if (self->target == TARGET_HANDLE && !self->handled)
    {
      set_fractions (self, 0, 0);
      start_pulse (self);
    }
  self->target = TARGET_NONE;
}

/* ---- drawing -------------------------------------------------------------------------------------------------- */

static void
draw_text (GtkSnapshot *snapshot, PangoLayout *layout, double width, double y, double alpha)
{
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (round ((width - layout_width (layout)) / 2), y));
  gtk_snapshot_append_layout (snapshot, layout, &(GdkRGBA) { 1, 1, 1, alpha });
  gtk_snapshot_restore (snapshot);
}

/* a button at its fraction: it grows from 0.7 to full size, its disc fades in, its glyph from 165 to full alpha,
 * and Dismiss's glyph turns from white to the background's colour */
static void
draw_button (RingingView *self, GtkSnapshot *snapshot, double x, double y, double fraction, Target button)
{
  double v = accel_decel (fraction), scale = BUTTON_SCALE + (1 - BUTTON_SCALE) * v;
  GdkRGBA white = vibeos_color (VIBEOS_WHITE), disc, glyph;

  if (button == TARGET_SNOOZE)
    {
      disc = vibeos_color_alpha (VIBEOS_SECONDARY, v);
      glyph = white;
    }
  else
    {
      GdkRGBA background = vibeos_color (VIBEOS_SURFACE);

      disc = vibeos_color_alpha (VIBEOS_WHITE, v);
      glyph = argb_evaluate (&white, &background, v);
    }
  glyph.alpha = (ICON_ALPHA + (255 - ICON_ALPHA) * v) / 255;

  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (x, y));
  gtk_snapshot_scale (snapshot, scale, scale);
  if (v > 0)
    clock_snapshot_dot (snapshot, 0, 0, BUTTON / 2.0, &disc);
  vibeos_snapshot_icon (snapshot, GTK_WIDGET (self), button == TARGET_SNOOZE ? "clock-snooze-symbolic"
                                                                          : "clock-dismiss-symbolic",
                     -BUTTON / 2.0, -BUTTON / 2.0, BUTTON, &glyph);
  gtk_snapshot_restore (snapshot);
}

static void
draw_content (RingingView *self, GtkSnapshot *snapshot, const Geometry *g, double width)
{
  double handle_scale = 1 - accel_decel (MAX (self->snooze, self->dismiss));

  draw_text (snapshot, self->clock, width, g->clock_y, 1);
  draw_text (snapshot, self->title, width, g->title_y, 1);

  /* the pulse: a white circle growing from nothing to a radius of 128 px, fading as it grows */
  if (self->pulse_value > 0 && self->pulse_value < 1)
    clock_snapshot_dot (snapshot, g->handle_x, g->cy, PULSE_RADIUS * self->pulse_value,
                        &(GdkRGBA) { 1, 1, 1, 1 - self->pulse_value });

  draw_button (self, snapshot, g->snooze_x, g->cy, self->snooze, TARGET_SNOOZE);
  draw_button (self, snapshot, g->dismiss_x, g->cy, self->dismiss, TARGET_DISMISS);

  /* the handle shrinks away as either button is reached */
  if (handle_scale > 0)
    {
      gtk_snapshot_save (snapshot);
      gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (g->handle_x + self->shift, g->cy));
      gtk_snapshot_scale (snapshot, handle_scale, handle_scale);
      vibeos_snapshot_icon (snapshot, GTK_WIDGET (self), "clock-fab-alarm-symbolic", -HANDLE / 2.0, -HANDLE / 2.0, HANDLE,
                         &(GdkRGBA) { 1, 1, 1, 1 });
      gtk_snapshot_restore (snapshot);
    }

  if (self->hint_shown)
    draw_text (snapshot, self->hint, width, g->hint_y, self->hint_alpha);
}

static void
ringing_view_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  RingingView *self = RINGING_VIEW (widget);
  double width = gtk_widget_get_width (widget), height = gtk_widget_get_height (widget);
  GdkRGBA background = vibeos_color (VIBEOS_SURFACE);       /* the window's background, #0A0F10 */
  Geometry g;

  measure_geometry (self, &g);
  gtk_snapshot_append_color (snapshot, &background, &GRAPHENE_RECT_INIT (0, 0, width, height));
  if (!self->content_gone)
    draw_content (self, snapshot, &g, width);
  else
    {
      /* "Snoozed" over "10 min", or "Alarm off", centred, each line with its font padding */
      double line = PAD_TOP (ALERT_SIZE) + layout_height (self->alert_title) + PAD_BOTTOM (ALERT_SIZE);
      double y = (height - line * (self->alert_info_shown ? 2 : 1)) / 2;

      draw_text (snapshot, self->alert_title, width, y + PAD_TOP (ALERT_SIZE), 1);
      if (self->alert_info_shown)
        draw_text (snapshot, self->alert_info, width, y + line + PAD_TOP (ALERT_SIZE), 1);
    }
  if (self->handled && self->reveal_alpha > 0)
    clock_snapshot_dot (snapshot, self->reveal_x, self->reveal_y, self->reveal_radius,
                        &(GdkRGBA) { 0x44 / 255.0, 0x44 / 255.0, 0x44 / 255.0, self->reveal_alpha });
}

/* ---- the alarm ------------------------------------------------------------------------------------------------ */

/* the ringing instance, from the service; FALSE when none rings */
static gboolean
load (RingingView *self)
{
  g_autofree char *label = NULL;
  guint instance, alarm;
  gint64 time;

  if (!alarms_client_get_ringing (alarms_client_get (), &instance, &alarm, &label, &time))
    return FALSE;
  self->instance = instance;
  /* the label, or "Alarm" */
  pango_layout_set_text (self->title, label && *label ? label : _("Alarm"), -1);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  return TRUE;
}

/* the receiver for ALARM_DONE: the alarm stopped some other way, or another took its place */
static void
on_ringing_changed (AlarmsClient *client, RingingView *self)
{
  if (self->handled)
    return;
  if (!load (self))
    on_close (self);
}

static void
ringing_view_map (GtkWidget *widget)
{
  RingingView *self = RINGING_VIEW (widget);

  GTK_WIDGET_CLASS (ringing_view_parent_class)->map (widget);
  show_time (self);
  if (!self->handled)
    start_pulse (self);
}

static void
ringing_view_unmap (GtkWidget *widget)
{
  RingingView *self = RINGING_VIEW (widget);

  g_clear_handle_id (&self->minute, g_source_remove);
  self->pulse_repeat = FALSE;
  vibeos_anim_stop (self->pulse);
  GTK_WIDGET_CLASS (ringing_view_parent_class)->unmap (widget);
}

static void
ringing_view_dispose (GObject *object)
{
  RingingView *self = RINGING_VIEW (object);

  g_clear_handle_id (&self->minute, g_source_remove);
  g_clear_handle_id (&self->close, g_source_remove);
  g_signal_handlers_disconnect_by_data (alarms_client_get (), self);
  g_clear_pointer (&self->pulse, vibeos_anim_free);
  g_clear_pointer (&self->reverse, vibeos_anim_free);
  g_clear_pointer (&self->bounce, vibeos_anim_free);
  g_clear_pointer (&self->hint_fade, vibeos_anim_free);
  g_clear_pointer (&self->reveal, vibeos_anim_free);
  g_clear_pointer (&self->fade, vibeos_anim_free);
  g_clear_object (&self->clock);
  g_clear_object (&self->title);
  g_clear_object (&self->hint);
  g_clear_object (&self->alert_title);
  g_clear_object (&self->alert_info);
  g_clear_object (&self->settings);
  G_OBJECT_CLASS (ringing_view_parent_class)->dispose (object);
}

static void
ringing_view_class_init (RingingViewClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = ringing_view_dispose;
  widget_class->snapshot = ringing_view_snapshot;
  widget_class->map = ringing_view_map;
  widget_class->unmap = ringing_view_unmap;
  gtk_widget_class_set_css_name (widget_class, "ringing-view");
}

static void
ringing_view_init (RingingView *self)
{
  GtkGesture *drag = gtk_gesture_drag_new ();

  /* each 300 over the design's weight, as the phone's Bold text setting is on: the alert's thin first line comes
   * out 400 */
  self->clock = new_layout (self, CLOCK_SIZE, 700);
  self->title = new_layout (self, TITLE_SIZE, 800);
  self->hint = new_layout (self, HINT_SIZE, 700);
  self->alert_title = new_layout (self, ALERT_SIZE, 400);
  self->alert_info = new_layout (self, ALERT_SIZE, 700);
  pango_layout_set_text (self->hint, _("Swipe right to dismiss"), -1);

  self->pulse = vibeos_anim_new (GTK_WIDGET (self), on_pulse, self);
  self->reverse = vibeos_anim_new (GTK_WIDGET (self), on_reverse, self);
  self->bounce = vibeos_anim_new (GTK_WIDGET (self), on_bounce, self);
  self->hint_fade = vibeos_anim_new (GTK_WIDGET (self), on_hint_fade, self);
  self->reveal = vibeos_anim_new (GTK_WIDGET (self), on_reveal, self);
  self->fade = vibeos_anim_new (GTK_WIDGET (self), on_fade, self);

  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (drag), 0);
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_drag_end), self);
  g_signal_connect (drag, "cancel", G_CALLBACK (on_drag_cancel), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);
}

/* ---- the volume keys (the "Volume buttons" setting) ----------------------------------------------------------- */

/* "Snooze" or "Dismiss" makes the volume keys do that while the alarm is on screen; "Control volume" leaves them
 * alone. The keys are the shell's: it passes them on as the app's "volume-key" action to the app in front that asked
 * for them (TakeVolumeKeys), so the window asks while it has the focus and the setting wants them, and gives them
 * back otherwise. */
static void
set_volume_keys (GtkWidget *window, gboolean take)
{
  GApplication *app = G_APPLICATION (gtk_window_get_application (GTK_WINDOW (window)));
  GDBusConnection *bus = app ? g_application_get_dbus_connection (app) : NULL;

  if (!bus || take == GPOINTER_TO_INT (g_object_get_data (G_OBJECT (window), "volume-keys")))
    return;
  g_object_set_data (G_OBJECT (window), "volume-keys", GINT_TO_POINTER (take));
  g_dbus_connection_call (bus, "vibeos.Shell", "/vibeos/Shell", "vibeos.Shell",
                          "TakeVolumeKeys", g_variant_new ("(sb)", g_application_get_application_id (app), take),
                          NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void
update_volume_keys (GtkWidget *window)
{
  RingingView *view = g_object_get_data (G_OBJECT (window), "ringing-view");

  set_volume_keys (window, gtk_window_is_active (GTK_WINDOW (window))
                           && g_settings_get_enum (view->settings, "volume-buttons") != 0);
}

static void
on_active (GtkWidget *window, GParamSpec *pspec, gpointer data)
{
  update_volume_keys (window);
}

static void
on_volume_buttons (GSettings *settings, const char *key, GtkWidget *window)
{
  update_volume_keys (window);
}

gboolean
ringing_window_volume_key (GtkWidget *window)
{
  RingingView *view = g_object_get_data (G_OBJECT (window), "ringing-view");
  int action = g_settings_get_enum (view->settings, "volume-buttons");

  if (!gtk_window_is_active (GTK_WINDOW (window)) || action == 0)
    return FALSE;
  /* once the alarm is handled the keys do nothing more, but are still taken */
  if (!view->handled)
    {
      if (action == 1)
        snooze (view);
      else
        dismiss (view);
    }
  return TRUE;
}

/* ---- the window ----------------------------------------------------------------------------------------------- */

/* Back does not dismiss; the window closes only by itself */
static gboolean
on_close_request (GtkWindow *window, gpointer data)
{
  return TRUE;
}

static void
on_destroy (GtkWidget *window, gpointer data)
{
  GtkApplication *app = gtk_window_get_application (GTK_WINDOW (window));
  guint cookie = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (window), "inhibit"));
  RingingView *view = g_object_get_data (G_OBJECT (window), "ringing-view");

  /* the keys go back to the shell with the window */
  if (view)
    g_signal_handlers_disconnect_by_data (view->settings, window);
  set_volume_keys (window, FALSE);

  if (app && cookie)
    gtk_application_uninhibit (app, cookie);
}

GtkWidget *
ringing_window_new (GtkApplication *app, GSettings *settings)
{
  RingingView *view = g_object_new (RINGING_TYPE_VIEW, NULL);
  GtkWidget *window;
  guint cookie;

  g_object_ref_sink (view);
  view->settings = g_object_ref (settings);
  if (!load (view))
    {
      g_object_unref (view);
      return NULL;
    }
  g_signal_connect (alarms_client_get (), "ringing-changed", G_CALLBACK (on_ringing_changed), view);

  window = vibeos_window_new (app);
  gtk_window_set_title (GTK_WINDOW (window), _("Alarm"));
  vibeos_window_set_content (GTK_WINDOW (window), GTK_WIDGET (view));
  g_object_unref (view);
  g_signal_connect (window, "close-request", G_CALLBACK (on_close_request), NULL);
  /* KEEP_SCREEN_ON */
  cookie = gtk_application_inhibit (app, GTK_WINDOW (window), GTK_APPLICATION_INHIBIT_IDLE, _("An alarm is ringing"));
  g_object_set_data (G_OBJECT (window), "inhibit", GUINT_TO_POINTER (cookie));
  g_signal_connect (window, "destroy", G_CALLBACK (on_destroy), NULL);
  g_object_set_data (G_OBJECT (window), "ringing-view", view);
  g_signal_connect (window, "notify::is-active", G_CALLBACK (on_active), NULL);
  g_signal_connect (settings, "changed::volume-buttons", G_CALLBACK (on_volume_buttons), window);
  return window;
}
