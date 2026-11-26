/* vibeos-anim: see vibeos-anim.h. */
#include "vibeos-anim.h"

#include <math.h>

struct _VibeosAnim {
  GtkWidget   *widget;          /* weak: NULL once it is gone */
  VibeosAnimFunc  func;
  gpointer     data;
  guint        tick;            /* the tick callback while running */

  double       from, to, value;
  guint        duration, delay;
  gint64       start_us;        /* 0 until the first frame: like Android, the clock starts on the next frame */
  gboolean     cubic;
  gboolean     modal;           /* on the modal motion's spring rather than a curve */
  SpongySpring spring;
  SpongyCurve  curve;
  SpongyInterp interp;
};

VibeosAnim *
vibeos_anim_new (GtkWidget *widget, VibeosAnimFunc func, gpointer data)
{
  VibeosAnim *anim = g_new0 (VibeosAnim, 1);
  anim->widget = widget;
  anim->func = func;
  anim->data = data;
  g_object_add_weak_pointer (G_OBJECT (widget), (gpointer *) &anim->widget);
  return anim;
}

void
vibeos_anim_free (VibeosAnim *anim)
{
  if (!anim)
    return;
  vibeos_anim_stop (anim);
  if (anim->widget)
    g_object_remove_weak_pointer (G_OBJECT (anim->widget), (gpointer *) &anim->widget);
  g_free (anim);
}

static double
ease (VibeosAnim *anim, double t)
{
  return anim->cubic ? spongy_interp_get (&anim->interp, t) : spongy_curve (anim->curve, t);
}

/* each frame of the modal motion, with G_MESSAGES_DEBUG=vibeos-modal: its time from the motion's start and its value
 * (VibeosSheet's floating card logs its own the same way, to set the two side by side) */
static void
modal_trace (VibeosAnim *anim, gint64 now)
{
  if (g_log_writer_default_would_drop (G_LOG_LEVEL_DEBUG, "vibeos-modal"))
    return;
  g_log ("vibeos-modal", G_LOG_LEVEL_DEBUG, "%s %s: frame %.1f ms, value %.4f", G_OBJECT_TYPE_NAME (anim->widget),
         anim->to > anim->from ? "open" : "close", (now - anim->start_us) / 1000.0, anim->value);
}

/* the modal motion's frame: the spring stepped to now; closing, it stops where it is gone */
static gboolean
modal_tick (VibeosAnim *anim, GdkFrameClock *clock, gint64 now)
{
  gboolean moving;

  if (!anim->start_us)
    {
      gint64 interval = 0;

      /* its clock a frame back: the first frame is already a frame into the motion */
      gdk_frame_clock_get_refresh_info (clock, now, &interval, NULL);
      anim->start_us = now - (interval > 0 ? interval : 16667);
      spongy_spring_animate (&anim->spring, anim->from, anim->spring.velocity, anim->to, anim->start_us / 1000.0);
    }
  moving = spongy_spring_update (&anim->spring, now / 1000.0);
  if (anim->to < anim->from && anim->spring.value <= anim->to)
    moving = FALSE;
  if (!moving)
    {
      anim->tick = 0;
      anim->spring.velocity = 0;
      anim->value = anim->to;
      modal_trace (anim, now);
      anim->func (anim, anim->value, anim->data);
      return G_SOURCE_REMOVE;
    }
  anim->value = anim->spring.value;
  modal_trace (anim, now);
  anim->func (anim, anim->value, anim->data);
  return G_SOURCE_CONTINUE;
}

static gboolean
on_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  VibeosAnim *anim = data;
  gint64 now = gdk_frame_clock_get_frame_time (clock);
  if (anim->modal)
    return modal_tick (anim, clock, now);
  if (!anim->start_us)
    anim->start_us = now + anim->delay * 1000;

  double t = anim->duration ? (now - anim->start_us) / (anim->duration * 1000.0) : 1;
  if (t < 0)
    return G_SOURCE_CONTINUE;          /* still in its delay */
  if (t >= 1) {
    anim->tick = 0;
    anim->value = anim->to;
    anim->func (anim, anim->value, anim->data);
    return G_SOURCE_REMOVE;
  }
  anim->value = anim->from + (anim->to - anim->from) * ease (anim, t);
  anim->func (anim, anim->value, anim->data);
  return G_SOURCE_CONTINUE;
}

