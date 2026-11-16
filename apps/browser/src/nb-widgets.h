/* nb-widgets: the parts of Jelly's screens that libvibeos does not have, drawn as Jelly draws them:
 *
 *   NbDialog     a rounded card over a dim, nearly as wide as the window, the neutral button at the start
 *   list dialog  a single-choice dialog on NbDialog
 *   NbSheet      a bottom sheet: the content slides up from the bottom over a dim
 *   toast        a short notice over the window, as Calculator's
 *   NbSwitch     the settings screen's switch
 *   NbMark       a checkbox or a radio button, drawn (no input of its own: its row toggles it)
 *   toolbar      the up arrow (named "Back" for the shell's back gesture), the title, actions
 *   field        the outlined text field around a GtkEntry
 *   indicator    a scrollbar: a thin thumb at the end that shows while scrolling and when the view appears
 *
 * Every one goes over the window made by vibeos_window_new (its root overlay), as libvibeos's dialog and snackbar do. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

enum {
  NB_RESPONSE_CANCEL = -1,
  NB_RESPONSE_OK = 1,
  NB_RESPONSE_NEUTRAL = 2,
  NB_RESPONSE_OTHER = 3,
};

#define NB_TYPE_DIALOG (nb_dialog_get_type ())
G_DECLARE_FINAL_TYPE (NbDialog, nb_dialog, NB, DIALOG, GtkWidget)

NbDialog  *nb_dialog_new          (const char *title);
void       nb_dialog_set_message  (NbDialog *self, const char *message);
void       nb_dialog_set_content  (NbDialog *self, GtkWidget *content);
/* buttons in this order: negative, then positive, at the end; NB_RESPONSE_NEUTRAL at the start */
GtkWidget *nb_dialog_add_button   (NbDialog *self, const char *label, int response);
/* a button with this response emits "response" but leaves the dialog open (the handler closes it with
 * nb_dialog_close): Edit favorite with an invalid URL. NB_RESPONSE_CANCEL sticky: not cancelable (a scrim tap,
 * Escape and back do nothing but emit it) */
void       nb_dialog_set_sticky   (NbDialog *self, int response);
void       nb_dialog_present      (NbDialog *self, GtkWidget *parent);
/* answers `response` and closes, as a button would */
void       nb_dialog_respond      (NbDialog *self, int response);
void       nb_dialog_close        (NbDialog *self);
/* Signals: "response" (int) — a scrim tap, Escape and back answer NB_RESPONSE_CANCEL */

typedef void (*NbListPicked) (int index, gpointer data);
/* a single-choice dialog: one radio row per item, `selected` checked, Cancel. Picking a row closes it and calls
 * `picked` (not on cancel) */
NbDialog  *nb_list_dialog_present (GtkWidget *parent, const char *title, const char *const *items, int selected,
                                   NbListPicked picked, gpointer data);

#define NB_TYPE_SHEET (nb_sheet_get_type ())
G_DECLARE_FINAL_TYPE (NbSheet, nb_sheet, NB, SHEET, GtkWidget)

/* `dim`: the scrim's black alpha at rest (light for a bottom sheet, darker for the add-to-home sheet) */
NbSheet   *nb_sheet_new           (GtkWidget *content, double dim);
void       nb_sheet_present       (NbSheet *self, GtkWidget *parent);
void       nb_sheet_dismiss       (NbSheet *self);
/* Signals: "closed" () once it has gone */

/* a sheet row: the icon, then the text */
GtkWidget *nb_sheet_row           (const char *icon_name, GdkPaintable *icon, const char *text);

/* a toast over the window `widget` is in */
void       nb_toast_show          (GtkWidget *widget, const char *text);

#define NB_TYPE_SWITCH (nb_switch_get_type ())
G_DECLARE_FINAL_TYPE (NbSwitch, nb_switch, NB, SWITCH, GtkWidget)
GtkWidget *nb_switch_new          (void);
gboolean   nb_switch_get_active   (NbSwitch *self);
void       nb_switch_set_active   (NbSwitch *self, gboolean active);
/* Properties: "active" */

/* The number of open tabs in its square (CSS node nb-tab-count: the square is its border, the number its font) */
#define NB_TYPE_TAB_COUNT (nb_tab_count_get_type ())
G_DECLARE_FINAL_TYPE (NbTabCount, nb_tab_count, NB, TAB_COUNT, GtkWidget)
GtkWidget *nb_tab_count_new       (void);
void       nb_tab_count_set_text  (NbTabCount *self, const char *text);

typedef enum { NB_MARK_CHECK, NB_MARK_RADIO } NbMarkKind;
#define NB_TYPE_MARK (nb_mark_get_type ())
G_DECLARE_FINAL_TYPE (NbMark, nb_mark, NB, MARK, GtkWidget)
GtkWidget *nb_mark_new            (NbMarkKind kind);
gboolean   nb_mark_get_active     (NbMark *self);
void       nb_mark_set_active     (NbMark *self, gboolean active);

/* A toolbar: the up arrow (accessible name "Back") calling `up`, the title (at `title_size`, or the default size),
 * and `actions` (or NULL) at the end. */
GtkWidget *nb_toolbar_new         (const char *title, double title_size, GCallback up, gpointer data,
                                   GtkWidget *actions);
GtkWidget *nb_toolbar_get_title   (GtkWidget *toolbar);
/* an icon button for a toolbar's actions, `color` a CSS class for its tint */
GtkWidget *nb_action_button       (const char *icon_name, const char *tooltip);

/* The outlined text field: the floating label over the outline, primary while focused; `error` under it */
GtkWidget *nb_field_new           (const char *label, gboolean password);
GtkEntry  *nb_field_get_entry     (GtkWidget *field);
void       nb_field_set_error     (GtkWidget *field, const char *error);

/* A scrollbar for `adjustment`, to be laid over the scrolling view's end edge (an overlay child, halign END) */
GtkWidget *nb_indicator_new       (GtkAdjustment *adjustment);
/* shows it as when the view appears: for 1.2 s, then a 250 ms fade */
void       nb_indicator_awaken    (GtkWidget *indicator);

/* A symbolic icon from the app's resources at `size` px */
GtkWidget *nb_icon                (const char *icon_name, int size);
/* A label: `size` px with the font padding above and below, `css` its colour/weight class or
 * NULL, ellipsized at the end when `ellipsize` */
GtkWidget *nb_label               (const char *text, double size, const char *css, gboolean ellipsize);

G_END_DECLS
