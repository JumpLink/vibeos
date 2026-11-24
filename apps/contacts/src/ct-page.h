/* CtPage: one of the Contacts screens (the list, QuickContact, the editor, Settings…),
 * pushed onto the window's pages (ct-window.h).
 *
 * Back on a page asks its back handler first (the editor's "Discard changes?", the list leaving search), and the
 * page goes if the handler did not deal with it. The shell's back gesture finds a toolbar button named "Back" and
 * presses it, which does the same; a page without one (QuickContact) has the dialog role, and the gesture sends
 * Escape to it, which comes here too.
 *
 * Signals: "resized" (int width, int height), when the page gets a new size, before its children are placed. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CT_TYPE_PAGE (ct_page_get_type ())
G_DECLARE_FINAL_TYPE (CtPage, ct_page, CT, PAGE, GtkBox)

/* TRUE when the handler dealt with Back itself */
typedef gboolean (*CtPageBack) (CtPage *page, gpointer data);

GtkWidget *ct_page_new               (GtkWidget *window);
GtkWidget *ct_page_get_window        (CtPage *self);
void       ct_page_set_back_handler  (CtPage *self, CtPageBack back, gpointer data);
/* Back, as Escape and the toolbar's arrow do: the handler, else the page goes */
void       ct_page_back              (CtPage *self);
/* the back handler alone: TRUE when it dealt with Back itself (an editor asking "Discard changes?"), the page
 * staying; FALSE when the page may go */
gboolean   ct_page_back_handled      (CtPage *self);

/* A page drawn over the one under it instead of in its place (QuickContact): no slide when it comes or goes; it
 * animates itself */
void       ct_page_set_translucent   (CtPage *self, gboolean translucent);
gboolean   ct_page_get_translucent   (CtPage *self);

/* The toolbar of a page that has one (Settings, "Contacts to display", the picker): in the CSS class's colour, the
 * navigation button (a borderless ripple; @nav_name "Back" for the Up arrow), the title, and a box at the end for
 * the actions, which it returns in @actions. */
GtkWidget *ct_page_toolbar           (CtPage *self, const char *css_class, const char *nav_icon, const char *nav_name,
                                      const char *title, GtkWidget **actions, GtkWidget **title_label);

G_END_DECLS
