/* TermKeys: the extra keys, Termux's row of the keys a phone's keyboard lacks (Esc, Tab, Ctrl, Alt and the cursor
 * keys), standing on the on-screen keyboard while it is up (term-layout.h).
 *
 * Esc, Tab and the cursor keys send their bytes to the session as they are pressed (term-input.h; the cursor keys in
 * the program's cursor key mode), and a cursor key held repeats as a keyboard's keys do. Ctrl and Alt are sticky, as
 * Termux's: a tap puts one on for the next key, of the row or typed (term_keys_filter ()), and it goes off after that
 * key; a long press locks it on until a tap puts it off; a tap on one that is on puts it off.
 *
 * Nothing in the row takes the focus: the terminal keeps it, and the keyboard stays up.
 *
 * Signals: "sent": the row sent a key (the window shows the end of the text, as a key typed does). */
#pragma once

#include <gtk/gtk.h>

#include "term-session.h"

G_BEGIN_DECLS

#define TERM_TYPE_KEYS (term_keys_get_type ())
G_DECLARE_FINAL_TYPE (TermKeys, term_keys, TERM, KEYS, GtkWidget)

TermKeys *term_keys_new         (void);
/* where the keys' bytes go, and whose modes say how the cursor keys are sent */
void      term_keys_set_session (TermKeys *self, TermSession *session);
/* What the terminal is sending (VteTerminal::commit): one character typed, with Ctrl or Alt on, is written changed
 * (TRUE) and the modifier goes off unless locked; anything else is left to be written as it is (FALSE). A program's
 * answers to its queries (they start with ESC) never take a modifier. */
gboolean  term_keys_filter      (TermKeys *self, const char *text, gsize length);

G_END_DECLS