static void
start (VibeosAnim *anim, double from, double to, guint duration, guint delay)
{
  vibeos_anim_stop (anim);
  anim->from = from;
  anim->to = to;
  anim->value = from;
  anim->duration = duration;
  anim->delay = delay;
  anim->start_us = 0;
  if (!anim->widget || !gtk_widget_get_mapped (anim->widget)) {
    /* nothing to see: no frames would come, so go straight to the end */
    anim->value = to;
    anim->func (anim, to, anim->data);
    return;
  }
  anim->tick = gtk_widget_add_tick_callback (anim->widget, on_tick, anim, NULL);
  /* until its first frame (or through its delay) it shows the start value */
  anim->func (anim, from, anim->data);
}

void
vibeos_anim_start (VibeosAnim *anim, double from, double to, guint duration, guint delay, SpongyCurve curve)
{
  anim->modal = FALSE;
  anim->cubic = FALSE;
  anim->curve = curve;
  start (anim, from, to, duration, delay);
}

void
vibeos_anim_start_cubic (VibeosAnim *anim, double from, double to, guint duration, guint delay,
                      float x1, float y1, float x2, float y2)
{
  anim->modal = FALSE;
  anim->cubic = TRUE;
  spongy_interp_cubic (&anim->interp, x1, y1, x2, y2);
  start (anim, from, to, duration, delay);
}

void
vibeos_anim_start_modal (VibeosAnim *anim, gboolean open)
{
  gboolean animate = TRUE;
  /* a spring turned round keeps its speed; one at rest starts from rest */
  double velocity = anim->modal && anim->tick ? anim->spring.velocity : 0;

  if (anim->widget)
    g_object_get (gtk_widget_get_settings (anim->widget), "gtk-enable-animations", &animate, NULL);
  anim->modal = TRUE;
  spongy_spring_init (&anim->spring, VIBEOS_MODAL_STIFFNESS, VIBEOS_MODAL_DAMPING, 0.001);
  anim->spring.velocity = velocity;
  if (!animate)
    {
      vibeos_anim_stop (anim);
      anim->from = anim->value = anim->to = open ? 1 : 0;
      anim->func (anim, anim->value, anim->data);
      return;
    }
  start (anim, anim->value, open ? 1 : 0, 0, 0);
}

void
vibeos_modal_snapshot_push (GtkSnapshot *snapshot, double value, const graphene_point_t *origin)
{
  double scale = VIBEOS_MODAL_MIN_SCALE + (1 - VIBEOS_MODAL_MIN_SCALE) * value;

  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, origin);
  gtk_snapshot_scale (snapshot, scale, scale);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (-origin->x, -origin->y));
  gtk_snapshot_push_opacity (snapshot, CLAMP (value, 0, 1));
}

void
vibeos_modal_snapshot_pop (GtkSnapshot *snapshot)
{
  gtk_snapshot_pop (snapshot);
  gtk_snapshot_restore (snapshot);
}

void
vibeos_modal_menu_origin (const graphene_rect_t *card, const graphene_rect_t *anchor, graphene_point_t *origin)
{
  double ax = anchor->origin.x + anchor->size.width / 2, ay = anchor->origin.y + anchor->size.height / 2;

  origin->x = fabs (ax - card->origin.x) <= fabs (ax - (card->origin.x + card->size.width))
              ? card->origin.x : card->origin.x + card->size.width;
  origin->y = fabs (ay - card->origin.y) <= fabs (ay - (card->origin.y + card->size.height))
              ? card->origin.y : card->origin.y + card->size.height;
}

void
vibeos_anim_stop (VibeosAnim *anim)
{
  if (anim->tick && anim->widget)
    gtk_widget_remove_tick_callback (anim->widget, anim->tick);
  anim->tick = 0;
}

void
vibeos_anim_finish (VibeosAnim *anim)
{
  if (!anim->tick)
    return;
  vibeos_anim_stop (anim);
  anim->value = anim->to;
  anim->func (anim, anim->to, anim->data);
}

gboolean
vibeos_anim_is_running (VibeosAnim *anim)
{
  return anim->tick != 0;
}

double
vibeos_anim_get_value (VibeosAnim *anim)
{
  return anim->value;
}

double
vibeos_anim_get_to (VibeosAnim *anim)
{
  return anim->to;
}
