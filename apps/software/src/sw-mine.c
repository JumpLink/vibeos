/* Mine: what this phone has.
 *
 * The tab: the account's picture (its page, or signing in), All apps, the jobs App Installer is running for Software
 * (installing, updating, uninstalling, waiting their turn), and the updates the store has (sw-updates.h), each with
 * Update, and Update all. A refresh asks the store for updates whatever the update setting says. Updates Mine has
 * shown count as suggested: the notification of updates (sw-check.h) goes, and does not come back for them.
 *
 * All apps: On this device is what dpkg says is installed, as the store's rows (POST /apps/lookup, 500 packages a
 * request), signed in or not. Not on this device is the account's history (GET /me/apps): apps it had on any of its
 * devices that this one does not have now. */
#include "sw-pages.h"
#include "sw-account.h"
#include "sw-installer.h"
#include "sw-json.h"
#include "sw-updates.h"
#include "sw-widgets.h"
#include "sw-window.h"

#include <glib/gi18n.h>

#define LOOKUP_CHUNK 500

typedef struct {
  GtkWidget *mine, *column, *jobs, *updates;
} Mine;

static void
clear (GtkWidget *box)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (box)))
    gtk_box_remove (GTK_BOX (box), child);
}

/* a job's app as a row: its row from a list seen before, else its package and name */
static JsonObject *
job_app (const SwJob *job)
{
  JsonObject *app = sw_app_cache_get (job->package);

  if (app)
    return json_object_ref (app);
  app = json_object_new ();
  json_object_set_string_member (app, "package", job->package);
  json_object_set_string_member (app, "name", job->name);
  return app;
}

static void
update_jobs (Mine *m)
{
  GPtrArray *jobs = sw_installer_get_jobs (sw_installer_get_default ());
  g_autofree char *count = NULL;

  clear (m->jobs);
  if (!jobs->len)
    return;
  count = g_strdup_printf ("%u", jobs->len);
  gtk_box_append (GTK_BOX (m->jobs), sw_section_header_new (_("In progress"), count, NULL, NULL, NULL));
  for (guint i = 0; i < jobs->len; i++)
    {
      const SwJob *job = g_ptr_array_index (jobs, i);
      g_autoptr (JsonObject) app = job_app (job);
      g_autofree char *line = sw_job_describe (job);
      /* TRANSLATORS: %s is how far the job has got ("Downloading 42%") */
      g_autofree char *what = g_strdup_printf (job->kind == SW_JOB_REMOVE ? _("Uninstalling · %s")
                                               : job->kind == SW_JOB_UPDATE ? _("Updating · %s") : _("Installing · %s"),
                                               line);

      gtk_box_append (GTK_BOX (m->jobs), sw_app_row_new (app, what));
    }
}

static void
on_update_all (GtkWidget *button, Mine *m)
{
  g_autoptr (GPtrArray) list = sw_updates_list (sw_updates_get_default ());

  for (guint i = 0; i < list->len; i++)
    {
      JsonObject *app = sw_json_object (g_ptr_array_index (list, i), "app");

      sw_installer_install (sw_installer_get_default (), sw_json_string (app, "package"), sw_json_string (app, "name"),
                            button);
    }
}

static void
on_check (GtkWidget *button, Mine *m)
{
  sw_updates_check (sw_updates_get_default (), TRUE);
}

static GtkWidget *
note (const char *text, const char *action, GCallback callback, gpointer data)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);

  gtk_widget_add_css_class (box, "sw-note");
  gtk_box_append (GTK_BOX (box), sw_label_new (text, "sw-body-text"));
  if (action)
    {
      GtkWidget *button = sw_button_new (action, SW_BUTTON_TONAL);

      gtk_widget_set_halign (button, GTK_ALIGN_START);
      g_signal_connect (button, "clicked", callback, data);
      gtk_box_append (GTK_BOX (box), button);
    }
  return box;
}

