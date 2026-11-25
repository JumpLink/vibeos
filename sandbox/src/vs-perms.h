/* What a sandboxed app may do, and where that is kept.
 *
 * Every grant lives in the desktop's permission store (org.freedesktop.impl.portal.PermissionStore, a table, an id,
 * then a list of strings per app id), so that the portals, WirePlumber, Settings and the launcher all read the same
 * thing:
 *
 *   location      "location"/"location"     the location portal's own: ["EXACT", time] or ["NONE", time]
 *   camera        "devices"/"camera"        the camera portal's own: "yes", "no" or "ask"
 *   background    "background"/"background" the background portal's own: "yes", "no" or "ask"
 *   the others    "vibeos-sandbox"/<name>   "yes" or "no": network, microphone, contacts, calendar, phone, sms, files
 *
 * An app with no entry gets the permission's default: network is allowed (an app may use the internet from the
 * start), location and camera ask (their portals ask when the app first wants them), the microphone asks (sound has
 * no portal: WirePlumber's sandbox-access.lua holds the app's capture stream and puts a bus name up for the question,
 * the shell asks the user and writes "yes" or "no" to the app's microphone entry), background is allowed (what the
 * background portal does with no entry; registering an app sets it to "ask"), the rest are not allowed. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum {
  VS_PERM_NETWORK,
  VS_PERM_LOCATION,
  VS_PERM_CAMERA,
  VS_PERM_MICROPHONE,
  VS_PERM_CONTACTS,
  VS_PERM_CALENDAR,
  VS_PERM_PHONE,
  VS_PERM_SMS,
  VS_PERM_FILES,
  VS_PERM_BACKGROUND,
  VS_N_PERMS
} VsPerm;

typedef enum {
  VS_GRANT_DENY,
  VS_GRANT_ALLOW,
  VS_GRANT_ASK,
} VsGrant;

/* the table of our own grants in the permission store */
#define VS_STORE_TABLE "vibeos-sandbox"
/* the shell's reply to a microphone question the user did not answer (dismissed, timed out, the phone locked):
 * "vibeos-sandbox-ask"/"microphone", the app's entry "dismissed", which WirePlumber's script takes as "not this time";
 * the shell takes it away again once the question is over */
#define VS_ASK_TABLE "vibeos-sandbox-ask"

/* ---- the store: the permission store over D-Bus, or one in memory for the tests ---- */

typedef struct _VsStore VsStore;
struct _VsStore {
  /* the app's strings in table/id; NULL and no error: none (no table, no entry, or the app is not in it) */
  char   **(*get) (VsStore *store, const char *table, const char *id, const char *app, GError **error);
  /* value NULL takes the app out of the entry */
  gboolean (*set) (VsStore *store, const char *table, const char *id, const char *app, const char *const *value,
                   GError **error);
  /* the ids in a table; NULL and no error: no such table */
  char   **(*ids) (VsStore *store, const char *table, GError **error);
  void     (*free) (VsStore *store);
};

VsStore    *vs_store_new_dbus   (GDBusConnection *bus);
VsStore    *vs_store_new_memory (void);
void        vs_store_free       (VsStore *store);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (VsStore, vs_store_free)

/* ---- the permissions ---- */

const char *vs_perm_name        (VsPerm perm);
/* -1 for a name that is not one */
int         vs_perm_from_name   (const char *name);
/* the grant with no entry in the store */
VsGrant     vs_perm_default     (VsPerm perm);
/* a portal asks the user at runtime: "ask" means something */
gboolean    vs_perm_can_ask     (VsPerm perm);
/* a change reaches an app that is running (the portals and WirePlumber read the store each time); the others are
 * read once, when the app starts */
gboolean    vs_perm_is_live     (VsPerm perm);
const char *vs_grant_name       (VsGrant grant);
int         vs_grant_from_name  (const char *name);

/* the app's grant (its default when the store has none or cannot be reached) */
VsGrant     vs_perm_get         (VsStore *store, const char *app_id, VsPerm perm);
/* ASK for a permission that cannot ask is refused */
gboolean    vs_perm_set         (VsStore *store, const char *app_id, VsPerm perm, VsGrant grant, GError **error);

/* takes the app out of every table the portals and the sandbox keep an app's permissions in (its permissions, the
 * notifications, the documents it was given…), as when it is uninstalled; FALSE when one could not be */
gboolean    vs_perm_forget      (VsStore *store, const char *app_id, GError **error);

/* all of them at once, for a launch */
typedef struct {
  VsGrant grant[VS_N_PERMS];
} VsGrants;

void        vs_grants_load      (VsStore *store, const char *app_id, VsGrants *grants);
gboolean    vs_grants_allowed   (const VsGrants *grants, VsPerm perm);

G_END_DECLS
