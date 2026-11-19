/* sw-check: looking for updates without the window, and remembering which updates were already suggested.
 *
 * `vibeos-software --check-updates` is the check without a window. The systemd user timer vibeos-software-updates.timer
 * runs it ten minutes after login and every six hours (a run missed while the phone was off or asleep happens as soon
 * as it can). It reads dpkg's list as the app does, asks the store (POST /updates/check), and, when the store has a
 * version that was not suggested before, posts one notification for all the updates. Tapping it opens Software on
 * Mine (the app action "show-updates"). It follows vibeos.Software update-mode as the app does: "never" checks
 * nothing, "wifi" skips a metered network. Without a notification server (no graphical session) it asks nothing.
 *
 * What was suggested is kept in ~/.local/share/vibeos-software/suggested-updates, one "package version" per line. A
 * version stays suggested once the notification told it or Mine showed it; a newer version of the same package is
 * suggested again. Each check that answers replaces the list with the store's current updates, so an app that was
 * updated (or whose update went away) is forgotten.
 *
 * No widgets here: the tests link it alone. */
#pragma once

#include "sw-api.h"

G_BEGIN_DECLS

/* the notification's id: a new one replaces the one before */
#define SW_CHECK_NOTIFICATION_ID "updates"
/* the app action its default action runs */
#define SW_CHECK_ACTION "show-updates"

/* whether vibeos.Software update-mode allows a check nobody asked for: "never" never, "always" always, "wifi" (and
 * anything unknown) when the network is not metered */
gboolean    sw_check_allowed        (const char *mode, gboolean metered);

/* the body of POST /updates/check: `arch` and every package of `installed` (package -> version), at most 5000 */
JsonNode   *sw_check_request        (GHashTable *installed, const char *arch);

/* The answer's entries (JsonObject, referenced) in the app's order: by the app's name. `data` is the answer's data
 * member; entries without an app package are left out. */
GPtrArray  *sw_check_entries        (JsonNode *data);

/* Asks the store for updates of `installed`. `callback` gets the entries (sw_check_entries), or the error. */
typedef void (*SwCheckCallback) (GPtrArray *entries, const SwApiError *error, gpointer data);
void        sw_check_ask            (SwApi *api, GHashTable *installed, const char *arch, GCancellable *cancellable,
                                     SwCheckCallback callback, gpointer data);

/* ~/.local/share/vibeos-software/suggested-updates */
char       *sw_suggested_path       (void);
/* package -> version, as kept in `path` (empty when there is no file) */
GHashTable *sw_suggested_load       (const char *path);
/* writes `suggested` to `path` (0600, made whole or not at all) */
gboolean    sw_suggested_save       (GHashTable *suggested, const char *path, GError **error);
/* the entries whose version is not the one suggested for their package: what a notification would announce */
GPtrArray  *sw_suggested_filter     (GHashTable *suggested, GPtrArray *entries);
/* makes `suggested` the entries' packages and versions; TRUE when that changed it */
gboolean    sw_suggested_replace    (GHashTable *suggested, GPtrArray *entries);

/* The version a person reads: the release's version from the store, else the package's without its epoch */
char       *sw_check_display_version (JsonObject *entry);
/* The notification's title and body for `entries` (all the updates; `fresh` of them not suggested before, which
 * are named first):
 *   one update      "Clock can be updated" / "Version 0.1.2 is ready to install."
 *   several         "3 apps can be updated" / "Clock, Weather and Files" ("…, Files and 2 more" past three) */
void        sw_check_notification_text (GPtrArray *entries, GPtrArray *fresh, char **title, char **body);

/* vibeos-software --check-updates: one check, as described above, with the app's environment (VIBEOS_STORE_URL,
 * VIBEOS_SOFTWARE_DPKG_STATUS) and settings. Returns the exit status. */
int         sw_check_run            (void);

G_END_DECLS
