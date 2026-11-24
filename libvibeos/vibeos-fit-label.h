/* VibeosFitLabel: a GtkLabel that shrinks to fit, as Android's TextView autoSize does.
 *
 * Where a word of the label is wider than the space it is given, or its text takes more lines than the label allows
 * (gtk_label_set_lines), the label is drawn smaller, as little as it takes, down to VIBEOS_FIT_LABEL_MIN_SCALE of its
 * size. Only where even that is not enough does a word break (and the text past the label's lines ellipsize, as the
 * label is set up to). A word wider than the space therefore never widens what holds the label.
 *
 * The label is set up as usual (its text, lines, ellipsizing, alignment and CSS classes) and handed to the fit label,
 * which holds it; it wraps, at words, and breaks a word only below the smallest size. Its CSS padding shrinks with
 * it: a box around the text belongs on the fit label (CSS name vibeos-fit-label). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define VIBEOS_TYPE_FIT_LABEL (vibeos_fit_label_get_type ())
G_DECLARE_FINAL_TYPE (VibeosFitLabel, vibeos_fit_label, VIBEOS, FIT_LABEL, GtkWidget)

/* the smallest the label is drawn, against its own size */
#define VIBEOS_FIT_LABEL_MIN_SCALE 0.8

/* `label`, a GtkLabel, in a fit label */
GtkWidget *vibeos_fit_label_new       (GtkWidget *label);
GtkWidget *vibeos_fit_label_get_label (VibeosFitLabel *self);

G_END_DECLS
