/* ModemManager's network modes: SupportedModes/CurrentModes as a(uu)/(uu), and the argument of SetCurrentModes.
 * The list keeps the modem's order, drops invalid or duplicate pairs and gives each a label for Settings. GLib only,
 * so the tests use the same interface as the page. */
#pragma once

#include <gio/gio.h>

#define NS_MOBILE_MODES_TYPE "a(uu)"

typedef struct {
  guint32 allowed, preferred;
  char    *label;
} NsMobileMode;

typedef GPtrArray NsMobileModes;

NsMobileModes *ns_mobile_modes_new (GVariant *supported, GVariant *current, GError **error);
void           ns_mobile_modes_free (NsMobileModes *m);
gchar        **ns_mobile_modes_labels (NsMobileModes *m);
guint          ns_mobile_modes_current (NsMobileModes *m, GVariant *current);
GVariant      *ns_mobile_mode_value (const NsMobileMode *mode);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (NsMobileModes, ns_mobile_modes_free)
