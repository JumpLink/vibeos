/* An app's permissions. An app installed from Software runs sandboxed (its desktop file says
 * X-VibeOS-Sandbox=<app id>), and its page has a Permissions row that sums up what it is allowed, as Android's does.
 * The row opens App permissions: what the app asks for (its X-VibeOS-Permissions), in Android's three groups, Allowed,
 * Ask every time and Not allowed, always in the same order. A tap on one opens its choices; the choice is saved at once.
 * vibeos-sandbox reads and writes them (the desktop's permission store, where the portals and WirePlumber read them
 * too) and stops the app when one is taken away. What an app does not ask for it never gets, and is not listed.
 *
 * "Ask every time" is the store's third grant: nobody has decided yet, and the portal asks the user the next time the
 * app wants it. Only location, the camera and running in the background have it (vibeos-sandbox's vs-perms.c says
 * which); the others are allowed or not.
 *
 * Any other app is not sandboxed: it can do whatever the user's account may, and its row says so. */
#include "ns-pages.h"
#include "ns-ui.h"

#include <gio/gdesktopappinfo.h>

enum { GRANT_DENY, GRANT_ALLOW, GRANT_ASK, N_GRANTS };

static const char *const grant_names[N_GRANTS] = { "deny", "allow", "ask" };

static const struct {
  const char *name, *title, *icon;
  gboolean    can_ask;
} perms[] = {
  { "location", N_("Location"), "find-location-symbolic", TRUE },
  { "camera", N_("Camera"), "camera-photo-symbolic", TRUE },
  { "microphone", N_("Microphone"), "audio-input-microphone-symbolic", FALSE },
  { "contacts", N_("Contacts"), "avatar-default-symbolic", FALSE },
  { "calendar", N_("Calendar"), "x-office-calendar-symbolic", FALSE },
  { "phone", N_("Phone"), "call-start-symbolic", FALSE },
  { "sms", N_("SMS"), "mail-unread-symbolic", FALSE },
  { "files", N_("Files and media"), "folder-symbolic", FALSE },
  { "network", N_("Network"), "network-wireless-symbolic", FALSE },
  { "background", N_("Run in the background"), "preferences-system-time-symbolic", TRUE },
};
#define N_PERMS G_N_ELEMENTS (perms)

/* the App permissions page's groups, by grant */
enum { GROUP_ALLOWED, GROUP_ASK, GROUP_DENIED, N_GROUPS };
static const char *const group_titles[N_GROUPS] = { N_("Allowed"), N_("Ask every time"), N_("Not allowed") };

static int
group_of (int grant)
{
  return grant == GRANT_ALLOW ? GROUP_ALLOWED : grant == GRANT_ASK ? GROUP_ASK : GROUP_DENIED;
}

/* a permission's choices, in Android's order, and the grant each one writes */
static const char *const choices_ask[] = { N_("Allow"), N_("Ask every time"), N_("Don't allow"), NULL };
static const int grants_ask[] = { GRANT_ALLOW, GRANT_ASK, GRANT_DENY };
static const char *const choices_yes_no[] = { N_("Allow"), N_("Don't allow"), NULL };
static const int grants_yes_no[] = { GRANT_ALLOW, GRANT_DENY };
/* the group a row is in says its choice: the rows have no line under their title */
static const char *const no_summaries[] = { "", "", "", NULL };

typedef struct {
  char      *app_id;
  int        grant[N_PERMS];
  gboolean   requested[N_PERMS];
  gboolean   known;
  GtkWidget *groups[N_GROUPS];   /* the open App permissions page's cards (weak) */
  GtkWidget *none;               /* its note for an app that asks for nothing (weak) */
  GtkWidget *summary;            /* the app page's row (weak) */
} Perms;

/* keeps *where pointing at `w` while it lives */
static void
track (GtkWidget **where, GtkWidget *w)
{
  if (*where)
    g_object_remove_weak_pointer (G_OBJECT (*where), (gpointer *) where);
  *where = w;
  if (w)
    g_object_add_weak_pointer (G_OBJECT (w), (gpointer *) where);
}

static void
perms_free (Perms *p)
{
  track (&p->summary, NULL);
  track (&p->none, NULL);
  for (guint i = 0; i < N_GROUPS; i++)
    track (&p->groups[i], NULL);
  g_free (p->app_id);
  g_free (p);
}

static int
grant_from_name (const char *name)
{
  for (int g = 0; g < N_GRANTS; g++)
    if (!g_strcmp0 (grant_names[g], name))
      return g;
  return -1;
}

