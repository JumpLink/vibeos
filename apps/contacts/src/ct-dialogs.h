/* The Contacts dialogs, on libvibeos's VibeosDialog (the scrim, the card, Back and a tap outside closing
 * it), in its "flush" form so that the card is as wide as the content: CT_DIALOG_FRAMEWORK has a fixed width,
 * CT_DIALOG_APPCOMPAT nearly the screen's.
 *
 * The layout: the title, then the message or the list, then the button bar (capital
 * labels, negative before positive, at the end). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* the chosen row or button: a row's index, CT_DIALOG_OK for the positive button, −1 for Cancel, Back or outside */
#define CT_DIALOG_OK 1000
typedef void (*CtDialogDone) (int response, gpointer data);

typedef enum {
  CT_DIALOG_FRAMEWORK,          /* a fixed width */
  CT_DIALOG_APPCOMPAT,          /* nearly the screen's width */
} CtDialogWidth;

/* a message with Cancel and @ok (no title: "Delete this contact?", "Discard changes?") */
void ct_confirm_dialog (GtkWidget *parent, const char *title, const char *message, const char *ok,
                        CtDialogDone done, gpointer data);
/* a title over rows and a Cancel button: a tap on a row answers with its index */
void ct_list_dialog    (GtkWidget *parent, CtDialogWidth width, const char *title, const char *const *labels,
                        int n_labels, gboolean cancel, CtDialogDone done, gpointer data);
/* radio rows, @selected checked, no buttons: a tap commits */
void ct_radio_dialog   (GtkWidget *parent, const char *title, const char *const *labels, int n_labels, int selected,
                        CtDialogDone done, gpointer data);

/* a one-line text field (the label name, the custom type label): @check returns an error to show under the field
 * (and keeps OK disabled) or NULL; OK is disabled while the trimmed text is empty */
typedef const char *(*CtTextCheck) (const char *text, gpointer data);
typedef void (*CtTextDone) (const char *text, gpointer data);       /* NULL when cancelled */
void ct_text_dialog    (GtkWidget *parent, const char *title, const char *text, const char *hint, int max_length,
                        CtTextCheck check, CtTextDone done, gpointer data);

/* a date: the month, the day and the year on wheels side by side, "Include
 * year" under them when @year_optional (a birthday), Cancel and Set. The title is the date in full and follows the
 * wheels. @year 0 opens without a year. @done gets the choice on Set (year 0 for none), and nothing on Cancel. */
typedef void (*CtDateDone) (int year, int month, int day, gpointer data);
void ct_date_dialog    (GtkWidget *parent, int year, int month, int day, gboolean year_optional, CtDateDone done,
                        gpointer data);

G_END_DECLS
