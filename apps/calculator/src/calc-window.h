/* calc-window: the calculator's window, ExactCalculator's Calculator activity (Calculator.java) with its
 * DisplayMotionLayout (activity_calculator.xml, activity_calculator_scene.xml). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* `settings`: neo.Calculator (the evaluator's degree mode, memory and clipboard entries) */
GtkWidget *calc_window_new (GtkApplication *app, GSettings *settings);

G_END_DECLS
