/* VolumeSlider: see volume-slider.h. Drawn with colour nodes under rounded clips. */
#include "volume-slider.h"

#include <math.h>
#include <neo.h>

#define INSET       14          /* the track's ends, in from the view's */
#define TRACK       16          /* its height; the outer ends fully round */
#define INNER       2           /* the corners beside the handle */
#define HANDLE_W    4
#define HANDLE_H    44
#define GAP         4           /* between the handle and the track, either side */
#define DOT         4           /* a stop indicator's diameter */
#define MAX_DOTS    20          /* more steps than this: no dots between the ends */

struct _VolumeSlider
{
  GtkWidget parent;
  int max, value;
  gboolean cancelled;           /* the touch went to the list */
};

G_DEFINE_FINAL_TYPE (VolumeSlider, volume_slider, GTK_TYPE_WIDGET)

enum { CHANGED, RELEASED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* where step @value sits: the first and last are half the track's height in from its ends */
static double
step_x (VolumeSlider *self, double value)
{
  double start = INSET + TRACK / 2.0, end = gtk_widget_get_width (GTK_WIDGET (self)) - INSET - TRACK / 2.0;

  return start + (self->max ? value / self->max : 0) * (end - start);
}

static void
append_rounded (GtkSnapshot *snapshot, const graphene_rect_t *rect, double left, double right, const GdkRGBA *color)
{
  GskRoundedRect shape;
  graphene_size_t l = GRAPHENE_SIZE_INIT (left, left), r = GRAPHENE_SIZE_INIT (right, right);

  if (rect->size.width <= 0)
    return;
  gsk_rounded_rect_init (&shape, rect, &l, &r, &r, &l);
  gtk_snapshot_push_rounded_clip (snapshot, &shape);
  gtk_snapshot_append_color (snapshot, color, rect);
  gtk_snapshot_pop (snapshot);
}

static void
volume_slider_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  VolumeSlider *self = CLOCK_VOLUME_SLIDER (widget);
  int width = gtk_widget_get_width (widget), height = gtk_widget_get_height (widget);
  double cy = height / 2.0, x = step_x (self, self->value), start = INSET, end = width - INSET;
  double active_end = x - HANDLE_W / 2.0 - GAP, inactive_start = x + HANDLE_W / 2.0 + GAP;
  GdkRGBA active = neo_color (NEO_PRIMARY), inactive = neo_color (NEO_SECONDARY_CONTAINER);
  GdkRGBA on_active = neo_color (NEO_ON_PRIMARY), on_inactive = neo_color (NEO_ON_SECONDARY_CONTAINER);

  append_rounded (snapshot, &GRAPHENE_RECT_INIT (start, cy - TRACK / 2.0, active_end - start, TRACK), TRACK / 2.0,
                  INNER, &active);
  append_rounded (snapshot, &GRAPHENE_RECT_INIT (inactive_start, cy - TRACK / 2.0, end - inactive_start, TRACK),
                  INNER, TRACK / 2.0, &inactive);
  /* a dot at every step but the one under the handle, in the colour that reads on its part of the track; a slider
   * of many steps (the screen saver's brightness, 0 … 100) is a continuous one, with a dot at its end only */
  for (int i = 0; i <= self->max; i++)
    {
      double dx = step_x (self, i);

      if (i == self->value || (self->max > MAX_DOTS && i < self->max))
        continue;
      append_rounded (snapshot, &GRAPHENE_RECT_INIT (dx - DOT / 2.0, cy - DOT / 2.0, DOT, DOT), DOT / 2.0, DOT / 2.0,
                      i < self->value ? &on_active : &on_inactive);
    }
  append_rounded (snapshot, &GRAPHENE_RECT_INIT (x - HANDLE_W / 2.0, cy - HANDLE_H / 2.0, HANDLE_W, HANDLE_H),
                  HANDLE_W / 2.0, HANDLE_W / 2.0, &active);
}

static void
volume_slider_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                       int *min_baseline, int *nat_baseline)
{
  /* a thumb-sized touch target tall; as wide as it is given */
  *min = orientation == GTK_ORIENTATION_VERTICAL ? 48 : 2 * INSET + TRACK;
  *nat = *min;
}

