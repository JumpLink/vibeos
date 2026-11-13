/* NeoPressable: one child made tappable, with the ripple (neo-ripple.h) behind it.
 *
 * It is anything tapped that shows a ripple: a list row, a text button, an icon button, a FAB. It emits "clicked" on a
 * tap and "long-pressed" after 400 ms held still; a press it loses to a scroll (a drag past the slop, a scroll view
 * taking the touch) does neither, and its ripple just fades.
 *
 * Inside a scroll view the ripple waits 100 ms before it starts, so a finger that is about to scroll does not flash the
 * row it began on. A tap shorter than that still shows the whole ripple.
 *
 * A press on a child that takes taps itself (another NeoPressable, a NeoSwitch, or any widget with the object data
 * "neo-tappable" set) is left to that child.
 *
 * The CSS name is "neo-pressable"; style it with classes. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef enum {
  NEO_RIPPLE_BOUNDED,      /* clipped to the widget's box, corners rounded by `radius` (?selectableItemBackground) */
  NEO_RIPPLE_CIRCLE,       /* clipped to a circle of `radius` around the centre (icon buttons, a FAB) */
  NEO_RIPPLE_NONE,         /* tappable, no ripple (the alarm card) */
} NeoRippleShape;

#define NEO_TYPE_PRESSABLE (neo_pressable_get_type ())
G_DECLARE_FINAL_TYPE (NeoPressable, neo_pressable, NEO, PRESSABLE, GtkWidget)

GtkWidget *neo_pressable_new          (GtkWidget *child);
void       neo_pressable_set_child    (NeoPressable *self, GtkWidget *child);
GtkWidget *neo_pressable_get_child    (NeoPressable *self);

void       neo_pressable_set_ripple   (NeoPressable *self, NeoRippleShape shape, double radius);
/* the default is neo_ripple_default_color () */
void       neo_pressable_set_ripple_color (NeoPressable *self, const GdkRGBA *color);

/* A fill in the widget's shape (and a raised button's shadow under it when `elevated`): what a FAB's background
 * tint is. Unlike a CSS background it scales with neo_pressable_set_scale. */
void       neo_pressable_set_fill     (NeoPressable *self, const GdkRGBA *fill, gboolean elevated);

/* Draw-time scale about the centre (Android's scaleX/scaleY): the FAB shows and hides with it. 0 hides it and
 * makes it untappable; the layout does not change. */
void       neo_pressable_set_scale    (NeoPressable *self, double scale);
double     neo_pressable_get_scale    (NeoPressable *self);

/* A text button (Delete, the dialog buttons, a snackbar action): a label in a pressable with .neo-text-button */
GtkWidget *neo_text_button_new        (const char *label);
/* An icon button: a symbolic icon centred in a thumb-sized target, with a circular ripple */
GtkWidget *neo_icon_button_new        (const char *icon_name);

/* A floating action button: a large circle in the primary colour (`large`, the screen's main action), or a smaller
 * one in the secondary colour (the actions beside it). Both are raised, with a shadow. */
GtkWidget *neo_fab_new                (const char *icon_name, gboolean large);
void       neo_fab_set_icon           (NeoPressable *fab, const char *icon_name);

/* TRUE when (x, y) in `widget` is on a descendant that takes taps itself (one with the "neo-tappable" object
 * data): a widget with its own tap gesture lets those through, as NeoPressable does */
gboolean   neo_pick_is_tappable       (GtkWidget *widget, double x, double y);

/* Signals: "clicked" (), "long-pressed" () */

G_END_DECLS
