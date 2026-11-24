/* VibeosPressable: one child made tappable, with the ripple (vibeos-ripple.h) behind it.
 *
 * It is anything tapped that shows a ripple: a list row, a text button, an icon button, a FAB. It emits "clicked" on a
 * tap and "long-pressed" after 400 ms held still; a press it loses to a scroll (a drag past the slop, a scroll view
 * taking the touch) does neither, and its ripple just fades.
 *
 * Inside a scroll view the ripple waits 100 ms before it starts, so a finger that is about to scroll does not flash the
 * row it began on. A tap shorter than that still shows the whole ripple.
 *
 * A press on a child that takes taps itself (another VibeosPressable, a VibeosSwitch, or any widget with the object data
 * "vibeos-tappable" set) is left to that child.
 *
 * The CSS name is "vibeos-pressable"; style it with classes. */
#pragma once

#include <gtk/gtk.h>

#include "vibeos-style.h"

G_BEGIN_DECLS

typedef enum {
  VIBEOS_RIPPLE_BOUNDED,      /* clipped to the widget's box, corners rounded by `radius` (?selectableItemBackground) */
  VIBEOS_RIPPLE_CIRCLE,       /* clipped to a circle of `radius` around the centre (icon buttons, a FAB) */
  VIBEOS_RIPPLE_NONE,         /* tappable, no ripple (the alarm card) */
} VibeosRippleShape;

#define VIBEOS_TYPE_PRESSABLE (vibeos_pressable_get_type ())
G_DECLARE_FINAL_TYPE (VibeosPressable, vibeos_pressable, VIBEOS, PRESSABLE, GtkWidget)

GtkWidget *vibeos_pressable_new          (GtkWidget *child);
void       vibeos_pressable_set_child    (VibeosPressable *self, GtkWidget *child);
GtkWidget *vibeos_pressable_get_child    (VibeosPressable *self);

void       vibeos_pressable_set_ripple   (VibeosPressable *self, VibeosRippleShape shape, double radius);
/* the default is vibeos_ripple_default_color () */
void       vibeos_pressable_set_ripple_color (VibeosPressable *self, const GdkRGBA *color);

/* A fill in the widget's shape (and a raised button's shadow under it when `elevated`): what a FAB's background
 * tint is. Unlike a CSS background it scales with vibeos_pressable_set_scale. */
void       vibeos_pressable_set_fill     (VibeosPressable *self, const GdkRGBA *fill, gboolean elevated);
/* The same fill in a colour role of the theme, read as the widget draws: it follows the theme as it changes, as the
 * FABs' fills do. vibeos_pressable_set_fill () sets a fixed colour again. */
void       vibeos_pressable_set_fill_role (VibeosPressable *self, VibeosColor role, gboolean elevated);

/* Draw-time scale about the centre (Android's scaleX/scaleY): the FAB shows and hides with it. 0 hides it and
 * makes it untappable; the layout does not change. */
void       vibeos_pressable_set_scale    (VibeosPressable *self, double scale);
double     vibeos_pressable_get_scale    (VibeosPressable *self);

/* A text button (Delete, the dialog buttons, a snackbar action): a label in a pressable with .vibeos-text-button */
GtkWidget *vibeos_text_button_new        (const char *label);
/* An icon button: a symbolic icon centred in a thumb-sized target, with a circular ripple */
GtkWidget *vibeos_icon_button_new        (const char *icon_name);

/* A floating action button: a large circle in the primary colour (`large`, the screen's main action), or a smaller
 * one in the secondary colour (the actions beside it). Both are raised, with a shadow. */
GtkWidget *vibeos_fab_new                (const char *icon_name, gboolean large);
void       vibeos_fab_set_icon           (VibeosPressable *fab, const char *icon_name);

/* TRUE when (x, y) in `widget` is on a descendant that takes taps itself (one with the "vibeos-tappable" object
 * data): a widget with its own tap gesture lets those through, as VibeosPressable does */
gboolean   vibeos_pick_is_tappable       (GtkWidget *widget, double x, double y);

/* Signals: "clicked" (), "long-pressed" () */

G_END_DECLS