static void
update_updates (Mine *m)
{
  SwUpdates *updates = sw_updates_get_default ();
  g_autoptr (GPtrArray) list = sw_updates_list (updates);
  const SwApiError *error = sw_updates_get_error (updates);
  g_autofree char *count = list->len ? g_strdup_printf ("%u", list->len) : NULL;

  clear (m->updates);
  gtk_box_append (GTK_BOX (m->updates), sw_section_header_new (_("Updates"), count,
                                                               list->len > 1 ? _("Update all") : NULL,
                                                               G_CALLBACK (on_update_all), m));
  if (sw_updates_is_checking (updates) && !list->len)
    {
      GtkWidget *ring = sw_progress_new (32, 3);

      gtk_widget_set_halign (ring, GTK_ALIGN_START);
      gtk_widget_add_css_class (ring, "sw-note");
      gtk_box_append (GTK_BOX (m->updates), ring);
      return;
    }
  if (error && !list->len)
    {
      g_autofree char *text = sw_api_error_describe (error);

      gtk_box_append (GTK_BOX (m->updates), note (text, _("Retry"), G_CALLBACK (on_check), m));
      return;
    }
  if (!list->len)
    {
      if (sw_updates_has_checked (updates))
        gtk_box_append (GTK_BOX (m->updates), note (_("Your apps are up to date."), _("Check again"),
                                                    G_CALLBACK (on_check), m));
      else
        gtk_box_append (GTK_BOX (m->updates), note (_("Software has not looked for updates yet."),
                                                    _("Check for updates"), G_CALLBACK (on_check), m));
      return;
    }
  for (guint i = 0; i < list->len; i++)
    {
      JsonObject *entry = g_ptr_array_index (list, i), *app = sw_json_object (entry, "app");
      g_autofree char *size = sw_format_size (sw_json_int (entry, "download_size", 0));
      const char *version = sw_json_string (sw_json_object (entry, "release"), "version");
      g_autofree char *line = g_strdup_printf ("%s%s%s", version ? version : sw_json_string (entry, "version"),
                                               *size ? " · " : "", size);

      if (app)
        gtk_box_append (GTK_BOX (m->updates), sw_app_row_new (app, line));
    }
}

static void
on_jobs_changed (SwInstaller *installer, const char *package, GtkWidget *mine)
{
  /* the rows' lines say what each job is doing: drawn again on every change */
  update_jobs (g_object_get_data (G_OBJECT (mine), "sw-mine"));
}

static void
on_updates_changed (SwUpdates *updates, GtkWidget *mine)
{
  GtkRoot *root = gtk_widget_get_root (mine);

  update_updates (g_object_get_data (G_OBJECT (mine), "sw-mine"));
  /* a check answered while Mine is in front: what it found is seen */
  if (gtk_widget_get_mapped (mine) && GTK_IS_WINDOW (root) && gtk_window_is_active (GTK_WINDOW (root)))
    sw_updates_mark_seen (updates);
}

static void
on_all_apps (VibeosListRow *row, Mine *m)
{
  sw_window_push (GTK_WIDGET (row), sw_all_apps_page_new (0));
}

static void
on_avatar (GtkWidget *button, Mine *m)
{
  sw_window_open_account (button);
}

static void
on_mine_map (GtkWidget *widget, Mine *m)
{
  update_jobs (m);
  update_updates (m);
  sw_updates_mark_seen (sw_updates_get_default ());
}

GtkWidget *
sw_mine_new (void)
{
  Mine *m = g_new0 (Mine, 1);
  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0), *title = gtk_label_new (_("Mine"));
  GtkWidget *avatar, *cards, *row, *sheet;

  m->mine = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (m->mine, "sw-mine");
  g_object_set_data_full (G_OBJECT (m->mine), "sw-mine", m, g_free);

  gtk_widget_add_css_class (header, "sw-mine-header");
  gtk_label_set_xalign (GTK_LABEL (title), 0);
  gtk_widget_set_hexpand (title, TRUE);
  gtk_widget_add_css_class (title, "sw-mine-title");
  gtk_accessible_update_property (GTK_ACCESSIBLE (title), GTK_ACCESSIBLE_PROPERTY_LEVEL, 1, -1);
  gtk_box_append (GTK_BOX (header), title);
  avatar = vibeos_pressable_new (sw_avatar_new (40));
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (avatar), VIBEOS_RIPPLE_CIRCLE, 24);
  gtk_widget_add_css_class (avatar, "sw-avatar-button");
  gtk_accessible_update_property (GTK_ACCESSIBLE (avatar), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Account and settings"),
                                  -1);
  g_signal_connect (avatar, "clicked", G_CALLBACK (on_avatar), m);
  gtk_box_append (GTK_BOX (header), avatar);
  gtk_box_append (GTK_BOX (m->mine), header);

  m->column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (m->column, "sw-mine-column");
  cards = vibeos_card_list_new ();
  row = vibeos_list_row_new (_("All apps"), _("On this device and on your others"));
  vibeos_list_row_set_icon (VIBEOS_LIST_ROW (row), "sw-apps-symbolic");
  vibeos_list_row_set_chevron (VIBEOS_LIST_ROW (row));
  g_signal_connect (row, "clicked", G_CALLBACK (on_all_apps), m);
  vibeos_card_list_append (VIBEOS_CARD_LIST (cards), row);
  gtk_box_append (GTK_BOX (m->column), cards);
  /* the jobs and the updates on a sheet with rounded top corners, over the tab's grey */
  sheet = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (sheet, "sw-mine-sheet");
  gtk_widget_set_vexpand (sheet, TRUE);
  m->jobs = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (m->jobs, "sw-mine-section");
  gtk_box_append (GTK_BOX (sheet), m->jobs);
  m->updates = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (m->updates, "sw-mine-section");
  gtk_box_append (GTK_BOX (sheet), m->updates);
  gtk_box_append (GTK_BOX (m->column), sheet);
  gtk_box_append (GTK_BOX (m->mine), sw_scroll_new (m->column));

  g_signal_connect_object (sw_installer_get_default (), "changed", G_CALLBACK (on_jobs_changed), m->mine, 0);
  g_signal_connect_object (sw_updates_get_default (), "changed", G_CALLBACK (on_updates_changed), m->mine, 0);
  g_signal_connect (m->mine, "map", G_CALLBACK (on_mine_map), m);
  update_jobs (m);
  update_updates (m);
  return m->mine;
}

