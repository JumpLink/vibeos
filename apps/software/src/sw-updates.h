/* SwUpdates: which installed apps the store has newer versions of.
 *
 * POST /updates/check (public: no account needed) gets every installed package and its version, and answers with the
 * store apps of them whose version for this architecture is newer, with sizes and release notes. Software asks when
 * it starts, when the installed list changes (an install or update finishes, apt ran), when the last check is an hour
 * old while it runs (looked at every ten minutes and when its window comes to the front) if the update setting allows
 * it (vibeos.Software update-mode: "wifi" only on an unmetered network, "always", "never"), and whenever the user
 * refreshes Mine. The count shows on the Mine tab. Without the window, `vibeos-software --check-updates` (sw-check.h)
 * asks from a timer and suggests the new ones in a notification.
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
/* checks (if the setting allows) when the last check went out an hour ago or more, or never */
void        sw_updates_check_if_stale (SwUpdates *self);
/* Mine shows the updates: they count as suggested (sw-check.h), and the notification about them goes. When the
 * timer's check found more since this list came, it asks the store again instead, and the answer is marked when Mine
 * shows it. */
void        sw_updates_mark_seen   (SwUpdates *self);

/* Signals: "changed" () */

G_END_DECLS
