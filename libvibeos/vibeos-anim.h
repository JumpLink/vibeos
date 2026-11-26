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

/* The modal motion: how About's dialog card opens (a VibeosSheet floating on a tablet) and every ⋮ menu: it grows from
 * VIBEOS_MODAL_MIN_SCALE as it fades in, on a spring that goes a little past (libadwaita's dialog: stiffness 500,
 * damping 0.62), and closes back on the same spring, stopping where it is gone rather than going past. The value
 * runs from 0 (gone) to 1 (in place), a little over 1 as it goes past; draw it with vibeos_modal_snapshot_push. The
 * first frame is already a frame into the motion, so what opens shows on the frame after the tap. With Reduce
 * animations on (gtk-enable-animations off) it jumps to the end. */
#define VIBEOS_MODAL_MIN_SCALE  0.8
#define VIBEOS_MODAL_STIFFNESS  500.0
#define VIBEOS_MODAL_DAMPING    0.62
void     vibeos_anim_start_modal (VibeosAnim *anim, gboolean open);

/* What is drawn between the two, at the modal motion's `value`: scaled about `origin` and faded */
void     vibeos_modal_snapshot_push (GtkSnapshot *snapshot, double value, const graphene_point_t *origin);
void     vibeos_modal_snapshot_pop  (GtkSnapshot *snapshot);
/* A menu's `origin`: the corner of its card nearest the anchor it opened from (a ⋮ over its right end: the top right),
 * so that it grows out of the button */
void     vibeos_modal_menu_origin (const graphene_rect_t *card, const graphene_rect_t *anchor, graphene_point_t *origin);

gboolean vibeos_anim_is_running (VibeosAnim *anim);
double   vibeos_anim_get_value  (VibeosAnim *anim);
/* the target of the run in progress (or of the last one) */
double   vibeos_anim_get_to     (VibeosAnim *anim);

G_END_DECLS
