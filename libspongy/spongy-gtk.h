/* spongy-gtk: libspongy's list scrolling as a GTK 4 widget. A SpongyScrollView scrolls its one child vertically the
 * way an Android RecyclerView does (spongy.h's SpongyList): a touch drag past the slop, the spline fling, the stretch
 * at both ends. It takes the place of a GtkScrolledWindow with a vertical policy and no scroll bars; a GtkScrollable
 * child (a GtkListView) is scrolled through its vadjustment, anything else is laid out at its full height.
 *
 * Distances follow Android's dp: SPONGY_DENSITY in the environment is the screen's pixels per dp (3 on a 1080 px
 * wide phone made for 420-480 dpi); without it the monitor's size gives it. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define SPONGY_TYPE_SCROLL_VIEW (spongy_scroll_view_get_type ())
G_DECLARE_FINAL_TYPE (SpongyScrollView, spongy_scroll_view, SPONGY, SCROLL_VIEW, GtkWidget)

GtkWidget *spongy_scroll_view_new        (void);
void       spongy_scroll_view_set_child  (SpongyScrollView *self, GtkWidget *child);
GtkWidget *spongy_scroll_view_get_child  (SpongyScrollView *self);
/* how far it is scrolled, logical px from the top */
double     spongy_scroll_view_get_offset (SpongyScrollView *self);
/* jumps there, stopping a fling */
void       spongy_scroll_view_scroll_to  (SpongyScrollView *self, double offset);
/* the vertical adjustment GtkScrolledWindow would have: a GtkScrollable child's, or one that follows the offset
 * (value), the child's height (upper) and the view's (page size). Setting its value scrolls there, as scroll_to.
 * Code written for GtkScrolledWindow (stay at the end, a collapsing header) can keep using it. */
GtkAdjustment *spongy_scroll_view_get_vadjustment (SpongyScrollView *self);

/* Signals: "edge-overshot" (GtkPositionType), as GtkScrolledWindow's: a touch pulled the top or bottom edge into a
 * stretch, or a wheel scrolled past it. Once per touch. */

G_END_DECLS
