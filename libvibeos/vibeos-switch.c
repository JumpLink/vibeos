/* VibeosSwitch: see vibeos-switch.h. */
#include "vibeos-switch.h"
#include "vibeos-anim.h"
#include "vibeos-style.h"

#define TRACK_W        52
#define TRACK_H        32
#define TRACK_OUTLINE  2
#define THUMB          24
#define THUMB_INSET    4       /* from the track's edge to the thumb's */
#define ICON           16
#define THUMB_PLAIN    16      /* without an icon, off: a smaller thumb */
#define TARGET_H       48      /* the touch target, tall enough for a thumb */
#define TOGGLE_MS      250

struct _VibeosSwitch {
  GtkWidget  parent;
  gboolean   active;
  double     position;           /* the thumb: 0 at the start (off) .. 1 at the end (on) */
  gboolean   plain;              /* no icons: a smaller thumb off */
  VibeosAnim   *slide;
};

G_DEFINE_FINAL_TYPE (VibeosSwitch, vibeos_switch, GTK_TYPE_WIDGET)

enum { PROP_0, PROP_ACTIVE, N_PROPS };
static GParamSpec *props[N_PROPS];

/* The icons in the thumb: ic_check and ic_close from DeskClock's res/drawable, viewport 960 */
static GskPath *
icon_path (gboolean check)
{
  static GskPath *paths[2];
  if (!paths[check])
    paths[check] = gsk_path_parse (check ? "M382,720L154,492L211,435L382,606L749,239L806,296L382,720Z"
                                         : "M256,760L200,704L424,480L200,256L256,200L480,424L704,200L760,256"
                                           "L536,480L760,704L704,760L480,536L256,760Z");
  return paths[check];
}

static void
vibeos_switch_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  VibeosSwitch *self = VIBEOS_SWITCH (widget);
  double w = gtk_widget_get_width (widget), h = gtk_widget_get_height (widget);
  double x = (w - TRACK_W) / 2, y = (h - TRACK_H) / 2;
  float t = self->position;

  /* every colour blends with the thumb's position, so they change as it slides */
  GdkRGBA track_off = vibeos_color (VIBEOS_SURFACE_CONTAINER_HIGHEST), track_on = vibeos_color (VIBEOS_PRIMARY);
  GdkRGBA outline_off = vibeos_color (VIBEOS_OUTLINE), outline_on = vibeos_color_alpha (VIBEOS_PRIMARY, 0);
  GdkRGBA thumb_off = vibeos_color (VIBEOS_OUTLINE), thumb_on = vibeos_color (VIBEOS_ON_PRIMARY);
  GdkRGBA icon_off = vibeos_color (VIBEOS_SURFACE_CONTAINER_HIGHEST), icon_on = vibeos_color (VIBEOS_ON_PRIMARY_CONTAINER);
  GdkRGBA track = vibeos_rgba_mix (&track_off, &track_on, t);
  GdkRGBA outline = vibeos_rgba_mix (&outline_off, &outline_on, t);
  GdkRGBA thumb = vibeos_rgba_mix (&thumb_off, &thumb_on, t);
  GdkRGBA icon = vibeos_rgba_mix (&icon_off, &icon_on, t);

  GskRoundedRect rect;
  gsk_rounded_rect_init_from_rect (&rect, &GRAPHENE_RECT_INIT (x, y, TRACK_W, TRACK_H), TRACK_H / 2.0);
  gtk_snapshot_push_rounded_clip (snapshot, &rect);
  gtk_snapshot_append_color (snapshot, &track, &rect.bounds);
  gtk_snapshot_pop (snapshot);
  if (outline.alpha > 0) {
    float widths[4] = { TRACK_OUTLINE, TRACK_OUTLINE, TRACK_OUTLINE, TRACK_OUTLINE };
    GdkRGBA colors[4] = { outline, outline, outline, outline };
    gtk_snapshot_append_border (snapshot, &rect, widths, colors);
  }

  /* the thumb's centre slides between the centres of its two places; without icons it grows as it goes */
  double size = self->plain ? THUMB_PLAIN + (THUMB - THUMB_PLAIN) * t : THUMB;
  double travel = TRACK_W - 2 * THUMB_INSET - THUMB;
  double cx = x + THUMB_INSET + THUMB / 2.0 + travel * t, cy = y + TRACK_H / 2.0;
  double tx = cx - size / 2, ty = cy - size / 2;
  gsk_rounded_rect_init_from_rect (&rect, &GRAPHENE_RECT_INIT (tx, ty, size, size), size / 2.0);
  gtk_snapshot_push_rounded_clip (snapshot, &rect);
  gtk_snapshot_append_color (snapshot, &thumb, &rect.bounds);
  gtk_snapshot_pop (snapshot);
  if (self->plain)
    return;

  /* the icon is a state selector, so it swaps at once; only its tint blends */
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (tx + (THUMB - ICON) / 2.0, ty + (THUMB - ICON) / 2.0));
  gtk_snapshot_scale (snapshot, ICON / 960.f, ICON / 960.f);
  gtk_snapshot_append_fill (snapshot, icon_path (self->active), GSK_FILL_RULE_WINDING, &icon);
  gtk_snapshot_restore (snapshot);
}

