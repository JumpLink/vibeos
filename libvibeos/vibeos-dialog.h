/* VibeosDialog: an alert dialog, drawn inside the app's own window (vibeos_window_new) over a black scrim.
 *
 * A rounded card: an optional title, a message or any content, and text buttons at the bottom end. It comes in
 * fading in while rising 20 px (220 ms, the standard curve) and leaves the same way (fading out while rising 10 px,
 * 150 ms, standard-accelerate).
 *
 *   VibeosDialog *d = vibeos_dialog_new ("Label");
 *   vibeos_dialog_set_content (d, entry);
 *   vibeos_dialog_add_button (d, "Cancel", VIBEOS_RESPONSE_CANCEL);
 *   vibeos_dialog_add_button (d, "OK", VIBEOS_RESPONSE_OK);
 *   g_signal_connect (d, "response", G_CALLBACK (on_label_response), self);
 *   vibeos_dialog_present (d, GTK_WIDGET (self));
 *
 * "response" fires once with the button's response (a tap on the scrim, Escape or Back give VIBEOS_RESPONSE_CANCEL),
 * then the dialog animates out and destroys itself. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

enum {
  VIBEOS_RESPONSE_CANCEL = -1,
  VIBEOS_RESPONSE_OK = 1,
};

#define VIBEOS_TYPE_DIALOG (vibeos_dialog_get_type ())
G_DECLARE_FINAL_TYPE (VibeosDialog, vibeos_dialog, VIBEOS, DIALOG, GtkWidget)

VibeosDialog *vibeos_dialog_new          (const char *title);
void       vibeos_dialog_set_message  (VibeosDialog *self, const char *message);
void       vibeos_dialog_set_content  (VibeosDialog *self, GtkWidget *content);
/* buttons are added from the start: Android puts the negative one before the positive one */
GtkWidget *vibeos_dialog_add_button   (VibeosDialog *self, const char *label, int response);
/* a button at the row's start, apart from the others: Android's neutral button (Batch Rename's MORE). It does not
 * answer or close the dialog; connect to its "clicked". Hiding it leaves the others where they are */
GtkWidget *vibeos_dialog_add_neutral_button (VibeosDialog *self, const char *label);
/* the card's own padding off (the time picker draws its header to the edges) */
void       vibeos_dialog_set_flush    (VibeosDialog *self, gboolean flush);
/* shows it over the window `parent` is in */
void       vibeos_dialog_present      (VibeosDialog *self, GtkWidget *parent);
/* centres the card on `pane` (the detail pane of a two-pane layout) rather than on the window, and keeps it within
 * the pane's width; the scrim still covers the whole window. NULL: the window again */
void       vibeos_dialog_set_anchor   (VibeosDialog *self, GtkWidget *pane);
/* answers with `response` and closes, as a button would */
void       vibeos_dialog_respond      (VibeosDialog *self, int response);

/* Signals: "response" (int) */

G_END_DECLS
