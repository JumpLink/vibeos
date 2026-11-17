/* spongy-gtk: libspongy's list scrolling as a GTK 4 widget. A SpongyScrollView scrolls its one child with
 * spongy.h's SpongyList: a touch drag past the slop, the spline fling, the stretch at both ends. It takes the place of
 * a GtkScrolledWindow with no scroll bars that scrolls one way: vertically (the default), or sideways (a strip of
 * tiles, a pager) with GtkOrientable's orientation set to horizontal. A GtkScrollable child (a GtkListView) is
 * scrolled through its adjustment along that axis, anything else is laid out at its full height (or, sideways, its
 * minimum width, as GtkScrolledWindow does).
 *
 * A sideways view in a vertical one (or the other way round): the axis the finger crosses the slop along first takes
 * the drag, and the other view does not move at all.
 *
 * Distances (the slop, fling speeds) are in dp: one logical pixel, whatever scale the screen is set to (its display
 * size), so they follow a new scale by themselves. SPONGY_DENSITY in the environment overrides that with a dp in
 * screen pixels (3: three screen pixels whatever the scale). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define SPONGY_TYPE_SCROLL_VIEW (spongy_scroll_view_get_type ())
G_DECLARE_FINAL_TYPE (SpongyScrollView, spongy_scroll_view, SPONGY, SCROLL_VIEW, GtkWidget)

GtkWidget *spongy_scroll_view_new        (void);
void       spongy_scroll_view_set_child  (SpongyScrollView *self, GtkWidget *child);
GtkWidget *spongy_scroll_view_get_child  (SpongyScrollView *self);
/* how far it is scrolled, logical px from the top (from the left, sideways) */
double     spongy_scroll_view_get_offset (SpongyScrollView *self);
/* jumps there, stopping a fling */
void       spongy_scroll_view_scroll_to  (SpongyScrollView *self, double offset);
/* the vertical adjustment GtkScrolledWindow would have: a GtkScrollable child's, or one that follows the offset
 * (value), the child's height (upper) and the view's (page size). Setting its value scrolls there, as scroll_to.
 * Code written for GtkScrolledWindow (stay at the end, a collapsing header) can keep using it. NULL for a
 * horizontal view. */
GtkAdjustment *spongy_scroll_view_get_vadjustment (SpongyScrollView *self);
/* the same for a horizontal view, along the width; NULL for a vertical one */
GtkAdjustment *spongy_scroll_view_get_hadjustment (SpongyScrollView *self);

/* Where a fling or a let-go drag comes to rest: anywhere (interval 0, the default); the multiple of `interval` px
 * nearest to where the fling would have stopped (a grid's columns); or, with `pages`, at most the next multiple in
 * the fling's direction, and the nearest one when it was let go without a fling (a pager: an interval of 0 is then
 * the view's own width or height, and a pager kept on its page when the view's size changes). The end is a snap
 * point too. A plain child only (not a GtkScrollable one). */
void       spongy_scroll_view_set_snap   (SpongyScrollView *self, double interval, gboolean pages);

/* Signals: "edge-overshot" (GtkPositionType), as GtkScrolledWindow's: a touch pulled the top or bottom edge (left
 * or right, sideways) into a stretch, or a wheel scrolled past it. Once per touch. */

G_END_DECLS