/* ---- what is allowed ---- */

/* the ones a summary names: what the user is asked about (the network and the background are no such thing) */
static gboolean
summed_up (guint i)
{
  return g_strcmp0 (perms[i].name, "network") && g_strcmp0 (perms[i].name, "background");
}

/* "Microphone", "Microphone and Camera", "Location, Camera and Microphone", or what there is none of */
static void
show_summary (Perms *p)
{
  if (!p->summary || !p->known)
    return;
  g_autoptr (GPtrArray) names = g_ptr_array_new ();
  gboolean asks = FALSE;
  for (guint i = 0; i < N_PERMS; i++) {
    if (!summed_up (i) || !p->requested[i])
      continue;
    asks = TRUE;
    if (p->grant[i] == GRANT_ALLOW)
      g_ptr_array_add (names, (gpointer) _(perms[i].title));
  }
  /* the list from its end: the last name, the last two joined by "and", each one before them by a comma */
  g_autofree char *list = NULL;
  for (guint i = names->len; i-- > 0;) {
    const char *name = names->pdata[i];
    char *longer = !list ? g_strdup (name)
                   /* TRANSLATORS: the last two of a list of permissions ("Camera and Microphone") */
                   : i + 2 == names->len ? g_strdup_printf (_("%s and %s"), name, list)
                   /* TRANSLATORS: a permission, then the rest of the list ("Location, Camera and Microphone") */
                                         : g_strdup_printf (_("%s, %s"), name, list);
    g_free (list);
    list = longer;
  }
  ns_row_set_subtitle (p->summary, list ? list : asks ? _("No permissions allowed") : _("No permissions requested"));
}

/* a row of the App permissions page: the app's page (which keeps the Perms) and the permission */
typedef struct {
  GObject *owner;
  guint    perm;
} PermRow;

static void
perm_row_free (PermRow *pr)
{
  g_object_unref (pr->owner);
  g_free (pr);
}

static void picked (GtkWidget *row, guint index, gpointer data);

/* the rows again, each in the group of its grant: a choice moves its row (the choices' page keeps the row it was
 * opened from, so it stays open) */
static void
fill_list (GObject *owner)
{
  Perms *p = g_object_get_data (owner, "ns-perms");
  if (!p || !p->groups[0] || !p->known)
    return;
  guint n[N_GROUPS] = { 0 };
  for (guint g = 0; g < N_GROUPS; g++)
    ns_group_clear (p->groups[g]);
  for (guint i = 0; i < N_PERMS; i++) {
    if (!p->requested[i])
      continue;
    int grant = (perms[i].can_ask || p->grant[i] != GRANT_ASK) ? p->grant[i] : GRANT_DENY;
    const char *const *choices = perms[i].can_ask ? choices_ask : choices_yes_no;
    const int *grants = perms[i].can_ask ? grants_ask : grants_yes_no;
    guint selected = 0;
    while (choices[selected] && grants[selected] != grant)
      selected++;
    int g = group_of (grant);
    PermRow *pr = g_new0 (PermRow, 1);
    pr->owner = g_object_ref (owner);
    pr->perm = i;
    GtkWidget *r = ns_row_pick (p->groups[g], _(perms[i].title), choices, selected, picked, pr);
    g_object_set_data_full (G_OBJECT (r), "ns-perm-row", pr, (GDestroyNotify) perm_row_free);
    vibeos_list_row_set_icon (VIBEOS_LIST_ROW (r), perms[i].icon);
    ns_row_pick_set_summaries (r, no_summaries);
    n[g]++;
  }
  for (guint g = 0; g < N_GROUPS; g++)
    ns_group_set_visible (p->groups[g], n[g] > 0);
  if (p->none)
    gtk_widget_set_visible (p->none, n[GROUP_ALLOWED] + n[GROUP_ASK] + n[GROUP_DENIED] == 0);
}

/* a line per permission: its name, its grant, and whether the app asks for it ("requested", "not-requested"; an
 * older vibeos-sandbox has no third column: it asks for all) */
static void
parse (Perms *p, const char *out)
{
  g_auto (GStrv) lines = g_strsplit (out ? out : "", "\n", -1);
  for (guint l = 0; lines[l]; l++) {
    g_auto (GStrv) f = g_strsplit (lines[l], "\t", 3);
    if (!f[0] || !f[1])
      continue;
    int g = grant_from_name (g_strstrip (f[1]));
    for (guint i = 0; i < N_PERMS; i++)
      if (!g_strcmp0 (perms[i].name, f[0]) && g >= 0) {
        p->grant[i] = g;
        p->requested[i] = !f[2] || g_strcmp0 (g_strstrip (f[2]), "not-requested");
      }
  }
  p->known = TRUE;
}

