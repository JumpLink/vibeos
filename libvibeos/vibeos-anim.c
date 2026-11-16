/* vibeos-anim: see vibeos-anim.h. */
#include "vibeos-anim.h"

struct _VibeosAnim {
  GtkWidget   *widget;          /* weak: NULL once it is gone */
  VibeosAnimFunc  func;
  gpointer     data;
  guint        tick;            /* the tick callback while running */

  double       from, to, value;
  guint        duration, delay;
  gint64       start_us;        /* 0 until the first frame: like Android, the clock starts on the next frame */
  gboolean     cubic;
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

static gboolean
on_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  VibeosAnim *anim = data;
  gint64 now = gdk_frame_clock_get_frame_time (clock);
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
  anim->cubic = FALSE;
  anim->curve = curve;
  start (anim, from, to, duration, delay);
}

void
vibeos_anim_start_cubic (VibeosAnim *anim, double from, double to, guint duration, guint delay,
                      float x1, float y1, float x2, float y2)
{
  anim->cubic = TRUE;
  spongy_interp_cubic (&anim->interp, x1, y1, x2, y2);
  start (anim, from, to, duration, delay);
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
