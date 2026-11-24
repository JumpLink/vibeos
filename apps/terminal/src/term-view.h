/* TermView: the terminal, a VteTerminal that takes every touch itself.
 *
 * VTE's GTK 4 widget was made for a mouse: a finger's press is a click to it, but a finger's move is nothing (it
 * reads motion from pointer motion events, which a touchscreen does not send), so a drag selects nothing and nothing
 * scrolls, and its long press is denied by its own click gesture. Here a capture-phase controller has every touch
 * first and stops it; the mouse, the touchpad and the keys still go to VTE as they always did.
 *
 *   tap         the focus (the shell opens the keyboard on a tap in a focused terminal: oskfocus.js), and for a
 *               program that tracks the mouse a click of its first button at that cell. A tap with text selected
 *               only ends the selection.
 *   long press  selects the word under the finger (or the one cell, when it is no word's); moving on extends the
 *               selection from it. Once the finger lifts the toolbar offers Copy, Paste and Select all, and a handle
 *               at each end moves that end.
 *   drag        one finger moved past the slop before the long press, or two fingers: scrolls, and flung goes on
 *               under libspongy's fling. The content follows the finger, less the slop (as Android's scroll views:
 *               no jump at the start); two fingers scroll by their centre, and a second finger on a one-finger
 *               scroll takes it over. What a scroll does is what VTE does for a mouse wheel: the history moves, or a
 *               program on the alternate screen (less, man) gets cursor keys, or one that tracks the mouse (htop, mc)
 *               gets wheel buttons, a step per row of travel. The modes come from TermModes, which reads them from
 *               what the program writes on its way here. A finger that stops a fling neither taps nor long-presses.
 *
 * The selection is the app's own (VTE has no call to set one), drawn over the text and copied with
 * vte_terminal_get_text_range_format (). That takes rows counted from the start of VTE's buffer, which VTE does not
 * say where the view starts in: its scroll adjustment counts from the oldest row it still keeps. So the view finds
 * the offset by asking for the rows it would be at each place the cursor's row allows and keeping the one whose text
 * is what VTE shows (update_offset ()), again whenever the contents change while something is selected. */
#pragma once

#include <vte/vte.h>

#include "term-session.h"

G_BEGIN_DECLS

#define TERM_TYPE_VIEW (term_view_get_type ())
G_DECLARE_FINAL_TYPE (TermView, term_view, TERM, VIEW, VteTerminal)

TermView *term_view_new               (void);
/* the session the view's grid size, clicks and wheel go to */
void      term_view_set_session       (TermView *self, TermSession *session);
/* ends the selection and closes its toolbar; FALSE when nothing was selected */
gboolean  term_view_end_selection     (TermView *self);
/* copies the selection (and ends it), or without one what the mouse selected in VTE */
void      term_view_copy              (TermView *self);
void      term_view_paste             (TermView *self);
/* selects everything VTE keeps, the history and the screen */
void      term_view_select_all        (TermView *self);
/* the y of the foot of the text cursor's row, in the view (below it when the view is scrolled back); FALSE when the
 * view cannot tell where its rows are */
gboolean  term_view_get_cursor_bottom (TermView *self, double *y);
/* the view to the end of the text, as a key typed takes it */
void      term_view_scroll_to_end     (TermView *self);

G_END_DECLS