/* ---- All apps -------------------------------------------------------------------------------------------------- */

typedef struct {
  GtkWidget  *page, *tabs, *pages, *here, *here_rows, *away, *away_rows;
  GPtrArray  *found;         /* AppSummary, from the lookups */
  guint       waiting;       /* lookups not answered yet */
  gboolean    here_failed;
} AllApps;

static void load_here (AllApps *a);
static void load_away (AllApps *a);

static void
all_apps_free (gpointer data)
{
  AllApps *a = data;

  g_ptr_array_unref (a->found);
  g_free (a);
}

static int
by_name (gconstpointer x, gconstpointer y)
{
  return g_utf8_collate (sw_json_string (*(JsonObject **) x, "name") ?: "",
                         sw_json_string (*(JsonObject **) y, "name") ?: "");
}

static void
on_lookup (SwApiResult *result, const SwApiError *error, gpointer data)
{
  AllApps *a = data;
  JsonArray *list = result ? sw_json_node_array (result->data) : NULL;

  a->waiting--;
  if (error)
    {
      if (!a->here_failed)
        sw_status_error (SW_STATUS (a->here), error, (SwRetryFunc) load_here, a);
      a->here_failed = TRUE;
      return;
    }
  for (guint i = 0; list && i < json_array_get_length (list); i++)
    if (sw_json_array_object (list, i))
      g_ptr_array_add (a->found, json_object_ref (sw_json_array_object (list, i)));
  if (a->waiting || a->here_failed)
    return;
  g_ptr_array_sort (a->found, by_name);
  clear (a->here_rows);
  for (guint i = 0; i < a->found->len; i++)
    {
      JsonObject *app = g_ptr_array_index (a->found, i);
      const char *version = sw_installer_get_installed (sw_installer_get_default (), sw_json_string (app, "package"));
      g_autofree char *line = version ? g_strdup_printf (_("Version %s"), version) : NULL;

      gtk_box_append (GTK_BOX (a->here_rows), sw_app_row_new (app, line));
    }
  if (!a->found->len)
    sw_status_empty (SW_STATUS (a->here), "sw-phone-symbolic", _("None of the apps on this device are in the store."),
                     NULL, NULL, NULL);
  else
    sw_status_show (SW_STATUS (a->here));
}

static void
send_lookup (AllApps *a, GPtrArray *packages)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (JsonNode) body = NULL;

  json_builder_begin_object (b);
  json_builder_set_member_name (b, "packages");
  json_builder_begin_array (b);
  for (guint i = 0; i < packages->len; i++)
    json_builder_add_string_value (b, g_ptr_array_index (packages, i));
  json_builder_end_array (b);
  json_builder_end_object (b);
  body = json_builder_get_root (b);
  a->waiting++;
  sw_api_send (sw_api_get_default (), "POST", "/apps/lookup", body, SW_API_NO_TOKEN, sw_cancellable_for (a->page),
               on_lookup, a);
}

static void
load_here (AllApps *a)
{
  SwInstaller *installer = sw_installer_get_default ();
  GHashTableIter iter;
  gpointer package;
  g_autoptr (GPtrArray) chunk = g_ptr_array_new ();

  sw_status_loading (SW_STATUS (a->here));
  if (!sw_installer_is_loaded (installer))
    return;
  g_ptr_array_set_size (a->found, 0);
  a->here_failed = FALSE;
  a->waiting = 0;
  g_hash_table_iter_init (&iter, sw_installer_get_installed_all (installer));
  while (g_hash_table_iter_next (&iter, &package, NULL))
    {
      g_ptr_array_add (chunk, package);
      if (chunk->len == LOOKUP_CHUNK)
        {
          send_lookup (a, chunk);
          g_ptr_array_set_size (chunk, 0);
        }
    }
  if (chunk->len)
    send_lookup (a, chunk);
  if (!a->waiting)
    sw_status_empty (SW_STATUS (a->here), "sw-phone-symbolic", _("Nothing is installed."), NULL, NULL, NULL);
}

