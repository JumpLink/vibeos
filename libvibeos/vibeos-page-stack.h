/* VibeosPageStack: an app's pages, one over the other, with the transitions between them.
 *
 * The page on top is the one shown, the one that takes input and the one with the focus. A push puts a page on top,
 * Back (vibeos_page_stack_pop) takes it off again; the first page stays. The transition is the stack's:
 *
 *   shared axis      the new page comes in from the right, a tenth of the width away, fading in, while the old one
 *                    moves the same way out and fades: 250 ms. Back reverses it in 170 ms.
 *   in a pane        the same inside one pane of a two-pane layout, over a fixed short distance: 250 ms in, and a
 *                    quick 90 ms back.
 *   fade through     the old page fades out (60 ms), then the new one fades in while growing from 92 % (240 ms).
 *   none             a cut.
 *
 * A row that opens a page can keep its press highlight on for a moment first (vibeos_page_stack_push_from), as a
 * list does while the page it opens gets ready.
 *
 * With G_MESSAGES_DEBUG=vibeos-page-stack every frame of a transition is logged with its frame-clock time and where
 * the incoming page is, for measuring the timings on a real display. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef enum {
  VIBEOS_PAGE_TRANSITION_SHARED_AXIS,
  VIBEOS_PAGE_TRANSITION_SHARED_AXIS_PANE,
  VIBEOS_PAGE_TRANSITION_FADE_THROUGH,
  VIBEOS_PAGE_TRANSITION_NONE,
} VibeosPageTransition;

#define VIBEOS_TYPE_PAGE_STACK (vibeos_page_stack_get_type ())
G_DECLARE_FINAL_TYPE (VibeosPageStack, vibeos_page_stack, VIBEOS, PAGE_STACK, GtkWidget)

GtkWidget *vibeos_page_stack_new            (void);
/* VIBEOS_PAGE_TRANSITION_SHARED_AXIS by default */
void       vibeos_page_stack_set_transition (VibeosPageStack *self, VibeosPageTransition transition);
VibeosPageTransition vibeos_page_stack_get_transition (VibeosPageStack *self);

/* opens `page` over the others */
void       vibeos_page_stack_push           (VibeosPageStack *self, GtkWidget *page);
/* the same, after showing `pressed` (the row tapped) highlighted for the stack's highlight time; the highlight goes
 * when the transition ends */
void       vibeos_page_stack_push_from      (VibeosPageStack *self, GtkWidget *page, GtkWidget *pressed);
/* 300 ms by default; 0 pushes at once */
void       vibeos_page_stack_set_highlight_time (VibeosPageStack *self, guint ms);
/* closes the top page, back to the one under it; the first page stays. FALSE when there was none to close */
gboolean   vibeos_page_stack_pop            (VibeosPageStack *self);
/* swaps the top page for `page`, with the push transition (a picker that becomes what it picked) */
void       vibeos_page_stack_replace        (VibeosPageStack *self, GtkWidget *page);
/* makes `page` the only one, with the push transition: another item chosen in a list replaces the whole stack */
void       vibeos_page_stack_set_root       (VibeosPageStack *self, GtkWidget *page);
/* closes every page down to the first */
void       vibeos_page_stack_pop_to_root    (VibeosPageStack *self);
/* takes every page off, the first too, at once */
void       vibeos_page_stack_clear          (VibeosPageStack *self);
/* moves the pages from `index` up from `from` onto the top of `to`, in order, at once (no transition): what a
 * two-pane layout does when it becomes one pane, or one pane two */
void       vibeos_page_stack_move_pages     (VibeosPageStack *from, guint index, VibeosPageStack *to);

GtkWidget *vibeos_page_stack_get_top        (VibeosPageStack *self);
GtkWidget *vibeos_page_stack_get_page       (VibeosPageStack *self, guint index);
guint      vibeos_page_stack_get_depth      (VibeosPageStack *self);
gboolean   vibeos_page_stack_is_animating   (VibeosPageStack *self);

/* Signals: "top-changed" (): another page came to the top (its transition may still run), or the stack emptied */

G_END_DECLS
