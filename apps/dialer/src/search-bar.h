/* The search bar at the top of the Phone app (dialer/main/impl/toolbar: SearchBarView, toolbar_layout.xml,
 * expanded_search_bar.xml; spec §2.2, §4.1).
 *
 * Collapsed it is a rounded bar inset 8 dp: the magnifier, "Search contacts", the voice search button and the
 * overflow ⋮. A tap on it opens the search; it then grows to the full 64 dp row over 200 ms, square, with a back
 * arrow, the search field and a ✕ that clears it. The window decides what opening and closing mean; the bar only
 * draws them and reports the taps and the text. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define DIALER_TYPE_SEARCH_BAR (dialer_search_bar_get_type ())
G_DECLARE_FINAL_TYPE (DialerSearchBar, dialer_search_bar, DIALER, SEARCH_BAR, GtkWidget)

GtkWidget  *dialer_search_bar_new          (void);
/* expand: @focus FALSE when the dialpad opens it (the digits are the query then, and a keyboard must not type
 * into the field) */
void        dialer_search_bar_expand       (DialerSearchBar *self, gboolean focus);
/* collapse, clearing the query */
void        dialer_search_bar_collapse     (DialerSearchBar *self);
gboolean    dialer_search_bar_get_expanded (DialerSearchBar *self);
const char *dialer_search_bar_get_query    (DialerSearchBar *self);
/* sets the field without a "query-changed" (setQueryWithoutUpdate: the dialpad's digits moving over) */
void        dialer_search_bar_set_query    (DialerSearchBar *self, const char *query);
void        dialer_search_bar_hide_keyboard (DialerSearchBar *self);
/* the ⋮ button, which the overflow menu hangs from */
GtkWidget  *dialer_search_bar_get_menu_button (DialerSearchBar *self);

/* Signals: "search-clicked" (), "back-clicked" (), "query-changed" (), "voice-clicked" (), "menu-clicked" () */

G_END_DECLS
