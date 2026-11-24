/* TermLayout: the window's content, for an on-screen keyboard that comes up over the window (the desktop file's
 * X-Vibeos-Keyboard=overlay). The window keeps its size and nothing in it is laid out again: the terminal's grid
 * stays as it is, no text is rewrapped. The shell says how much of the window the keyboard covers (vibeos.Shell's
 * KeyboardOverlap, announced as the keyboard starts to move), and the layout makes room itself:
 *
 *  - the extra keys (term-keys.h) stand on the keyboard's top edge while it is up, coming up from behind it;
 *  - the content (the top bar and the terminal) slides up, just far enough to keep the text cursor's line above the
 *    extra keys and never further than the terminal's last row onto them. It follows the cursor while the keyboard
 *    is up (output takes it down, `clear` back up), and slides back down with the keyboard.
 *
 * Both move as the keyboard does (keyboard.js: 200 ms decelerating up, 190 ms accelerating down). With no shell to
 * say (another desktop), nothing moves and the extra keys stay hidden. */
#pragma once

#include <gtk/gtk.h>

#include "term-view.h"

G_BEGIN_DECLS

#define TERM_TYPE_LAYOUT (term_layout_get_type ())
G_DECLARE_FINAL_TYPE (TermLayout, term_layout, TERM, LAYOUT, GtkWidget)

/* `content` holds `view`, whose text cursor the content's slide keeps in sight; `keys` is the extra keys' row */
GtkWidget *term_layout_new (GtkWidget *content, TermView *view, GtkWidget *keys);

G_END_DECLS
