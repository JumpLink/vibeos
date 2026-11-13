/* ClockNavigator: the Clock's pages (the tabs, the cities picker, Settings) in one window, with transitions between
 * them (a copy of Messaging's MsgNavigator): a page opened comes in from 96 px to the right while fading in, over the
 * one it covers, which moves 96 px left; going back reverses it. The page on top is the one shown and the one with the
 * focus. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CLOCK_TYPE_NAVIGATOR (clock_navigator_get_type ())
G_DECLARE_FINAL_TYPE (ClockNavigator, clock_navigator, CLOCK, NAVIGATOR, GtkWidget)

GtkWidget *clock_navigator_new (void);
/* opens @page over the others */
void       clock_navigator_push (ClockNavigator *self, GtkWidget *page);
/* closes the top page, back to the one under it; the first page stays. FALSE when there was none */
gboolean   clock_navigator_pop (ClockNavigator *self);
/* swaps the top page for @page with the open transition: the contact picker becomes the conversation it picked */
void       clock_navigator_replace (ClockNavigator *self, GtkWidget *page);
/* closes every page down to the first */
void       clock_navigator_pop_to_root (ClockNavigator *self);
GtkWidget *clock_navigator_get_top (ClockNavigator *self);
guint      clock_navigator_get_depth (ClockNavigator *self);

/* Signals: "top-changed" (): a page came to the top (its transition may still run) */

G_END_DECLS
