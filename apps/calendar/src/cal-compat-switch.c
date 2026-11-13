/* CalCompatSwitch (cal-compat-switch.h).
 *
 * A thin track with round ends under a bigger round thumb. Off, a light grey thumb over a dark grey track; on, both
 * in the primary colour, the track at 30 %. The thumb slides in 250 ms on an accelerate-decelerate curve; the
 * colours follow its position. */
#include "cal-compat-switch.h"
#include "cal-colors.h"

#include <neo.h>

#define WIDTH 46.7
#define HEIGHT 27.0
#define TRACK_HEIGHT 14.0
#define TRACK_END 35.0
#define THUMB 20.0
#define THUMB_OFF 13.3                  /* the thumb's centre, off */
#define THUMB_ON (TRACK_END - TRACK_HEIGHT / 2)
#define TRACK_START (THUMB_OFF - TRACK_HEIGHT / 2)
#define SLIDE_MS 250
#define THUMB_OFF_COLOR (cal_palette->switch_thumb_off)
#define TRACK_OFF_COLOR (cal_palette->switch_track_off)

struct _CalCompatSwitch {
  GtkWidget parent;
  gboolean active;
  double position;                      /* 0 off … 1 on */
  NeoAnim *slide;
};

G_DEFINE_FINAL_TYPE (CalCompatSwitch, cal_compat_switch, GTK_TYPE_WIDGET)

enum { PROP_0, PROP_ACTIVE, N_PROPS };
static GParamSpec *props[N_PROPS];

static GdkRGBA
mix (guint32 a, guint32 b, double t)
{
  GdkRGBA x = cal_argb (a), y = cal_argb (b);

  return (GdkRGBA) { x.red + (y.red - x.red) * t, x.green + (y.green - x.green) * t,
                     x.blue + (y.blue - x.blue) * t, x.alpha + (y.alpha - x.alpha) * t };
}

static void
cal_compat_switch_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CalCompatSwitch *self = CAL_COMPAT_SWITCH (widget);
  double y = gtk_widget_get_height (widget) / 2, x0 = (gtk_widget_get_width (widget) - WIDTH) / 2;
  double centre = x0 + THUMB_OFF + (THUMB_ON - THUMB_OFF) * self->position;
  GskRoundedRect track, thumb;
  /* the on track is the primary colour at 30 %: the off one's grey turns into it as the thumb goes */
  GdkRGBA track_color = mix (TRACK_OFF_COLOR, CAL_PRIMARY & 0x4dffffff, self->position);
  GdkRGBA thumb_color = mix (THUMB_OFF_COLOR, CAL_PRIMARY, self->position);
  gsk_rounded_rect_init_from_rect (&track, &GRAPHENE_RECT_INIT (x0 + TRACK_START, y - TRACK_HEIGHT / 2,
                                                                TRACK_END - TRACK_START, TRACK_HEIGHT),
                                   TRACK_HEIGHT / 2);
  gtk_snapshot_push_rounded_clip (snapshot, &track);
  gtk_snapshot_append_color (snapshot, &track_color, &track.bounds);
  gtk_snapshot_pop (snapshot);
  gsk_rounded_rect_init_from_rect (&thumb, &GRAPHENE_RECT_INIT (centre - THUMB / 2, y - THUMB / 2 + 0.5, THUMB,
                                                                THUMB), THUMB / 2);
  gtk_snapshot_push_rounded_clip (snapshot, &thumb);
  gtk_snapshot_append_color (snapshot, &thumb_color, &thumb.bounds);
  gtk_snapshot_pop (snapshot);
}

static void
cal_compat_switch_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                           int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = (int) ceil (orientation == GTK_ORIENTATION_HORIZONTAL ? WIDTH : HEIGHT);
}

static void
on_slide (NeoAnim *anim, double value, gpointer data)
{
  CalCompatSwitch *self = data;

  self->position = value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* the press is the switch's, not the row's around it (which would toggle it a second time) */
static void
on_pressed (GtkGestureClick *click, int n_press, double x, double y, CalCompatSwitch *self)
{
  gtk_gesture_set_state (GTK_GESTURE (click), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void
on_released (GtkGestureClick *click, int n_press, double x, double y, CalCompatSwitch *self)
{
  cal_compat_switch_set_active (self, !self->active);
}

static void
cal_compat_switch_dispose (GObject *object)
{
  CalCompatSwitch *self = CAL_COMPAT_SWITCH (object);

  g_clear_pointer (&self->slide, neo_anim_free);
  G_OBJECT_CLASS (cal_compat_switch_parent_class)->dispose (object);
}

static void
cal_compat_switch_get_property (GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  if (id == PROP_ACTIVE)
    g_value_set_boolean (value, CAL_COMPAT_SWITCH (object)->active);
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, pspec);
}

static void
cal_compat_switch_set_property (GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
  if (id == PROP_ACTIVE)
    cal_compat_switch_set_active (CAL_COMPAT_SWITCH (object), g_value_get_boolean (value));
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, pspec);
}

static void
cal_compat_switch_class_init (CalCompatSwitchClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = cal_compat_switch_dispose;
  object_class->get_property = cal_compat_switch_get_property;
  object_class->set_property = cal_compat_switch_set_property;
  widget_class->snapshot = cal_compat_switch_snapshot;
  widget_class->measure = cal_compat_switch_measure;
  props[PROP_ACTIVE] = g_param_spec_boolean ("active", NULL, NULL, FALSE,
                                             G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (object_class, N_PROPS, props);
  gtk_widget_class_set_css_name (widget_class, "cal-compat-switch");
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_SWITCH);
}

static void
cal_compat_switch_init (CalCompatSwitch *self)
{
  GtkGesture *click = gtk_gesture_click_new ();

  self->slide = neo_anim_new (GTK_WIDGET (self), on_slide, self);
  g_signal_connect (click, "pressed", G_CALLBACK (on_pressed), self);
  g_signal_connect (click, "released", G_CALLBACK (on_released), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
  gtk_accessible_update_state (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_STATE_CHECKED, GTK_ACCESSIBLE_TRISTATE_FALSE,
                               -1);
}

GtkWidget *
cal_compat_switch_new (void)
{
  return g_object_new (CAL_TYPE_COMPAT_SWITCH, NULL);
}

gboolean
cal_compat_switch_get_active (CalCompatSwitch *self)
{
  g_return_val_if_fail (CAL_IS_COMPAT_SWITCH (self), FALSE);
  return self->active;
}

void
cal_compat_switch_set_active (CalCompatSwitch *self, gboolean active)
{
  g_return_if_fail (CAL_IS_COMPAT_SWITCH (self));
  active = !!active;
  if (self->active == active)
    return;
  self->active = active;
  /* a switch not on screen yet starts where it is; NeoAnim jumps to the end there */
  neo_anim_start (self->slide, self->position, active ? 1 : 0, SLIDE_MS, 0, SPONGY_ACCELERATE_DECELERATE);
  gtk_accessible_update_state (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_STATE_CHECKED,
                               active ? GTK_ACCESSIBLE_TRISTATE_TRUE : GTK_ACCESSIBLE_TRISTATE_FALSE, -1);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ACTIVE]);
}
