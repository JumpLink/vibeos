/* An M3 outlined text field (TextInputLayout, Widget.Material3.TextInputLayout.OutlinedBox) around a GtkText or a
 * GtkTextView: a 1 dp colorOutline box with 4 dp corners (2 dp colorPrimary while it has the focus), and its label in
 * the box, at the text's size and place, while the text is empty and unfocused, else 12 sp on the box's top edge in
 * a gap of its own (CollapsingTextHelper, 167 ms).
 *
 * The widget is the TextInputLayout: the box starts the label's half height under its top (labelMarginTop, 16 px at
 * 3x) and is the EditText, @padding on every side of the text. Its lines are an EditText's: @text_size sp lines with
 * includeFontPadding, so the box is as tall as Etar's (still:cal-new-event). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CAL_TYPE_OUTLINED_FIELD (cal_outlined_field_get_type ())
G_DECLARE_FINAL_TYPE (CalOutlinedField, cal_outlined_field, CAL, OUTLINED_FIELD, GtkWidget)

/* @child: a GtkText or a GtkTextView, its font @text_size px with @letter_spacing (its CSS); @label: the hint */
GtkWidget *cal_outlined_field_new   (GtkWidget *child, const char *label, double text_size, double letter_spacing,
                                     double padding);

/* the box's outline from (@x, @y), @w × @h with @r corners, open on the top edge from @gap_start to @gap_end (closed
 * when they are equal) */
GskPath   *cal_outline_path         (double x, double y, double w, double h, double r, double gap_start,
                                     double gap_end);

G_END_DECLS
