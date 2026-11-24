/* term-window: a Terminal window, its shell running in it from the start and the window closing once the shell has
 * exited.
 *
 * From the top: a top bar with the title the program gives the window (the shell's prompt sets it to the user, the
 * host and the directory; "Terminal" until one is set) and the ⋮ menu (New window, Paste, About); the terminal
 * (term-view.h); and under it, empty for now and hidden, the place for a row of extra keys (Esc, Tab, Ctrl, the
 * arrows), which goes between the terminal and the keyboard.
 *
 * Keys the window has before the terminal: Escape (and the back gesture, which sends it while the selection's toolbar
 * shows) ends the selection when there is one, rather than going to the program; Ctrl+Shift+C copies and
 * Ctrl+Shift+V pastes, as in GNOME Console. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

GtkWidget *term_window_new (GtkApplication *app);

G_END_DECLS
