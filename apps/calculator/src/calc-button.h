/* CalcButton: one key of the pads.
 *
 * A pill in the key's container colour, inset top and bottom inside the key's cell, with its label centred: text
 * at 0.4 of the key's height, so it grows with the key, or an icon (the delete key's). A press shows the ripple
 * (neo-ripple.h) in the label's colour, which is also what lightens the pill while the finger is down, back to
 * nothing ~300 ms after the release.
 *
 * Signals: "clicked" () on a tap, "long-pressed" () -> gboolean after 400 ms held: a handler that returns TRUE
 * takes the touch (no "clicked" then).
 *
 * Left out: a haptic tick on every press, the phone has no vibrator API for apps here; and a corner morph while
 * pressed, which would change the corners by at most 5 px for one or two frames. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* the button themes: the colours of each kind of key */
typedef enum {
  CALC_BUTTON_ADVANCED,   /* the advanced pad's keys: no visible pill */
  CALC_BUTTON_NUMERIC,    /* the digits and the point */
  CALC_BUTTON_OPERATOR,   /* the operators, ( ) and % */
  CALC_BUTTON_CLEAR,      /* the delete key */
  CALC_BUTTON_ACTION,     /* AC */
  CALC_BUTTON_EQUALS,     /* = */
} CalcButtonTheme;

#define CALC_TYPE_BUTTON (calc_button_get_type ())
G_DECLARE_FINAL_TYPE (CalcButton, calc_button, CALC, BUTTON, GtkWidget)

GtkWidget  *calc_button_new        (const char *label, CalcButtonTheme theme);
GtkWidget  *calc_button_new_icon   (const char *icon_name, double icon_size, CalcButtonTheme theme);
void        calc_button_set_label  (CalcButton *self, const char *label);
const char *calc_button_get_label  (CalcButton *self);
/* the key's size in px, fractional (CalcPad allocates the whole logical pixels around it) */
void        calc_button_set_exact_size (CalcButton *self, double width, double height);
/* the font weight of the label (CALC_WEIGHT_*) */
void        calc_button_set_weight (CalcButton *self, int weight);

G_END_DECLS
