/* The dialpad: the twelve keys under a row with the digits typed.
 *
 * Two kinds, as the Dialer has them:
 *  - DIALPAD_DIALER, the main screen's: the digits can be edited (a key adds one, backspace takes one off, a long
 *    press on it clears them all, a long press on 0 makes it a +), with ⋮ and ⌫ beside them;
 *  - DIALPAD_INCALL, the in-call keypad: each key is a touch tone, the digits only show what was sent, and an ✕ at
 *    the start closes the keypad. Under the keys it leaves room for the end-call button, which stays in front.
 *
 * The keys say when they go down and up ("key-down", "key-up"), for touch tones that last as long as the press. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef enum
{
  DIALPAD_DIALER,
  DIALPAD_INCALL,
} DialpadMode;

#define DIALER_TYPE_DIALPAD (dialer_dialpad_get_type ())
G_DECLARE_FINAL_TYPE (DialerDialpad, dialer_dialpad, DIALER, DIALPAD, GtkWidget)

GtkWidget  *dialer_dialpad_new (DialpadMode mode);
const char *dialer_dialpad_get_digits (DialerDialpad *self);
void        dialer_dialpad_set_digits (DialerDialpad *self, const char *digits);
/* adds @c to the digits (the in-call keypad shows the tone it sent) */
void        dialer_dialpad_append (DialerDialpad *self, char c);
/* compact, for a phone held sideways: lower keys, each with its digit and letters on one line */
void        dialer_dialpad_set_compact (DialerDialpad *self, gboolean compact);

/* Signals:
 *   "key-down" (int c): '0'…'9', '*', '#' pressed
 *   "key-up" (int c): released, or the press was taken away
 *   "key-long-pressed" (int c): held 400 ms
 *   "digits-changed" ()
 *   "close" (): the in-call keypad's ✕
 *   "overflow" (GtkWidget *anchor): the main dialpad's ⋮ */

G_END_DECLS
