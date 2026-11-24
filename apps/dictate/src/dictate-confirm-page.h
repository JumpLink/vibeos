/* dictate-confirm-page: Settings' Confirmations: what Vibe does before it acts, by kind of action, as the code does
 * it: what it reads it says at once; what can be undone it does and offers to undo; a message or a call it shows and
 * waits for the tap or "send", or (vibeos.Dictate outward-confirm 'countdown', which DictateTalk enforces) reads back
 * and sends after three seconds unless cancelled. Kinds the code has no rule of its own for are not listed. */
#pragma once

#include <gtk/gtk.h>
#include <vibeos.h>

G_BEGIN_DECLS

GtkWidget *dictate_confirm_page_new (VibeosPageStack *pages, GSettings *settings);
/* the row's value in Settings: what a text or a call waits for */
char      *dictate_confirm_summary  (GSettings *settings);

G_END_DECLS