/* the page that owns the Perms stays until the answer is in */
static void
loaded (GObject *src, GAsyncResult *res, gpointer data)
{
  g_autoptr (GObject) owner = data;
  Perms *p = g_object_get_data (owner, "ns-perms");
  g_autofree char *out = NULL;
  if (!g_subprocess_communicate_utf8_finish (G_SUBPROCESS (src), res, &out, NULL, NULL) || !p)
    return;
  parse (p, out);
  show_summary (p);
  fill_list (owner);
}

static void
load (GObject *owner)
{
  Perms *p = g_object_get_data (owner, "ns-perms");
  if (!p)
    return;
  g_autoptr (GSubprocess) proc = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                                   NULL, "vibeos-sandbox", "permissions", p->app_id, NULL);
  if (proc)
    g_subprocess_communicate_utf8_async (proc, NULL, NULL, loaded, g_object_ref (owner));
}

static void
set_done (GObject *src, GAsyncResult *res, gpointer data)
{
  g_autoptr (GObject) owner = data;
  g_subprocess_wait_finish (G_SUBPROCESS (src), res, NULL);
  /* what the store says now, whatever happened */
  load (owner);
}

/* a choice made: in the store at once */
static void
picked (GtkWidget *row, guint index, gpointer data)
{
  PermRow *pr = data;
  Perms *p = g_object_get_data (pr->owner, "ns-perms");
  if (!p)
    return;
  const int *grants = perms[pr->perm].can_ask ? grants_ask : grants_yes_no;
  int grant = grants[index];
  if (p->grant[pr->perm] == grant)
    return;
  p->grant[pr->perm] = grant;
  show_summary (p);
  fill_list (pr->owner);
  g_autoptr (GSubprocess) proc = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                                   NULL, "vibeos-sandbox", "set", p->app_id, perms[pr->perm].name,
                                                   grant_names[grant], NULL);
  if (proc)
    g_subprocess_wait_async (proc, NULL, set_done, g_object_ref (pr->owner));
}

/* ---- App permissions ---- */

static void
list_page (GtkWidget *row, gpointer data)
{
  GObject *owner = data;
  Perms *p = g_object_get_data (owner, "ns-perms");
  GtkWidget *c;
  NsPage *page = ns_page_new (_("App permissions"), &c);
  /* the page keeps the app's page (and the Perms) while it is open */
  g_object_set_data_full (G_OBJECT (page), "ns-perms-owner", g_object_ref (owner), g_object_unref);
  for (guint g = 0; g < N_GROUPS; g++) {
    track (&p->groups[g], ns_group (c, _(group_titles[g])));
    ns_group_set_visible (p->groups[g], FALSE);
  }
  track (&p->none, ns_note (c, _("This app asks for no permissions.")));
  gtk_widget_set_visible (p->none, FALSE);
  ns_footnote (c, _("Taking a permission away stops the app. What an app does not ask for, it never gets."));
  fill_list (owner);
  ns_push (page);
  load (owner);
}

void
ns_app_permissions_row (GtkWidget *group, GAppInfo *app, GtkWidget *owner)
{
  g_autofree char *app_id = G_IS_DESKTOP_APP_INFO (app)
    ? g_desktop_app_info_get_string (G_DESKTOP_APP_INFO (app), "X-VibeOS-Sandbox") : NULL;
  if (!app_id || !*app_id) {
    /* not in a sandbox: nothing here keeps it from anything the user's account may do */
    ns_row (group, "security-low-symbolic", _("Permissions"), _("Not sandboxed: full access"));
    return;
  }
  Perms *p = g_new0 (Perms, 1);
  p->app_id = g_steal_pointer (&app_id);
  g_object_set_data_full (G_OBJECT (owner), "ns-perms", p, (GDestroyNotify) perms_free);
  track (&p->summary, ns_row (group, "security-high-symbolic", _("Permissions"), NULL));
  ns_row_on_activate (p->summary, list_page, owner);
  load (G_OBJECT (owner));
}

void
ns_app_permissions_refresh (GtkWidget *owner)
{
  load (G_OBJECT (owner));
}
