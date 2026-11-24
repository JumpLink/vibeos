/* The Dictate window: the app bar (the title, Copy, Clear), the document, and the microphone FAB under it.
 *
 * What is said goes into the document where its cursor was when the microphone started: the sentence being spoken
 * in a lighter colour as the recogniser hears it, in the text's own colour once the speaker finished it (a pause).
 * The document is ordinary editable text, kept in the user's data directory (vibeos-dictate/document.txt) from one
 * start of the app to the next. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* agent: Agent's window, a spoken conversation with the model on the phone (Talk) instead of the document */
GtkWidget *dictate_window_new              (GtkApplication *app, gboolean agent);
/* what the microphone FAB does: start listening, or stop */
void       dictate_window_toggle_dictation (GtkWidget *window);
/* the power key held (`held`) and let go: Agent listens in between, then answers; `locked`: the phone is locked, and
 * Agent only reads until dictate_window_unlocked () (or a hold on the unlocked phone) */
void       dictate_window_hold             (GtkWidget *window, gboolean held, gboolean locked);
/* the hold given up while the key is still down (it became part of a key chord): listening stops, nothing is sent */
void       dictate_window_hold_cancel      (GtkWidget *window);
void       dictate_window_unlocked         (GtkWidget *window);

/* Agent's: the call `id` rings, and is said aloud, then Agent listens for what to do (the window need not be on the
 * screen); it stopped ringing; the calls now (DictateCall, dictate-calls.h), for what the model is told goes on */
void       dictate_window_announce_call    (GtkWidget *window, guint id);
void       dictate_window_call_stopped     (GtkWidget *window, guint id);
void       dictate_window_calls_changed    (GtkWidget *window, GPtrArray *calls);

G_END_DECLS
