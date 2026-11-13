/* calc-window: the calculator's window: the display, the two pads and the history that a drag down from the
 * display pulls over them. Ported from ExactCalculator's main screen. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* `settings`: neo.Calculator (the evaluator's degree mode, memory and clipboard entries) */
GtkWidget *calc_window_new (GtkApplication *app, GSettings *settings);

G_END_DECLS
