/* CalcToast: Android's text toast, the "Text copied" a result's Copy shows (CalculatorResult.copyContent:
 * Toast.makeText (..., LENGTH_SHORT)), inside the app's own window.
 *
 * SystemUI's text_toast.xml: a pill (28 dp corners) at least 48 dp tall, 16 dp of padding either side, the app's
 * icon (24 dp, 12 dp before the text) and 14 sp text, centred 48 dp above the bottom (toast_y_offset). It fades in,
 * stays 2 s (LENGTH_SHORT) and fades out; a new one replaces the one showing.
 *
 * NOT MEASURED: no still or recording has a toast. The numbers are the framework's; check against a still.
 *
 * A libneo candidate: any app that confirms with a toast would want it. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* shows `text` over the window `widget` is in (made by neo_window_new) */
void calc_toast_show (GtkWidget *widget, const char *text);

G_END_DECLS
