/* TermToolbar: the floating text toolbar over the terminal's selection (Copy, Paste, Select all), inside the app's
 * own window.
 *
 * A row of text buttons on a raised card, centred over the text it is about and 8 px above it, or under it when there
 * is no room above, kept 16 px inside the window. It takes no focus: the terminal keeps it, and with it the keyboard
 * stays as it was. Its accessible role is a dialog's, so that VibeOS's back gesture sends Escape, which the window
 * takes for closing it (term-window.c) before the terminal would pass it on to the program. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define TERM_TYPE_TOOLBAR (term_toolbar_get_type ())
G_DECLARE_FINAL_TYPE (TermToolbar, term_toolbar, TERM, TOOLBAR, GtkWidget)

TermToolbar *term_toolbar_new     (void);
/* a button at the end; returns it (a VibeosPressable: connect to "clicked") */
GtkWidget   *term_toolbar_add     (TermToolbar *self, const char *label);
/* shows it in the root overlay of `widget`'s window, about `content` (in `widget`'s coordinates) */
void         term_toolbar_show_at (TermToolbar *self, GtkWidget *widget, const graphene_rect_t *content);
void         term_toolbar_hide    (TermToolbar *self);
gboolean     term_toolbar_is_shown (TermToolbar *self);

G_END_DECLS