/* --- touch: the nearest step to the finger, as it moves */

static void
move_to (VolumeSlider *self, double x)
{
  double start = step_x (self, 0), end = step_x (self, self->max);
  int value = end > start ? (int) round (CLAMP ((x - start) / (end - start), 0, 1) * self->max) : 0;

  if (value == self->value)
    return;
  self->value = value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[CHANGED], 0, value);
}

/* In a scrolling list the slider waits to see which way a finger goes: sideways past the touch slop it is the slider's,
 * and a tap moves it too; a vertical move is left to the list. */
#define TOUCH_SLOP 8

static gboolean
claimed (GtkGestureDrag *drag)
{
  GtkGesture *gesture = GTK_GESTURE (drag);

  return gtk_gesture_get_sequence_state (gesture, gtk_gesture_single_get_current_sequence (GTK_GESTURE_SINGLE (drag)))
         == GTK_EVENT_SEQUENCE_CLAIMED;
}

static void
on_drag_update (GtkGestureDrag *drag, double dx, double dy, VolumeSlider *self)
{
  double x, y;

  if (!claimed (drag))
    {
      if (fabs (dx) < TOUCH_SLOP || fabs (dx) < fabs (dy))
        return;
      gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_CLAIMED);
    }
  gtk_gesture_drag_get_start_point (drag, &x, &y);
  move_to (self, x + dx);
}

/* The list took the touch (it scrolled): GTK still ends the drag, with whatever it had moved by then, which could
 * be less than the slop. That is no tap, and must neither move the slider nor play the preview. */
static void
on_cancel (GtkGesture *gesture, GdkEventSequence *sequence, VolumeSlider *self)
{
  self->cancelled = TRUE;
}

static void
on_drag_begin (GtkGestureDrag *drag, double x, double y, VolumeSlider *self)
{
  self->cancelled = FALSE;
}

static void
on_drag_end (GtkGestureDrag *drag, double dx, double dy, VolumeSlider *self)
{
  double x, y;

  if (self->cancelled || gtk_gesture_get_sequence_state (GTK_GESTURE (drag),
                                                         gtk_gesture_single_get_current_sequence (
                                                           GTK_GESTURE_SINGLE (drag)))
                         == GTK_EVENT_SEQUENCE_DENIED)
    return;
  if (!claimed (drag))
    {
      /* a tap: there */
      if (fabs (dx) >= TOUCH_SLOP || fabs (dy) >= TOUCH_SLOP)
        return;
      gtk_gesture_drag_get_start_point (drag, &x, &y);
      move_to (self, x);
    }
  /* let go: the preview */
  g_signal_emit (self, signals[RELEASED], 0);
}

static void
volume_slider_class_init (VolumeSliderClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  widget_class->snapshot = volume_slider_snapshot;
  widget_class->measure = volume_slider_measure;
  gtk_widget_class_set_css_name (widget_class, "volume-slider");
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_SLIDER);
  signals[CHANGED] = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                   G_TYPE_NONE, 1, G_TYPE_INT);
  signals[RELEASED] = g_signal_new ("released", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                    G_TYPE_NONE, 0);
}

static void
volume_slider_init (VolumeSlider *self)
{
  GtkGesture *drag = gtk_gesture_drag_new ();

  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
  g_signal_connect (drag, "cancel", G_CALLBACK (on_cancel), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_drag_end), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));
}

GtkWidget *
volume_slider_new (int max)
{
  VolumeSlider *self = g_object_new (CLOCK_TYPE_VOLUME_SLIDER, NULL);

  self->max = max;
  return GTK_WIDGET (self);
}

int
volume_slider_get_value (VolumeSlider *self)
{
  return self->value;
}

void
volume_slider_set_value (VolumeSlider *self, int value)
{
  self->value = CLAMP (value, 0, self->max);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_VALUE_MAX, (double) self->max,
                                  GTK_ACCESSIBLE_PROPERTY_VALUE_NOW, (double) self->value, -1);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}