static void
vibeos_switch_measure (GtkWidget *widget, GtkOrientation orientation, int for_size,
                    int *minimum, int *natural, int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = orientation == GTK_ORIENTATION_HORIZONTAL ? TRACK_W : TARGET_H;
}

static void
on_slide (VibeosAnim *anim, double value, gpointer data)
{
  VibeosSwitch *self = data;
  self->position = value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_released (GtkGestureClick *click, int n_press, double x, double y, VibeosSwitch *self)
{
  vibeos_switch_set_active (self, !self->active);
}

/* ---- GObject --------------------------------------------------------------------------------------------------- */

static void
vibeos_switch_get_property (GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  if (id == PROP_ACTIVE)
    g_value_set_boolean (value, VIBEOS_SWITCH (object)->active);
}

static void
vibeos_switch_set_property (GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
  if (id == PROP_ACTIVE)
    vibeos_switch_set_active (VIBEOS_SWITCH (object), g_value_get_boolean (value));
}

static void
vibeos_switch_dispose (GObject *object)
{
  VibeosSwitch *self = VIBEOS_SWITCH (object);
  g_clear_pointer (&self->slide, vibeos_anim_free);
  G_OBJECT_CLASS (vibeos_switch_parent_class)->dispose (object);
}

static void
vibeos_switch_class_init (VibeosSwitchClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  object_class->get_property = vibeos_switch_get_property;
  object_class->set_property = vibeos_switch_set_property;
  object_class->dispose = vibeos_switch_dispose;
  widget_class->snapshot = vibeos_switch_snapshot;
  widget_class->measure = vibeos_switch_measure;
  gtk_widget_class_set_css_name (widget_class, "vibeos-switch");

  props[PROP_ACTIVE] = g_param_spec_boolean ("active", NULL, NULL, FALSE,
                                             G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
vibeos_switch_init (VibeosSwitch *self)
{
  self->slide = vibeos_anim_new (GTK_WIDGET (self), on_slide, self);
  g_object_set_data (G_OBJECT (self), "vibeos-tappable", GINT_TO_POINTER (TRUE));
  GtkGesture *click = gtk_gesture_click_new ();
  g_signal_connect (click, "released", G_CALLBACK (on_released), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
}

GtkWidget *
vibeos_switch_new (void)
{
  return g_object_new (VIBEOS_TYPE_SWITCH, NULL);
}

gboolean
vibeos_switch_get_active (VibeosSwitch *self)
{
  return self->active;
}

void
vibeos_switch_set_active (VibeosSwitch *self, gboolean active)
{
  active = !!active;
  if (active == self->active)
    return;
  self->active = active;
  /* from where the thumb is, so a second tap mid-slide turns it round; accelerate-decelerate */
  vibeos_anim_start (self->slide, self->position, active, TOGGLE_MS, 0, SPONGY_ACCELERATE_DECELERATE);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ACTIVE]);
}

void
vibeos_switch_set_thumb_icons (VibeosSwitch *self, gboolean icons)
{
  g_return_if_fail (VIBEOS_IS_SWITCH (self));
  self->plain = !icons;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}
