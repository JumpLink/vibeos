/* vibeos-anim: one number animated on a widget's frame clock, along an easing curve.
 *
 *   self->expand = vibeos_anim_new (widget, on_expand, self);
 *   vibeos_anim_start (self->expand, 0, 1, 150, 0, SPONGY_FAST_OUT_SLOW_IN);
 *
 * The callback runs once per frame with the eased value, and a last time with the end value, when
 * vibeos_anim_is_running () is already FALSE. Durations and delays are in milliseconds. The animation belongs to the
 * widget's owner, which frees it in its dispose; it stops by itself if the widget goes away first. */
#pragma once

#include <gtk/gtk.h>
#include <spongy.h>

G_BEGIN_DECLS

typedef struct _VibeosAnim VibeosAnim;
typedef void (*VibeosAnimFunc) (VibeosAnim *anim, double value, gpointer data);

VibeosAnim *vibeos_anim_new        (GtkWidget *widget, VibeosAnimFunc func, gpointer data);
void     vibeos_anim_free       (VibeosAnim *anim);

/* From `from` to `to` along one of Android's curves. A widget that is not on screen jumps to the end. */
void     vibeos_anim_start      (VibeosAnim *anim, double from, double to, guint duration, guint delay, SpongyCurve curve);
/* the same along any cubic-bezier (x1, y1, x2, y2) */
void     vibeos_anim_start_cubic (VibeosAnim *anim, double from, double to, guint duration, guint delay,
                               float x1, float y1, float x2, float y2);
/* stops where it is, without the last callback */
void     vibeos_anim_stop       (VibeosAnim *anim);
/* jumps to the end, with the last callback */
void     vibeos_anim_finish     (VibeosAnim *anim);

gboolean vibeos_anim_is_running (VibeosAnim *anim);
double   vibeos_anim_get_value  (VibeosAnim *anim);
/* the target of the run in progress (or of the last one) */
double   vibeos_anim_get_to     (VibeosAnim *anim);

G_END_DECLS