static void
sign_in (AllApps *a)
{
  sw_window_push (a->page, sw_sign_in_page_new ());
}

static void
on_my_apps (SwApiResult *result, const SwApiError *error, gpointer data)
{
  AllApps *a = data;
  JsonArray *list;
  int shown = 0;

  if (error)
    {
      sw_status_error (SW_STATUS (a->away), error, (SwRetryFunc) load_away, a);
      return;
    }
  list = sw_json_array (sw_json_node_object (result->data), "not_on_this_device");
  clear (a->away_rows);
  for (guint i = 0; list && i < json_array_get_length (list); i++)
    {
      JsonObject *entry = sw_json_array_object (list, i), *app = sw_json_object (entry, "app");
      g_autofree char *date = sw_format_date (sw_json_string (entry, "last_installed_at"));
      g_autofree char *line = NULL;

      /* the store may not know yet that it is back on this one */
      if (!app || sw_installer_get_installed (sw_installer_get_default (), sw_json_string (app, "package")))
        continue;
      line = sw_json_bool (entry, "on_other_devices", FALSE) ? g_strdup (_("On another of your devices"))
             /* TRANSLATORS: %s is a date */
             : date ? g_strdup_printf (_("Last installed %s"), date) : NULL;
      gtk_box_append (GTK_BOX (a->away_rows), sw_app_row_new (app, line));
      shown++;
    }
  if (!shown)
    sw_status_empty (SW_STATUS (a->away), "sw-phone-symbolic",
                     _("Every app your account had is on this device."), NULL, NULL, NULL);
  else
    sw_status_show (SW_STATUS (a->away));
}

static void
load_away (AllApps *a)
{
  if (!sw_account_is_signed_in (sw_account_get_default ()))
    {
      sw_status_empty (SW_STATUS (a->away), "sw-account-symbolic",
                       _("Sign in to see the apps your account had on your other devices, or before."), _("Sign in"),
                       (SwRetryFunc) sign_in, a);
      return;
    }
  sw_status_loading (SW_STATUS (a->away));
  sw_api_get (sw_api_get_default (), "/me/apps", SW_API_NONE, sw_cancellable_for (a->page), on_my_apps, a);
}

static void
on_tab (SwTabs *tabs, int index, AllApps *a)
{
  gtk_stack_set_visible_child (GTK_STACK (a->pages), index ? a->away : a->here);
  if (index)
    load_away (a);
}

static void
on_all_account_changed (SwAccount *account, GtkWidget *page)
{
  AllApps *a = g_object_get_data (G_OBJECT (page), "sw-all-apps");

  if (sw_tabs_get_selected (SW_TABS (a->tabs)) == 1)
    load_away (a);
}

static void
on_installed_changed (SwInstaller *installer, const char *package, GtkWidget *page)
{
  AllApps *a = g_object_get_data (G_OBJECT (page), "sw-all-apps");

  /* the list's first reading, if the page came before it */
  if (!package && !a->found->len && !a->waiting)
    load_here (a);
}

GtkWidget *
sw_all_apps_page_new (int tab)
{
  AllApps *a = g_new0 (AllApps, 1);

  a->found = g_ptr_array_new_with_free_func ((GDestroyNotify) json_object_unref);
  a->page = sw_page_new (_("All apps"));
  g_object_set_data_full (G_OBJECT (a->page), "sw-all-apps", a, all_apps_free);
  a->tabs = sw_tabs_new (_("On this device"), _("Not on this device"), NULL);
  g_signal_connect (a->tabs, "changed", G_CALLBACK (on_tab), a);
  gtk_box_append (GTK_BOX (a->page), a->tabs);
  a->here_rows = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (a->here_rows, "sw-list");
  a->here = sw_status_new (sw_scroll_new (a->here_rows));
  a->away_rows = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (a->away_rows, "sw-list");
  a->away = sw_status_new (sw_scroll_new (a->away_rows));
  a->pages = gtk_stack_new ();
  gtk_stack_add_child (GTK_STACK (a->pages), a->here);
  gtk_stack_add_child (GTK_STACK (a->pages), a->away);
  sw_page_set_body (a->page, a->pages);
  g_signal_connect_object (sw_account_get_default (), "changed", G_CALLBACK (on_all_account_changed), a->page, 0);
  g_signal_connect_object (sw_installer_get_default (), "changed", G_CALLBACK (on_installed_changed), a->page, 0);
  load_here (a);
  if (tab == 1)
    {
      sw_tabs_set_selected (SW_TABS (a->tabs), 1);
      on_tab (SW_TABS (a->tabs), 1, a);
    }
  return a->page;
}
