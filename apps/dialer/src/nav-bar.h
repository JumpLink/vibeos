/* The Dialer's bottom navigation.
 *
 * Not libneo's NeoNavBar: the Dialer lays its own items out, in three equal
 * columns. The selected item has a pill behind its icon, which widens from 40 % as it fades in over 400 ms while
 * the old one narrows and fades out; its icon swaps to the filled one at once. An item can carry a badge with a
 * count over the pill's top-right corner: the Recents tab's new missed calls. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define DIALER_TYPE_NAV_BAR (dialer_nav_bar_get_type ())
G_DECLARE_FINAL_TYPE (DialerNavBar, dialer_nav_bar, DIALER, NAV_BAR, GtkWidget)

GtkWidget *dialer_nav_bar_new          (void);
/* @icon is the outline icon, @selected_icon the filled one shown while the item is selected */
void       dialer_nav_bar_append       (DialerNavBar *self, const char *icon, const char *selected_icon,
                                        const char *label);
/* selects without emitting "selected" (restoring the saved tab, opening Recents from a notification) */
void       dialer_nav_bar_set_selected (DialerNavBar *self, int index);
int        dialer_nav_bar_get_selected (DialerNavBar *self);
/* 0 hides the badge; over 9 it reads "9+" */
void       dialer_nav_bar_set_count    (DialerNavBar *self, int index, guint count);

/* Signals: "selected" (int index) on a tap on another item */

G_END_DECLS
