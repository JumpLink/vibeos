/* NeoDialog: an Android alert dialog, drawn inside the app's own window (neo_window_new) over a 60 % black scrim.
 *
 * A 28 dp-cornered card in surfaceContainerHighest: an optional title, a message or any content, and text buttons
 * at the bottom end. It comes in with Android's Animation.Material.Dialog (alpha 0 -> 1 while rising 20 dp, 220 ms,
 * the standard curve) and leaves the same way (alpha 1 -> 0 while rising 10 dp, 150 ms, standard-accelerate).
 *
 *   NeoDialog *d = neo_dialog_new ("Label");
 *   neo_dialog_set_content (d, entry);
 *   neo_dialog_add_button (d, "Cancel", NEO_RESPONSE_CANCEL);
 *   neo_dialog_add_button (d, "OK", NEO_RESPONSE_OK);
 *   g_signal_connect (d, "response", G_CALLBACK (on_label_response), self);
 *   neo_dialog_present (d, GTK_WIDGET (self));
 *
 * "response" fires once with the button's response (a tap on the scrim, Escape or Back give NEO_RESPONSE_CANCEL),
 * then the dialog animates out and destroys itself. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

enum {
  NEO_RESPONSE_CANCEL = -1,
  NEO_RESPONSE_OK = 1,
};

#define NEO_TYPE_DIALOG (neo_dialog_get_type ())
G_DECLARE_FINAL_TYPE (NeoDialog, neo_dialog, NEO, DIALOG, GtkWidget)

NeoDialog *neo_dialog_new          (const char *title);
void       neo_dialog_set_message  (NeoDialog *self, const char *message);
void       neo_dialog_set_content  (NeoDialog *self, GtkWidget *content);
/* buttons are added from the start: Android puts the negative one before the positive one */
GtkWidget *neo_dialog_add_button   (NeoDialog *self, const char *label, int response);
/* the card's own padding off (the time picker draws its header to the edges) */
void       neo_dialog_set_flush    (NeoDialog *self, gboolean flush);
/* shows it over the window `parent` is in */
void       neo_dialog_present      (NeoDialog *self, GtkWidget *parent);
/* answers with `response` and closes, as a button would */
void       neo_dialog_respond      (NeoDialog *self, int response);

/* Signals: "response" (int) */

G_END_DECLS
