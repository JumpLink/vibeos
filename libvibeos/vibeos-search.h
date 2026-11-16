/* VibeosSearchBar and VibeosSearchView: Material 3's search, as a bar at the top of a list and the view it opens.
 *
 * VibeosSearchBar is the pill a list shows over its first items: a magnifier, a hint ("Search settings"), and
 * optionally something at the end (an avatar, a menu). It does no typing itself; a tap on it emits "activated", and
 * the app opens a VibeosSearchView. Its height, the icon's and the hint's insets, and its colour can be set, for the
 * apps whose bar is larger or darker than the usual one.
 *
 * VibeosSearchView covers the window (vibeos_search_view_open puts it in the window's overlay, fading in): a back
 * button, the entry, a clear button once there is text, and under them whatever the app shows the results in. It
 * emits "search-changed" with the text as it is typed and "closed" when the back button or Escape closes it. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define VIBEOS_TYPE_SEARCH_BAR (vibeos_search_bar_get_type ())
G_DECLARE_FINAL_TYPE (VibeosSearchBar, vibeos_search_bar, VIBEOS, SEARCH_BAR, GtkWidget)

GtkWidget *vibeos_search_bar_new          (const char *hint);
void       vibeos_search_bar_set_hint     (VibeosSearchBar *self, const char *hint);
/* the pill's height, its margin from the sides, the magnifier's inset and the hint's, all in px */
void       vibeos_search_bar_set_geometry (VibeosSearchBar *self, int height, int side, int icon_x, int text_x);
/* the pill's colour (NULL: the default container colour) */
void       vibeos_search_bar_set_fill     (VibeosSearchBar *self, const GdkRGBA *fill);
/* a widget at the pill's end, or NULL */
void       vibeos_search_bar_set_end      (VibeosSearchBar *self, GtkWidget *end);

/* Signals: "activated" () */

#define VIBEOS_TYPE_SEARCH_VIEW (vibeos_search_view_get_type ())
G_DECLARE_FINAL_TYPE (VibeosSearchView, vibeos_search_view, VIBEOS, SEARCH_VIEW, GtkWidget)

GtkWidget  *vibeos_search_view_new         (const char *hint);
/* what shows under the entry: the results, the recent searches */
void        vibeos_search_view_set_content (VibeosSearchView *self, GtkWidget *content);
const char *vibeos_search_view_get_text    (VibeosSearchView *self);
void        vibeos_search_view_set_text    (VibeosSearchView *self, const char *text);
/* over everything in `widget`'s window, fading in, the entry focused */
void        vibeos_search_view_open        (VibeosSearchView *self, GtkWidget *widget);
/* fades out and leaves the window; emits "closed" */
void        vibeos_search_view_close       (VibeosSearchView *self);

/* Signals: "search-changed" (const char *text), "closed" () */

G_END_DECLS
