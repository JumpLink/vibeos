/* neo-anim: one number animated on a widget's frame clock, along an easing curve.
 *
 *   self->expand = neo_anim_new (widget, on_expand, self);
 *   neo_anim_start (self->expand, 0, 1, 150, 0, SPONGY_FAST_OUT_SLOW_IN);
 *
 * The callback runs once per frame with the eased value, and a last time with the end value, when
 * neo_anim_is_running () is already FALSE. Durations and delays are in milliseconds. The animation belongs to the
 * widget's owner, which frees it in its dispose; it stops by itself if the widget goes away first. */
#pragma once

#include <gtk/gtk.h>
#include <spongy.h>

G_BEGIN_DECLS

typedef struct _NeoAnim NeoAnim;
typedef void (*NeoAnimFunc) (NeoAnim *anim, double value, gpointer data);

NeoAnim *neo_anim_new        (GtkWidget *widget, NeoAnimFunc func, gpointer data);
void     neo_anim_free       (NeoAnim *anim);

/* From `from` to `to` along one of Android's curves. A widget that is not on screen jumps to the end. */
void     neo_anim_start      (NeoAnim *anim, double from, double to, guint duration, guint delay, SpongyCurve curve);
/* the same along any cubic-bezier (x1, y1, x2, y2) */
void     neo_anim_start_cubic (NeoAnim *anim, double from, double to, guint duration, guint delay,
                               float x1, float y1, float x2, float y2);
/* stops where it is, without the last callback */
void     neo_anim_stop       (NeoAnim *anim);
/* jumps to the end, with the last callback */
void     neo_anim_finish     (NeoAnim *anim);

gboolean neo_anim_is_running (NeoAnim *anim);
double   neo_anim_get_value  (NeoAnim *anim);
/* the target of the run in progress (or of the last one) */
double   neo_anim_get_to     (NeoAnim *anim);

G_END_DECLS
