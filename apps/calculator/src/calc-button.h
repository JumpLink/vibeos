/* CalcButton: one key of the pads, ExactCalculator's HapticButton (a Material 3 Expressive MaterialButton).
 *
 * A pill in the key's container colour, inset 4 dp top and bottom inside the key's cell, with its label centred:
 * text at 0.4 of the key's height (HapticButton.onSizeChanged: setTextSize (h * 0.4f)), or an icon (the delete
 * key's, 48 dp, button_iconsize). A press shows the ripple (neo-ripple.h) in the label's colour at 10 % alpha,
 * which is also what lightens the pill while the finger is down (measured on the recording: +17 of 255 on the
 * numeric pill, back to nothing ~300 ms after the release).
 *
 * Signals: "clicked" () on a tap, "long-pressed" () -> gboolean after 400 ms held: a handler that returns TRUE
 * takes the touch (no "clicked" then), as an OnLongClickListener does.
 *
 * Left out: the haptic tick on every press (HapticButton's performHapticFeedback (VIRTUAL_KEY)), the phone has no
 * vibrator API for apps here; and the Expressive corner morph while pressed, which on the recording changes the
 * corners by at most 5 px for one or two frames. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* the button themes (themes.xml: Theme.Button.*) */
typedef enum {
  CALC_BUTTON_ADVANCED,   /* colorSurface / colorOnSurface: no visible pill */
  CALC_BUTTON_NUMERIC,    /* colorSurfaceVariant / colorOnSurface */
  CALC_BUTTON_OPERATOR,   /* colorSecondaryContainer / colorOnSecondaryContainer */
  CALC_BUTTON_CLEAR,      /* colorSurfaceVariant / colorOnSurfaceVariant: the delete key */
  CALC_BUTTON_ACTION,     /* colorPrimaryContainer / colorOnPrimaryContainer: AC */
  CALC_BUTTON_EQUALS,     /* colorTertiary / colorOnTertiary */
} CalcButtonTheme;

#define CALC_TYPE_BUTTON (calc_button_get_type ())
G_DECLARE_FINAL_TYPE (CalcButton, calc_button, CALC, BUTTON, GtkWidget)

GtkWidget  *calc_button_new        (const char *label, CalcButtonTheme theme);
GtkWidget  *calc_button_new_icon   (const char *icon_name, double icon_size, CalcButtonTheme theme);
void        calc_button_set_label  (CalcButton *self, const char *label);
const char *calc_button_get_label  (CalcButton *self);
/* the key's size in dp, fractional (CalcPad allocates the whole logical pixels around it) */
void        calc_button_set_exact_size (CalcButton *self, double width, double height);
/* the font weight of the label (CALC_WEIGHT_*) */
void        calc_button_set_weight (CalcButton *self, int weight);

G_END_DECLS
