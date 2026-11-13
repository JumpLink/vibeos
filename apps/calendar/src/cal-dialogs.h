/* The calendar's small dialogs, on libneo's NeoDialog (a dialog inside the window). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* @index the choice, or −1 when cancelled (Cancel, Back, a tap outside) */
typedef void (*CalChoiceDone) (int index, gpointer data);

/* a title over a list, a tap on a row answers (the editor's "which events?") */
void cal_choice_dialog (GtkWidget *parent, const char *title, const char *const *labels, int n_labels,
                        CalChoiceDone done, gpointer data);
/* radio rows with Cancel and OK, OK once one is chosen (deleting a recurring event) */
void cal_radio_dialog  (GtkWidget *parent, const char *title, const char *const *labels, int n_labels,
                        CalChoiceDone done, gpointer data);
/* a list row's dialog: radio rows with @selected checked, a tap on one answers, Cancel */
void cal_list_dialog   (GtkWidget *parent, const char *title, const char *const *labels, int n_labels, int selected,
                        CalChoiceDone done, gpointer data);
/* a message with Cancel and @ok; @done gets 0 for @ok, −1 else */
void cal_confirm_dialog (GtkWidget *parent, const char *message, const char *ok, CalChoiceDone done, gpointer data);

/* a text row's dialog: a title over a text field holding @text, Cancel and OK; @done gets the text on OK, NULL
 * on cancel */
typedef void (*CalTextDone) (const char *text, gpointer data);
void cal_text_dialog (GtkWidget *parent, const char *title, const char *text, CalTextDone done, gpointer data);

G_END_DECLS
