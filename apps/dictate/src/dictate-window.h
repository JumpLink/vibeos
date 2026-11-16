/* The Dictate window: the app bar (the title, Copy, Clear), the document, and the microphone FAB under it.
 *
 * What is said goes into the document where its cursor was when the microphone started: the sentence being spoken
 * in a lighter colour as the recogniser hears it, in the text's own colour once the speaker finished it (a pause).
 * The document is ordinary editable text, kept in the user's data directory (vibeos-dictate/document.txt) from one
 * start of the app to the next. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

GtkWidget *dictate_window_new              (GtkApplication *app);
/* what the microphone FAB does: start listening, or stop */
void       dictate_window_toggle_dictation (GtkWidget *window);

G_END_DECLS
