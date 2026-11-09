/* CalcLicenses: "Open source licenses", ExactCalculator's Licenses activity (activity_license.xml): a toolbar with
 * the back arrow and the title over a WebView of assets/licenses.html.
 *
 * There is no WebView here: the page is laid out by GTK from the same file (its one heading, its list item and its
 * <pre>), with Chrome's default styles for them (16 px sans-serif text, an 18.72 px bold h3, 13 px monospace in
 * the <pre> on #eeeeee with 1em of padding, 8 px of body margin) on the page's white. NOT MEASURED against a still.
 * It opens and closes without the activity transition, as the Dialer's screens do.
 *
 * Signals: "closed" () when its back arrow is tapped (the window removes it). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CALC_TYPE_LICENSES (calc_licenses_get_type ())
G_DECLARE_FINAL_TYPE (CalcLicenses, calc_licenses, CALC, LICENSES, GtkWidget)

GtkWidget *calc_licenses_new (void);

G_END_DECLS
