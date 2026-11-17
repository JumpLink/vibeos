/* VibeosSheet: a bottom sheet inside the app's own window, as libadwaita shows its dialogs on a phone (AdwDialog as a
 * bottom sheet), made of libvibeos's own parts.
 *
 * The sheet rises from the window's bottom over a dim, its top corners rounded. It is as tall as its first page needs,
 * up to a gap under the window's top (30 px on a phone, more on a taller window, as libadwaita leaves), or always that
 * tall (vibeos_sheet_set_full_height). On a wide window it keeps to 640 px, centred. It moves on libadwaita's spring
 * for bottom sheets, damping ratio 0.8 and stiffness 400: it is nearly in place in 200 ms, rises past it by 1.5 % and
 * settles in about 450 ms; it goes down the same way, without going past the bottom.
 *
 * Each page has a bar on top (a compact VibeosTopBar): the first page a title or none, a page pushed over it a back
 * arrow and its title. A close button, an X in a tonal circle, ends every bar. A pushed page slides in from the right
 * over the page under it, which dims as it is covered, and slides back out on back: libadwaita's navigation spring,
 * critically damped with stiffness 1000.
 *
 * It closes on its close button, a tap on the dim, Escape, Alt+Left (what VibeOS's back gesture sends) and the Back
 * key; on a pushed page those keys and the back arrow go back a page instead. Closing, it emits "closed", slides down,
 * then leaves the window and is destroyed; the focus goes back where it was.
 *
 *   GtkWidget *sheet = vibeos_sheet_new ("Set City");
 *   vibeos_sheet_set_content (VIBEOS_SHEET (sheet), content);
 *   vibeos_sheet_present (VIBEOS_SHEET (sheet), button);
 *
 * It goes into the root overlay of the window `parent` is in (vibeos_window_get_overlay): a window made by
 * vibeos_window_new, or any window that keeps its root GtkOverlay as its object data "vibeos-overlay". Its text is in
 * libvibeos's font and colours whatever the window's are (call vibeos_init first). Content that can be taller than the
 * sheet scrolls itself: put it in a SpongyScrollView. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define VIBEOS_TYPE_SHEET (vibeos_sheet_get_type ())
G_DECLARE_FINAL_TYPE (VibeosSheet, vibeos_sheet, VIBEOS, SHEET, GtkWidget)

/* a sheet whose first page's bar has `title` (NULL: none, only the close button) */
GtkWidget *vibeos_sheet_new             (const char *title);
/* the first page's content, under its bar; it takes the page's height */
void       vibeos_sheet_set_content     (VibeosSheet *self, GtkWidget *content);
/* as tall as the window lets it be, whatever its content needs: a sheet whose content changes as it is used (a list
 * of results), and one that has to stay put over the on-screen keyboard */
void       vibeos_sheet_set_full_height (VibeosSheet *self, gboolean full_height);
/* a page over the one on top, with a back arrow and `title`, sliding in */
void       vibeos_sheet_push            (VibeosSheet *self, const char *title, GtkWidget *content);
/* the top page slides out, back to the one under it; FALSE when the first page is on top */
gboolean   vibeos_sheet_back            (VibeosSheet *self);
/* rises over the window `parent` is in */
void       vibeos_sheet_present         (VibeosSheet *self, GtkWidget *parent);
/* slides down and goes, as the close button does */
void       vibeos_sheet_close           (VibeosSheet *self);

/* Signals: "closed" (), as it starts to slide down */

G_END_DECLS
