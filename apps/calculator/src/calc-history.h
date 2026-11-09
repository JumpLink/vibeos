/* CalcHistory: the history, ExactCalculator's HistoryFragment (fragment_history.xml, history_item.xml).
 *
 * A toolbar (the back arrow, "History", a ⋮ menu with "Clear") over a list that starts at its end: the newest
 * calculation at the bottom, just above the display, older ones above it (reverseLayout). Each row shows its date
 * when it is the first of its day (Today, Yesterday, 3 days ago, Nov 2), with a divider over it unless it is the
 * oldest; then the expression and its result, which is computed again as the row comes into view (and cancelled
 * when it leaves), and scrolls like the display's. With no history: the history icon over "No History".
 *
 * Only the rows on screen exist, as RecyclerView's.
 *
 * Signals: "back" (), "menu" () (the ⋮: the window opens its menu, Clear, there), "result-long-pressed"
 * (CalcResult *): a row's result wants its copy toolbar. */
#pragma once

#include "calc-result.h"
#include "evaluator.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CALC_TYPE_HISTORY (calc_history_get_type ())
G_DECLARE_FINAL_TYPE (CalcHistory, calc_history, CALC, HISTORY, GtkWidget)

GtkWidget *calc_history_new            (Evaluator *evaluator);
/* its size in dp, fractional: it is allocated the whole logical pixels around it */
void       calc_history_set_exact_size (CalcHistory *self, double width, double height);
/* the ⋮ button, for the menu's anchor */
GtkWidget *calc_history_get_overflow   (CalcHistory *self);

/* Android's DateUtils.getRelativeTimeSpanString (time, now, DAY_IN_MILLIS, FORMAT_ABBREV_RELATIVE), ms since the
 * epoch: "Today", "Yesterday", "N days ago" within a week, else "Nov 2" or "Nov 2, 2025" (another year) */
char      *calc_history_date_string    (gint64 time_ms, gint64 now_ms);

G_END_DECLS
