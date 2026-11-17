/* SwUpdates: which installed apps the store has newer versions of.
 *
 * POST /updates/check (public: no account needed) gets every installed package and its version, and answers with the
 * store apps of them whose version for this architecture is newer, with sizes and release notes. Software asks when
 * it starts and when the installed list changes (an install or update finishes, apt ran) if the update setting
 * allows it (vibeos.Software update-mode: "wifi" only on an unmetered network, "always", "never"), and whenever the
 * user refreshes Mine. The count shows on the Mine tab.
 *
 * Software does not install updates by itself: Update and Update all on Mine do, through App Installer. */
#pragma once

#include "sw-api.h"

G_BEGIN_DECLS

#define SW_TYPE_UPDATES (sw_updates_get_type ())
G_DECLARE_FINAL_TYPE (SwUpdates, sw_updates, SW, UPDATES, GObject)

SwUpdates  *sw_updates_get_default (void);

/* asks the store; `asked` is a user's refresh (it ignores the setting) */
void        sw_updates_check       (SwUpdates *self, gboolean asked);
/* the update entry of `package` (app, installed_version, version, download_size, installed_size, release), or NULL */
JsonObject *sw_updates_get         (SwUpdates *self, const char *package);
/* every entry, by app name */
GPtrArray  *sw_updates_list        (SwUpdates *self);
gboolean    sw_updates_is_checking (SwUpdates *self);
/* the last check's error, or NULL */
const SwApiError *sw_updates_get_error (SwUpdates *self);
/* a check has answered since Software started */
gboolean    sw_updates_has_checked (SwUpdates *self);

/* Signals: "changed" () */

G_END_DECLS
