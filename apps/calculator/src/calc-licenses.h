/* CalcLicenses: "Open source licenses": a toolbar with the back arrow and the title over ExactCalculator's
 * licenses.html.
 *
 * There is no web view here: the page is laid out by GTK from that file (its one heading, its list item and its
 * <pre>), with a browser's default styles for them, on the page's white.
 * It opens and closes without a transition, as the Dialer's screens do.
 *
 * Signals: "closed" () when its back arrow is tapped (the window removes it). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CALC_TYPE_LICENSES (calc_licenses_get_type ())
G_DECLARE_FINAL_TYPE (CalcLicenses, calc_licenses, CALC, LICENSES, GtkWidget)

GtkWidget *calc_licenses_new (void);

G_END_DECLS
