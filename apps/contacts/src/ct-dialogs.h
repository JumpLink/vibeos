/* The Contacts dialogs (spec §5.0.4), on libneo's NeoDialog (the scrim, the card, Back and a tap outside closing
 * it), in its "flush" form so that the card is as wide as the content: a framework AlertDialog is 320 dp wide, an
 * AppCompat one 95 % of the screen (342 dp). The card is #1b1b1b with 16 dp corners (Contacts' dialog_background).
 *
 * The layout is alert_dialog_material's: the title 20 sp medium #e1e1e1 18 dp from the top and 24 dp in, then the
 * message (16 sp) or the list, then the button bar (12/4/12/4 dp padding, 54 dp buttons: 14 sp medium caps #81b5bc,
 * negative before positive, at the end). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* the chosen row or button: a row's index, CT_DIALOG_OK for the positive button, −1 for Cancel, Back or outside */
#define CT_DIALOG_OK 1000
typedef void (*CtDialogDone) (int response, gpointer data);

typedef enum {
  CT_DIALOG_FRAMEWORK,          /* 320 dp (config_prefDialogWidth) */
  CT_DIALOG_APPCOMPAT,          /* 95 % of the screen's width (abc_dialog_min_width_minor) */
} CtDialogWidth;

/* a message with Cancel and @ok (no title: "Delete this contact?", "Discard changes?") */
void ct_confirm_dialog (GtkWidget *parent, const char *title, const char *message, const char *ok,
                        CtDialogDone done, gpointer data);
/* a title over rows (select_dialog_item, 56 dp, 16 sp) and a Cancel button: a tap on a row answers with its index */
void ct_list_dialog    (GtkWidget *parent, CtDialogWidth width, const char *title, const char *const *labels,
                        int n_labels, gboolean cancel, CtDialogDone done, gpointer data);
/* ListPreference's dialog: radio rows (18 sp, the radio 20 dp in), @selected checked, no buttons: a tap commits */
void ct_radio_dialog   (GtkWidget *parent, const char *title, const char *const *labels, int n_labels, int selected,
                        CtDialogDone done, gpointer data);

/* a one-line text field (the label name, the custom type label): @check returns an error to show under the field
 * (and keeps OK disabled) or NULL; OK is disabled while the trimmed text is empty */
typedef const char *(*CtTextCheck) (const char *text, gpointer data);
typedef void (*CtTextDone) (const char *text, gpointer data);       /* NULL when cancelled */
void ct_text_dialog    (GtkWidget *parent, const char *title, const char *text, const char *hint, int max_length,
                        CtTextCheck check, CtTextDone done, gpointer data);

/* DatePickerDialog (§4.16): the month, the day and the year on NumberPicker wheels in a 270 dp column, "Include
 * year" under them when @year_optional (a birthday), Cancel and Set. The title is the date in full and follows the
 * wheels. @year 0 opens without a year. @done gets the choice on Set (year 0 for none), and nothing on Cancel. */
typedef void (*CtDateDone) (int year, int month, int day, gpointer data);
void ct_date_dialog    (GtkWidget *parent, int year, int month, int day, gboolean year_optional, CtDateDone done,
                        gpointer data);

G_END_DECLS
