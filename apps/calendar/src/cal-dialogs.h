/* The calendar's small dialogs, on libneo's NeoDialog (an AlertDialog inside the window). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* @index the choice, or −1 when cancelled (Cancel, Back, a tap outside) */
typedef void (*CalChoiceDone) (int index, gpointer data);

/* AlertDialog.setItems: a title over a list, a tap on a row answers (EditEventFragment's "which events?") */
void cal_choice_dialog (GtkWidget *parent, const char *title, const char *const *labels, int n_labels,
                        CalChoiceDone done, gpointer data);
/* AlertDialog.setSingleChoiceItems with Cancel and OK: radio rows, OK once one is chosen (DeleteEventHelper's
 * recurring delete) */
void cal_radio_dialog  (GtkWidget *parent, const char *title, const char *const *labels, int n_labels,
                        CalChoiceDone done, gpointer data);
/* ListPreference's dialog: radio rows with @selected checked, a tap on one answers, Cancel */
void cal_list_dialog   (GtkWidget *parent, const char *title, const char *const *labels, int n_labels, int selected,
                        CalChoiceDone done, gpointer data);
/* a message with Cancel and @ok; @done gets 0 for @ok, −1 else */
void cal_confirm_dialog (GtkWidget *parent, const char *message, const char *ok, CalChoiceDone done, gpointer data);

G_END_DECLS
