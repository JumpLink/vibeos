/* The window (fm-window.h). */
#include "fm-window.h"
#include "fm-browser.h"
#include "fm-dialogs.h"
#include "fm-drawer.h"
#include "fm-home.h"
#include "fm-icon.h"
#include "fm-jobs.h"
#include "fm-remote-page.h"
#include "fm-settings.h"
#include "fm-share.h"
#include "fm-ui.h"
#include "fm-util.h"

#include <glib/gi18n.h>
#include <vibeos.h>

#define BACK_MAX        100
#define TOAST_FADE_MS   250
#define TOAST_SHORT_MS  2000    /* how long a toast stays */

typedef struct {
  FmPageKind page;
  GFile *location;              /* the browser's */
} BackEntry;

/* the window's own part of the shell */
typedef struct {
  FmShell pub;
  /* the toolbar's buttons that come and go */
  GtkWidget *search, *view, *empty, *edit, *add, *overflow;
  char *bottom_mode;            /* what the bottom bar holds now: rebuilt only when it changes */
  GtkWidget *toast, *toast_label;
  VibeosAnim *toast_anim;
  guint toast_timeout;
  GtkWidget *back_marker;
  GtkWidget *row;               /* the window's content: the docked drawer, then the page */
  VibeosLayout *layout;
  FmSnackAction snack_cb;
  gpointer snack_data;
  GDestroyNotify snack_destroy;
  gulong search_changed;
} Shell;

#define SHELL(s) ((Shell *) (s))

FmShell *
fm_shell_get (GtkWidget *widget)
{
  GtkRoot *root = gtk_widget_get_root (widget);

  return root ? g_object_get_data (G_OBJECT (root), "fm-shell") : NULL;
}

static void
back_entry_free (gpointer data)
{
  BackEntry *entry = data;

  g_clear_object (&entry->location);
  g_free (entry);
}

/* ---- the places ---------------------------------------------------------------------------------------------------- */

GList *
fm_shell_mounts (FmShell *shell)
{
  GList *mounts = g_volume_monitor_get_mounts (shell->monitor), *visible = NULL;

  for (GList *l = mounts; l; l = l->next)
    if (!g_mount_is_shadowed (l->data))
      visible = g_list_prepend (visible, g_object_ref (l->data));
  g_list_free_full (mounts, g_object_unref);
  return g_list_reverse (visible);
}

FmPlace *
fm_shell_place_for (FmShell *shell, GFile *file)
{
  GList *mounts = fm_shell_mounts (shell);
  FmPlace *place = fm_place_find (file, shell->remotes, mounts);

  g_list_free_full (mounts, g_object_unref);
  return place;
}

FmRemote *
fm_shell_find_remote (FmShell *shell, const char *id)
{
  for (guint i = 0; id && i < shell->remotes->len; i++)
    {
      FmRemote *remote = shell->remotes->pdata[i];

      if (!g_strcmp0 (remote->id, id))
        return remote;
    }
  return NULL;
}

void
fm_shell_remotes_changed (FmShell *shell)
{
  g_autoptr(GError) error = NULL;

  if (!fm_remotes_save (shell->remotes, shell->remotes_path, &error))
    g_warning ("Could not save the remotes to %s: %s", shell->remotes_path, error->message);
  fm_remote_page_refresh (shell->remote_page);
  fm_home_refresh (shell->home);
  fm_drawer_refresh (shell->drawer);
  fm_shell_update_bars (shell);
}

/* ---- the toast and the snackbar ------------------------------------------------------------------------------------ */

static void
on_toast_anim (VibeosAnim *anim, double value, gpointer data)
{
  Shell *s = data;

  gtk_widget_set_opacity (s->toast, value);
  gtk_widget_set_visible (s->toast, value > 0);
}

static gboolean
on_toast_timeout (gpointer data)
{
  Shell *s = data;

  s->toast_timeout = 0;
  vibeos_anim_start (s->toast_anim, vibeos_anim_get_value (s->toast_anim), 0, TOAST_FADE_MS, 0, SPONGY_LINEAR);
  return G_SOURCE_REMOVE;
}

void
fm_shell_toast (FmShell *shell, const char *text)
{
  Shell *s = SHELL (shell);

  gtk_label_set_label (GTK_LABEL (s->toast_label), text);
  g_clear_handle_id (&s->toast_timeout, g_source_remove);
  vibeos_anim_start (s->toast_anim, vibeos_anim_get_value (s->toast_anim), 1, TOAST_FADE_MS, 0, SPONGY_LINEAR);
  s->toast_timeout = g_timeout_add (TOAST_FADE_MS + TOAST_SHORT_MS, on_toast_timeout, s);
}

static GtkWidget *
toast_new (Shell *s)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *icon = gtk_image_new_from_icon_name ("vibeos.Files");

  gtk_image_set_pixel_size (GTK_IMAGE (icon), 24);
  gtk_widget_add_css_class (icon, "fm-toast-icon");
  gtk_box_append (GTK_BOX (box), icon);
  s->toast_label = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (s->toast_label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (s->toast_label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_lines (GTK_LABEL (s->toast_label), 2);
  gtk_label_set_ellipsize (GTK_LABEL (s->toast_label), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (GTK_LABEL (s->toast_label), 30);
  gtk_label_set_xalign (GTK_LABEL (s->toast_label), 0);
  gtk_widget_add_css_class (s->toast_label, "fm-toast-text");
  gtk_box_append (GTK_BOX (box), s->toast_label);
  gtk_widget_add_css_class (box, "fm-toast");
  gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (box, GTK_ALIGN_END);
  gtk_widget_set_can_target (box, FALSE);
  gtk_widget_set_visible (box, FALSE);
  gtk_accessible_update_property (GTK_ACCESSIBLE (box), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Message"), -1);
  s->toast = box;
  s->toast_anim = vibeos_anim_new (box, on_toast_anim, s);
  return box;
}

static void
snack_clear (Shell *s)
{
  if (s->snack_destroy && s->snack_data)
    s->snack_destroy (s->snack_data);
  s->snack_cb = NULL;
  s->snack_data = NULL;
  s->snack_destroy = NULL;
}

static void
on_snack_action (gpointer data)
{
  Shell *s = data;
  FmSnackAction cb = s->snack_cb;

  s->snack_cb = NULL;
  if (cb)
    cb (&s->pub, s->snack_data);
}

void
fm_shell_snackbar (FmShell *shell, const char *text, const char *action, FmSnackAction action_cb, gpointer data,
                   GDestroyNotify destroy)
{
  Shell *s = SHELL (shell);
  g_autofree char *caps = action ? fm_caps (action) : NULL;

  snack_clear (s);
  s->snack_cb = action_cb;
  s->snack_data = data;
  s->snack_destroy = destroy;
  vibeos_snackbar_show (VIBEOS_SNACKBAR (shell->snackbar), text, action_cb ? caps : NULL,
                     action_cb ? on_snack_action : NULL, s, VIBEOS_SNACKBAR_LONG);
}

void
fm_shell_job_progress (FmShell *shell, double fraction)
{
  gtk_widget_set_visible (shell->job_line, fraction >= 0);
  if (fraction >= 0)
    fm_progress_set_fraction (shell->job_line, fraction);
}

/* ---- going places -------------------------------------------------------------------------------------------------- */

static const char *
page_name (FmPageKind page)
{
  switch (page)
    {
    case FM_PAGE_REMOTE:
      return "remote";
    case FM_PAGE_BROWSER:
      return "browser";
    case FM_PAGE_SETTINGS:
      return "settings";
    case FM_PAGE_HOME:
    default:
      return "home";
    }
}

/* search off without listing again (the next listing replaces it) */
static void
leave_search (FmShell *shell)
{
  Shell *s = SHELL (shell);

  if (!fm_browser_is_searching (shell->browser))
    return;
  fm_browser_search_end (shell->browser);
  g_signal_handler_block (shell->search_entry, s->search_changed);
  gtk_editable_set_text (GTK_EDITABLE (shell->search_entry), "");
  g_signal_handler_unblock (shell->search_entry, s->search_changed);
}

static void
push_back (FmShell *shell)
{
  BackEntry *entry = g_new0 (BackEntry, 1);
  GFile *location = fm_browser_get_location (shell->browser);

  entry->page = shell->page;
  if (shell->page == FM_PAGE_BROWSER && location)
    entry->location = g_object_ref (location);
  else if (shell->page == FM_PAGE_BROWSER)
    entry->page = FM_PAGE_HOME;
  g_ptr_array_add (shell->back, entry);
  if (shell->back->len > BACK_MAX)
    g_ptr_array_remove_index (shell->back, 0);
}

static void
set_page (FmShell *shell, FmPageKind page)
{
  if (page != FM_PAGE_BROWSER)
    {
      FmList *list = fm_browser_get_list (shell->browser);

      leave_search (shell);
      if (fm_list_get_selecting (list))
        fm_list_set_selecting (list, FALSE);
    }
  if (shell->page == FM_PAGE_REMOTE && page != FM_PAGE_REMOTE && fm_remote_page_is_editing (shell->remote_page))
    fm_remote_page_toggle_edit (shell->remote_page);
  shell->page = page;
  gtk_stack_set_visible_child_name (GTK_STACK (shell->pages), page_name (page));
  fm_shell_update_bars (shell);
}

static void
browse (FmShell *shell, GFile *location)
{
  leave_search (shell);
  fm_browser_load (shell->browser, location);
  set_page (shell, FM_PAGE_BROWSER);
}

void
fm_shell_open_location (FmShell *shell, GFile *location)
{
  GFile *current = fm_browser_get_location (shell->browser);

  if (fm_drawer_is_open (shell->drawer))
    fm_drawer_open (shell->drawer, FALSE);
  if (shell->page == FM_PAGE_BROWSER && current && g_file_equal (current, location)
      && !fm_browser_is_searching (shell->browser))
    {
      fm_browser_reload (shell->browser, NULL);
      return;
    }
  push_back (shell);
  browse (shell, location);
}

void
fm_shell_show_page (FmShell *shell, FmPageKind page)
{
  if (fm_drawer_is_open (shell->drawer))
    fm_drawer_open (shell->drawer, FALSE);
  if (page == shell->page)
    return;
  push_back (shell);
  if (page == FM_PAGE_HOME)
    fm_home_refresh (shell->home);
  else if (page == FM_PAGE_REMOTE)
    fm_remote_page_refresh (shell->remote_page);
  set_page (shell, page);
}

/* ---- opening a file ------------------------------------------------------------------------------------------------ */

static void
launch_failed (GtkWidget *window, const GError *error)
{
  FmShell *shell = g_object_get_data (G_OBJECT (window), "fm-shell");

  if (!shell)
    return;
  g_warning ("Could not open the file: %s", error->message);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)
      || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)
      || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_FAILED))
    fm_shell_toast (shell, _("No apps are installed to open this type of file"));
  else
    fm_shell_toast (shell, error->message);
}

static gboolean
launch_files (GtkWidget *window, GAppInfo *app, GList *files)
{
  g_autoptr(GAppLaunchContext) context = G_APP_LAUNCH_CONTEXT (gdk_display_get_app_launch_context (
    gtk_widget_get_display (window)));
  g_autoptr(GError) error = NULL;

  if (g_app_info_launch (app, files, context, &error))
    return TRUE;
  launch_failed (window, error);
  return FALSE;
}

/* ---- opening a file: as an app opens from its icon ----------------------------------------------------------------- */

/* A file opens in the app for its type, which gets its URI or GVfs's FUSE path for a server's file. The shell starts
 * the app (vibeos.Shell.Open): out of the file's icon as an app out of its icon on the home screen, its splash until
 * the window is there, and back at the window's root flies it back into the file. A shell without Open: the app is
 * started here, as it was. */
typedef struct {
  GtkWidget *window;
  GFile *file;
  GAppInfo *app;
  double box[4];        /* the icon it opens out of, in the window's surface px; no size: none */
} OpenRequest;

static void
open_request_free (OpenRequest *r)
{
  g_object_unref (r->window);
  g_object_unref (r->file);
  g_clear_object (&r->app);
  g_free (r);
}

static void
open_here (OpenRequest *r)
{
  GList files = { r->file, NULL, NULL };

  launch_files (r->window, r->app, &files);
}

static void
on_shell_open (GObject *source, GAsyncResult *result, gpointer data)
{
  OpenRequest *r = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);

  if (!ret)
    {
      g_debug ("vibeos.Shell.Open: %s", error->message);
      open_here (r);
    }
  open_request_free (r);
}

static void
on_default_handler (GObject *source, GAsyncResult *result, gpointer data)
{
  OpenRequest *r = data;
  g_autoptr(GError) error = NULL;
  GDBusConnection *bus;
  g_autofree char *uri = NULL;
  const char *id;

  r->app = g_file_query_default_handler_finish (G_FILE (source), result, &error);
  if (!r->app)
    {
      launch_failed (r->window, error);
      open_request_free (r);
      return;
    }
  id = g_app_info_get_id (r->app);
  bus = g_application_get_dbus_connection (g_application_get_default ());
  if (!id || !bus)
    {
      open_here (r);
      open_request_free (r);
      return;
    }
  uri = g_file_get_uri (r->file);
  g_dbus_connection_call (bus, "vibeos.Shell", "/vibeos/Shell", "vibeos.Shell", "Open",
                          g_variant_new ("(ss(dddd))", uri, id, r->box[0], r->box[1], r->box[2], r->box[3]),
                          NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, NULL, on_shell_open, r);
}

/* @from's box in its window's surface (what the shell places windows by) */
static void
surface_box (GtkWidget *from, double box[4])
{
  GtkNative *native;
  graphene_rect_t r;
  double sx, sy;

  box[0] = box[1] = box[2] = box[3] = 0;
  if (!from || !(native = gtk_widget_get_native (from))
      || !gtk_widget_compute_bounds (from, GTK_WIDGET (native), &r))
    return;
  gtk_native_get_surface_transform (native, &sx, &sy);
  box[0] = r.origin.x + sx;
  box[1] = r.origin.y + sy;
  box[2] = r.size.width;
  box[3] = r.size.height;
}

static void
open_file (FmShell *shell, GFile *file, GtkWidget *from)
{
  OpenRequest *r = g_new0 (OpenRequest, 1);

  r->window = g_object_ref (shell->window);
  r->file = g_object_ref (file);
  surface_box (from, r->box);
  g_file_query_default_handler_async (file, G_PRIORITY_DEFAULT, NULL, on_default_handler, r);
}

void
fm_shell_open_item_from (FmShell *shell, GFile *location, gboolean is_folder, GtkWidget *from)
{
  if (is_folder)
    fm_shell_open_location (shell, location);
  else
    open_file (shell, location, from);
}

void
fm_shell_open_item_location (FmShell *shell, GFile *location, gboolean is_folder)
{
  fm_shell_open_item_from (shell, location, is_folder, NULL);
}

/* ---- the selection's actions --------------------------------------------------------------------------------------- */

static GPtrArray *
selected_items (FmShell *shell)
{
  return fm_list_get_selected (fm_browser_get_list (shell->browser));
}

/* where the selected rows really are (a recent file's target, not its recent:// entry) */
static GPtrArray *
locations_of (GPtrArray *items)
{
  GPtrArray *files = g_ptr_array_new_with_free_func (g_object_unref);

  for (guint i = 0; i < items->len; i++)
    g_ptr_array_add (files, fm_item_get_location (items->pdata[i]));
  return files;
}

static void
end_selection (FmShell *shell)
{
  fm_list_set_selecting (fm_browser_get_list (shell->browser), FALSE);
  fm_shell_update_bars (shell);
}

void
fm_shell_set_clip (FmShell *shell, GPtrArray *files, gboolean cut)
{
  g_clear_pointer (&shell->clip, g_ptr_array_unref);
  shell->clip = files ? g_ptr_array_ref (files) : NULL;
  shell->clip_cut = cut;
  fm_shell_update_bars (shell);
}

static void
take (FmShell *shell, gboolean cut)
{
  g_autoptr(GPtrArray) items = selected_items (shell);
  g_autoptr(GPtrArray) files = locations_of (items);

  end_selection (shell);
  if (files->len)
    fm_shell_set_clip (shell, files, cut);
}

/* Delete: the question (unless recycling without one), then the job */
typedef struct {
  FmShell *shell;
  GPtrArray *files;
} DeleteRequest;

static void
on_delete_answer (gboolean ok, gboolean permanently, gpointer data)
{
  DeleteRequest *request = data;

  if (ok)
    {
      fm_jobs_delete (request->shell, request->files, !permanently);
      end_selection (request->shell);
    }
  g_ptr_array_unref (request->files);
  g_free (request);
}

static void
delete_selected (FmShell *shell)
{
  g_autoptr(GPtrArray) items = selected_items (shell);
  gboolean in_trash = fm_browser_is_trash (shell->browser);
  gboolean can_trash = !in_trash && items->len > 0;
  gboolean use_trash = g_settings_get_boolean (shell->settings, "use-recycle-bin");
  DeleteRequest *request;

  if (!items->len)
    return;
  for (guint i = 0; i < items->len && can_trash; i++)
    {
      FmItem *item = items->pdata[i];

      can_trash = item->can_trash;
    }
  request = g_new0 (DeleteRequest, 1);
  request->shell = shell;
  /* in the Recycle Bin the entries themselves go; elsewhere the files */
  if (in_trash)
    {
      request->files = g_ptr_array_new_with_free_func (g_object_unref);
      for (guint i = 0; i < items->len; i++)
        g_ptr_array_add (request->files, g_object_ref (((FmItem *) items->pdata[i])->file));
    }
  else
    request->files = locations_of (items);
  if (can_trash && use_trash && !g_settings_get_boolean (shell->settings, "recycle-confirmation"))
    on_delete_answer (TRUE, FALSE, request);
  else
    fm_delete_dialog (shell->window, items, can_trash, use_trash, on_delete_answer, request);
}

static void
restore_selected (FmShell *shell)
{
  g_autoptr(GPtrArray) items = selected_items (shell);

  if (!items->len)
    return;
  fm_jobs_restore (shell, items);
  end_selection (shell);
}

/* Rename */
typedef struct {
  FmShell *shell;
  FmItem *item;
  GFile *file;
} RenameRequest;

static void
rename_request_free (RenameRequest *request)
{
  g_object_unref (request->item);
  g_object_unref (request->file);
  g_free (request);
}

static const char *
check_rename (const char *text, gpointer data)
{
  RenameRequest *request = data;
  FmNameCheck check = fm_check_name (text);
  const char *original = request->item->edit_name ? request->item->edit_name : request->item->name;

  if (check != FM_NAME_OK)
    return fm_name_check_message (check);
  if (g_strcmp0 (text, original) && fm_browser_has_name (request->shell->browser, text))
    return _("File or folder already exists");
  return NULL;
}

static void
on_renamed (GObject *source, GAsyncResult *result, gpointer data)
{
  GtkWidget *window = data;
  FmShell *shell = g_object_get_data (G_OBJECT (window), "fm-shell");
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) renamed = g_file_set_display_name_finish (G_FILE (source), result, &error);

  if (shell && renamed)
    {
      g_autofree char *name = g_file_get_basename (renamed);

      fm_shell_toast (shell, _("Renamed successfully"));
      if (shell->page == FM_PAGE_BROWSER)
        fm_browser_reload (shell->browser, name);
    }
  else if (shell)
    {
      g_warning ("Could not rename: %s", error->message);
      fm_shell_toast (shell, _("Failed to rename"));
    }
  g_object_unref (window);
}

static void
on_rename_done (const char *text, gpointer data)
{
  RenameRequest *request = data;
  const char *original = request->item->edit_name ? request->item->edit_name : request->item->name;

  if (text && g_strcmp0 (text, original))
    {
      g_file_set_display_name_async (request->file, text, G_PRIORITY_DEFAULT, NULL, on_renamed,
                                     g_object_ref (request->shell->window));
      end_selection (request->shell);
    }
  rename_request_free (request);
}

/* Batch Rename: each item's new name from the pattern, its stem for %% and its place in the selection for ## */
typedef struct {
  FmShell *shell;
  GPtrArray *items;             /* FmItem */
} BatchRequest;

/* the items' new names, in their order */
static GStrv
batch_names (const char *pattern, const char *extension, gpointer data)
{
  BatchRequest *request = data;
  g_autoptr(GStrvBuilder) builder = g_strv_builder_new ();

  for (guint i = 0; i < request->items->len; i++)
    {
      FmItem *item = request->items->pdata[i];
      const char *name = item->edit_name ? item->edit_name : item->name;
      gboolean folder = fm_item_is_folder (item);
      g_autofree char *stem = NULL, *renamed = NULL;
      int end = -1;

      fm_stem_range (name, folder, &end);
      stem = g_utf8_substring (name, 0, end);
      renamed = fm_batch_name (pattern, stem, i + 1);
      /* a file takes the extension typed; one left empty, or a folder, keeps its own */
      if (*extension && !folder)
        g_strv_builder_take (builder, g_strconcat (renamed, ".", extension, NULL));
      else
        g_strv_builder_take (builder, g_strconcat (renamed, g_utf8_offset_to_pointer (name, end), NULL));
    }
  return g_strv_builder_end (builder);
}

static void
on_batch_done (const char *pattern, const char *extension, gpointer data)
{
  BatchRequest *request = data;

  if (pattern)
    {
      g_autoptr(GPtrArray) files = locations_of (request->items);
      g_auto(GStrv) names = batch_names (pattern, extension, request);

      fm_jobs_rename (request->shell, files, names);
      end_selection (request->shell);
    }
  g_ptr_array_unref (request->items);
  g_free (request);
}

static void
batch_rename (FmShell *shell, GPtrArray *items)
{
  BatchRequest *request = g_new0 (BatchRequest, 1);
  g_autofree const char **names = g_new0 (const char *, items->len + 1);
  g_autofree char *common = NULL;
  gboolean mixed = FALSE;

  request->shell = shell;
  request->items = g_ptr_array_ref (items);
  /* the extension field starts with the one all the files share, empty when they differ or a folder is there */
  for (guint i = 0; i < items->len; i++)
    {
      FmItem *item = items->pdata[i];
      const char *name = item->edit_name ? item->edit_name : item->name;
      int end = -1;
      const char *extension;

      names[i] = name;
      if (fm_item_is_folder (item))
        {
          mixed = TRUE;
          continue;
        }
      fm_stem_range (name, FALSE, &end);
      extension = g_utf8_offset_to_pointer (name, end);
      extension = *extension == '.' ? extension + 1 : extension;
      if (!common)
        common = g_strdup (extension);
      else if (strcmp (common, extension))
        mixed = TRUE;
    }
  fm_batch_rename_dialog (shell->window, names, items->len, mixed || !common ? "" : common, batch_names, on_batch_done,
                          request);
}

static void
rename_selected (FmShell *shell)
{
  g_autoptr(GPtrArray) items = selected_items (shell);
  RenameRequest *request;
  const char *name;
  int end = -1;

  if (items->len > 1)
    {
      batch_rename (shell, items);
      return;
    }
  if (items->len != 1)
    return;
  request = g_new0 (RenameRequest, 1);
  request->shell = shell;
  request->item = g_object_ref (items->pdata[0]);
  request->file = fm_item_get_location (request->item);
  name = request->item->edit_name ? request->item->edit_name : request->item->name;
  fm_stem_range (name, fm_item_is_folder (request->item), &end);
  fm_text_dialog (shell->window, _("Rename"), name, end, check_rename, on_rename_done, request);
}

/* Open with: the apps for the file's type */
typedef struct {
  GtkWidget *window;
  GList *apps;
  GFile *file;
} OpenWith;

static void
on_open_with (int index, gpointer data)
{
  OpenWith *open_with = data;
  GAppInfo *app = index >= 0 ? g_list_nth_data (open_with->apps, index) : NULL;

  if (app)
    {
      GList files = { open_with->file, NULL, NULL };

      launch_files (open_with->window, app, &files);
    }
  g_list_free_full (open_with->apps, g_object_unref);
  g_object_unref (open_with->file);
  g_object_unref (open_with->window);
  g_free (open_with);
}

static void
open_with (FmShell *shell, FmItem *item)
{
  OpenWith *request;
  g_autofree FmListRow *rows = NULL;
  guint n;

  if (!item->content_type)
    {
      fm_shell_toast (shell, _("No apps are installed to open this type of file"));
      return;
    }
  request = g_new0 (OpenWith, 1);
  request->window = g_object_ref (shell->window);
  request->file = fm_item_get_location (item);
  request->apps = g_app_info_get_all_for_type (item->content_type);
  n = g_list_length (request->apps);
  if (!n)
    {
      fm_shell_toast (shell, _("No apps are installed to open this type of file"));
      on_open_with (-1, request);
      return;
    }
  rows = g_new0 (FmListRow, n);
  for (guint i = 0; i < n; i++)
    {
      GAppInfo *app = g_list_nth_data (request->apps, i);

      rows[i].glyph = FM_GLYPH_FILE;
      rows[i].icon = g_app_info_get_icon (app);
      rows[i].label = g_app_info_get_display_name (app);
    }
  fm_list_dialog (shell->window, _("Open with"), rows, (int) n, on_open_with, request);
}

static void
add_bookmark (FmShell *shell, FmItem *item)
{
  g_autoptr(GFile) location = fm_item_get_location (item);
  g_autofree char *uri = g_file_get_uri (location);

  if (fm_state_add_bookmark (shell->state, uri, item->name))
    {
      fm_shell_toast (shell, _("Bookmark created successfully"));
      fm_drawer_refresh (shell->drawer);
    }
  else
    fm_shell_toast (shell, _("This folder is already bookmarked"));
}

/* Compress: the name of the .zip, made in the folder of the first item */
typedef struct {
  FmShell *shell;
  GPtrArray *files;             /* GFile */
  GFile *folder;
  gboolean here;                /* the folder is the one listed: its names are known without asking */
} CompressRequest;

static const char *
check_compress (const char *text, gpointer data)
{
  CompressRequest *request = data;
  FmNameCheck check = fm_check_name (text);
  g_autofree char *name = NULL;

  if (check != FM_NAME_OK)
    return fm_name_check_message (check);
  name = g_strconcat (text, ".zip", NULL);
  if (request->here && fm_browser_has_name (request->shell->browser, name))
    return _("File or folder already exists");
  return NULL;
}

static void
on_compress_done (const char *text, gpointer data)
{
  CompressRequest *request = data;

  if (text)
    {
      g_autofree char *name = g_strconcat (text, ".zip", NULL);
      g_autoptr(GFile) target = g_file_get_child_for_display_name (request->folder, name, NULL);

      if (target)
        fm_jobs_compress (request->shell, request->files, target);
      end_selection (request->shell);
    }
  g_ptr_array_unref (request->files);
  g_object_unref (request->folder);
  g_free (request);
}

static void
compress_selected (FmShell *shell, GPtrArray *items)
{
  CompressRequest *request;
  FmItem *first = items->pdata[0];
  const char *name = first->edit_name ? first->edit_name : first->name;
  GFile *listed = fm_browser_get_location (shell->browser);
  g_autoptr(GFile) location = fm_item_get_location (first);
  g_autofree char *stem = NULL;
  int end = -1;

  request = g_new0 (CompressRequest, 1);
  request->shell = shell;
  request->files = locations_of (items);
  request->folder = fm_parent (location);
  if (!request->folder)
    {
      fm_shell_toast (shell, _("Failed to compress"));
      g_ptr_array_unref (request->files);
      g_free (request);
      return;
    }
  request->here = listed && !fm_browser_is_virtual (shell->browser) && g_file_equal (listed, request->folder);
  /* the first item's name, a file's without its extension */
  fm_stem_range (name, fm_item_is_folder (first), &end);
  stem = g_utf8_substring (name, 0, end);
  fm_text_dialog_suffix (shell->window, _("Compress"), stem, -1, ".zip", check_compress, on_compress_done, request);
}

typedef enum {
  MORE_SHARE,
  MORE_BOOKMARK,
  MORE_COMPRESS,
  MORE_OPEN_WITH,
  MORE_PROPERTIES,
  MORE_SETTINGS,
} MoreChoice;

#define MORE_MAX 6

/* More: Share, Add Bookmark (a folder), Compress, Open with (a file), Properties, Settings, as File Manager+'s. A
 * bookmark is a place to go to, so a file gets none. The popup's scrim keeps the selection as it is while it shows,
 * so the choice is found again from it. */
static int
more_choices (FmShell *shell, GPtrArray *items, MoreChoice *choices, const char **labels)
{
  g_autoptr(GPtrArray) files = locations_of (items);
  int n = 0;

  if (fm_share_possible (files))
    {
      choices[n] = MORE_SHARE;
      labels[n++] = _("Share");
    }
  if (items->len == 1 && fm_item_is_folder (items->pdata[0]))
    {
      choices[n] = MORE_BOOKMARK;
      labels[n++] = _("Add Bookmark");
    }
  choices[n] = MORE_COMPRESS;
  labels[n++] = _("Compress");
  if (items->len == 1 && !fm_item_is_folder (items->pdata[0]))
    {
      choices[n] = MORE_OPEN_WITH;
      labels[n++] = _("Open with");
    }
  choices[n] = MORE_PROPERTIES;
  labels[n++] = _("Properties");
  choices[n] = MORE_SETTINGS;
  labels[n++] = _("Settings");
  return n;
}

static void
on_more (int index, gpointer data)
{
  FmShell *shell = data;
  g_autoptr(GPtrArray) items = selected_items (shell);
  g_autoptr(GPtrArray) files = locations_of (items);
  MoreChoice choices[MORE_MAX];
  const char *labels[MORE_MAX];
  int n;

  if (!items->len)
    return;
  n = more_choices (shell, items, choices, labels);
  if (index < 0 || index >= n)
    return;
  switch (choices[index])
    {
    case MORE_SHARE:
      fm_share_files (shell->window, files);
      end_selection (shell);
      break;
    case MORE_BOOKMARK:
      add_bookmark (shell, items->pdata[0]);
      end_selection (shell);
      break;
    case MORE_OPEN_WITH:
      open_with (shell, items->pdata[0]);
      end_selection (shell);
      break;
    case MORE_COMPRESS:
      compress_selected (shell, items);
      break;
    case MORE_PROPERTIES:
      fm_properties_dialog (shell->window, items);
      break;
    case MORE_SETTINGS:
      end_selection (shell);
      fm_shell_show_page (shell, FM_PAGE_SETTINGS);
      break;
    }
}

static void
show_more (FmShell *shell, GtkWidget *anchor)
{
  g_autoptr(GPtrArray) items = selected_items (shell);
  MoreChoice choices[MORE_MAX];
  const char *labels[MORE_MAX];
  int n;

  if (!items->len)
    return;
  n = more_choices (shell, items, choices, labels);
  /* above the bar, its bottom on the bar's top and its end at the item's; the menu is 48 px a row and 8 px at each
   * end */
  fm_popup_show (anchor, labels, n, FM_POPUP_AT, gtk_widget_get_width (anchor) - 196, -(double) (n * 48 + 16),
                 on_more, shell);
}

/* ---- the toolbar's actions ----------------------------------------------------------------------------------------- */

static void
on_nav (VibeosPressable *button, FmShell *shell)
{
  fm_drawer_open (shell->drawer, TRUE);
}

static void
on_back_button (VibeosPressable *button, FmShell *shell)
{
  fm_shell_back (shell);
}

static void
on_search (VibeosPressable *button, FmShell *shell)
{
  fm_browser_search_begin (shell->browser);
  fm_shell_update_bars (shell);
  gtk_widget_grab_focus (shell->search_entry);
}

static void
on_search_changed (GtkEditable *entry, FmShell *shell)
{
  fm_browser_search_filter (shell->browser, gtk_editable_get_text (entry));
}

static void
on_search_activate (GtkEntry *entry, FmShell *shell)
{
  fm_browser_search_run (shell->browser, gtk_editable_get_text (GTK_EDITABLE (entry)));
}

static void
on_view (VibeosPressable *button, FmShell *shell)
{
  fm_view_dialog (shell->window, shell->settings);
}

static void
on_empty_answer (gboolean ok, gpointer data)
{
  if (ok)
    fm_jobs_empty_trash (data);
}

static void
on_empty (VibeosPressable *button, FmShell *shell)
{
  fm_confirm_dialog (shell->window, _("Empty Recycle Bin"), _("Are you sure you want to empty the Recycle Bin?"),
                     on_empty_answer, shell);
}

static void
on_edit (VibeosPressable *button, FmShell *shell)
{
  fm_remote_page_toggle_edit (shell->remote_page);
}

static void
on_add (VibeosPressable *button, FmShell *shell)
{
  fm_remote_page_add (shell->remote_page);
}

/* New folder: Create Folder, then the folder scrolled to */
static const char *
check_new_folder (const char *text, gpointer data)
{
  FmShell *shell = data;
  FmNameCheck check = fm_check_name (text);

  if (check == FM_NAME_EMPTY)
    return _("Folder name cannot be empty.");
  if (check != FM_NAME_OK)
    return fm_name_check_message (check);
  if (fm_browser_has_name (shell->browser, text))
    return _("File or folder already exists");
  return NULL;
}

static void
on_folder_made (GObject *source, GAsyncResult *result, gpointer data)
{
  GtkWidget *window = data;
  FmShell *shell = g_object_get_data (G_OBJECT (window), "fm-shell");
  g_autoptr(GError) error = NULL;

  if (g_file_make_directory_finish (G_FILE (source), result, &error))
    {
      g_autofree char *name = g_file_get_basename (G_FILE (source));

      if (shell && shell->page == FM_PAGE_BROWSER)
        fm_browser_reload (shell->browser, name);
    }
  else if (shell)
    {
      g_warning ("Could not create the folder: %s", error->message);
      fm_shell_toast (shell, _("Failed to create a folder"));
    }
  g_object_unref (window);
}

static void
on_new_folder_done (const char *text, gpointer data)
{
  FmShell *shell = data;
  GFile *location = fm_browser_get_location (shell->browser);
  g_autoptr(GFile) folder = NULL;

  if (!text || !location)
    return;
  folder = g_file_get_child_for_display_name (location, text, NULL);
  if (!folder)
    folder = g_file_get_child (location, text);
  g_file_make_directory_async (folder, G_PRIORITY_DEFAULT, NULL, on_folder_made, g_object_ref (shell->window));
}

static void
new_folder (FmShell *shell)
{
  if (shell->page != FM_PAGE_BROWSER || fm_browser_is_virtual (shell->browser))
    return;
  fm_text_dialog (shell->window, _("Create Folder"), _("New folder"), -1, check_new_folder, on_new_folder_done,
                  shell);
}

typedef enum {
  OVERFLOW_NEW_FOLDER,
  OVERFLOW_REFRESH,
  OVERFLOW_SETTINGS,
  OVERFLOW_ABOUT,
} OverflowChoice;

/* About: VibeOS's About sheet, as every VibeOS app has it */
static const VibeosAboutInfo fm_about = { .name = "Files", .icon_name = "vibeos.Files" };

static void
on_overflow_chosen (int index, gpointer data)
{
  FmShell *shell = data;
  OverflowChoice choices[4];
  int n = 0;

  /* the same list on_overflow made */
  if (shell->page == FM_PAGE_BROWSER && !fm_browser_is_virtual (shell->browser))
    choices[n++] = OVERFLOW_NEW_FOLDER;
  choices[n++] = OVERFLOW_REFRESH;
  if (shell->page == FM_PAGE_HOME)
    {
      choices[n++] = OVERFLOW_SETTINGS;
      choices[n++] = OVERFLOW_ABOUT;
    }
  if (index < 0 || index >= n)
    return;
  switch (choices[index])
    {
    case OVERFLOW_NEW_FOLDER:
      new_folder (shell);
      break;
    case OVERFLOW_REFRESH:
      fm_shell_reload (shell);
      break;
    case OVERFLOW_SETTINGS:
      fm_shell_show_page (shell, FM_PAGE_SETTINGS);
      break;
    case OVERFLOW_ABOUT:
      vibeos_about_present (shell->window, &fm_about);
      break;
    }
}

/* the overflow: Home has Refresh, Settings and About; a folder New folder and Refresh; the Recycle Bin Refresh */
static void
on_overflow (VibeosPressable *button, FmShell *shell)
{
  const char *labels[4];
  int n = 0;

  if (shell->page == FM_PAGE_BROWSER && !fm_browser_is_virtual (shell->browser))
    labels[n++] = _("New folder");
  labels[n++] = _("Refresh");
  if (shell->page == FM_PAGE_HOME)
    {
      labels[n++] = _("Settings");
      labels[n++] = _("About");
    }
  fm_popup_show (GTK_WIDGET (button), labels, n, FM_POPUP_OVER_END, 0, 0, on_overflow_chosen, shell);
}

static void
on_select_close (VibeosPressable *button, FmShell *shell)
{
  end_selection (shell);
}

static void
on_select_all (VibeosPressable *button, FmShell *shell)
{
  fm_list_select_all (fm_browser_get_list (shell->browser));
  fm_shell_update_bars (shell);
}

static void
on_select_range (VibeosPressable *button, FmShell *shell)
{
  fm_list_select_range (fm_browser_get_list (shell->browser));
  fm_shell_update_bars (shell);
}

/* ---- the bottom bar ------------------------------------------------------------------------------------------------ */

typedef enum {
  BOTTOM_COPY,
  BOTTOM_CUT,
  BOTTOM_DELETE,
  BOTTOM_RENAME,
  BOTTOM_MORE,
  BOTTOM_RESTORE,
  BOTTOM_CANCEL,
  BOTTOM_PASTE,
} BottomItem;

static void
on_bottom_item (VibeosPressable *button, FmShell *shell)
{
  BottomItem item = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "fm-item"));

  switch (item)
    {
    case BOTTOM_COPY:
      take (shell, FALSE);
      break;
    case BOTTOM_CUT:
      take (shell, TRUE);
      break;
    case BOTTOM_DELETE:
      delete_selected (shell);
      break;
    case BOTTOM_RENAME:
      rename_selected (shell);
      break;
    case BOTTOM_MORE:
      show_more (shell, GTK_WIDGET (button));
      break;
    case BOTTOM_RESTORE:
      restore_selected (shell);
      break;
    case BOTTOM_CANCEL:
      fm_shell_set_clip (shell, NULL, FALSE);
      break;
    case BOTTOM_PASTE:
      {
        GFile *dest = fm_browser_get_location (shell->browser);
        g_autoptr(GPtrArray) clip = shell->clip ? g_ptr_array_ref (shell->clip) : NULL;

        if (!clip || !dest || shell->page != FM_PAGE_BROWSER || fm_browser_is_virtual (shell->browser))
          break;
        fm_jobs_copy (shell, clip, dest, shell->clip_cut);
        fm_shell_set_clip (shell, NULL, FALSE);
        break;
      }
    }
}

/* a bottom bar item: the glyph over the label */
static GtkWidget *
bottom_item (FmShell *shell, BottomItem item, const char *icon, const char *label, gboolean sensitive)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *image = gtk_image_new_from_icon_name (icon);
  GtkWidget *text = fm_label (label, "fm-bottom-text", 14, FALSE);
  GtkWidget *button;

  gtk_image_set_pixel_size (GTK_IMAGE (image), 24);
  gtk_widget_add_css_class (image, "fm-bottom-icon");
  gtk_label_set_xalign (GTK_LABEL (text), 0.5f);
  gtk_box_append (GTK_BOX (box), image);
  gtk_box_append (GTK_BOX (box), text);
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  button = vibeos_pressable_new (box);
  gtk_widget_add_css_class (button, "fm-bottom-item");
  gtk_widget_set_hexpand (button, TRUE);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (button), VIBEOS_RIPPLE_BOUNDED, 0);
  gtk_widget_set_sensitive (button, sensitive);
  g_object_set_data (G_OBJECT (button), "fm-item", GINT_TO_POINTER (item));
  g_signal_connect (button, "clicked", G_CALLBACK (on_bottom_item), shell);
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  gtk_box_append (GTK_BOX (shell->bottom_items), button);
  return button;
}

static void
update_bottom (FmShell *shell, gboolean selecting, guint selected)
{
  Shell *s = SHELL (shell);
  gboolean trash = fm_browser_is_trash (shell->browser);
  gboolean can_paste = shell->page == FM_PAGE_BROWSER && !fm_browser_is_virtual (shell->browser)
                       && fm_browser_get_location (shell->browser);
  g_autofree char *mode = NULL;
  GtkWidget *child;

  if (selecting)
    mode = g_strdup_printf ("select:%d", trash);
  else if (shell->clip)
    mode = g_strdup_printf ("paste:%d", can_paste);
  gtk_widget_set_visible (shell->bottom, mode != NULL);
  if (!g_strcmp0 (mode, s->bottom_mode))
    return;
  g_free (s->bottom_mode);
  s->bottom_mode = g_strdup (mode);
  while ((child = gtk_widget_get_first_child (shell->bottom_items)))
    gtk_box_remove (GTK_BOX (shell->bottom_items), child);
  if (!mode)
    return;
  if (selecting && trash)
    {
      bottom_item (shell, BOTTOM_RESTORE, "edit-undo-symbolic", _("Restore"), TRUE);
      bottom_item (shell, BOTTOM_DELETE, "edit-delete-symbolic", _("Delete"), TRUE);
    }
  else if (selecting)
    {
      /* File Manager+'s action bar: Rename with several selected is Batch Rename */
      bottom_item (shell, BOTTOM_COPY, "edit-copy-symbolic", _("Copy"), TRUE);
      bottom_item (shell, BOTTOM_CUT, "edit-cut-symbolic", _("Move"), TRUE);
      bottom_item (shell, BOTTOM_RENAME, "insert-text-symbolic", _("Rename"), TRUE);
      bottom_item (shell, BOTTOM_DELETE, "user-trash-symbolic", _("Delete"), TRUE);
      bottom_item (shell, BOTTOM_MORE, "view-more-symbolic", _("More"), TRUE);
    }
  else
    {
      bottom_item (shell, BOTTOM_CANCEL, "window-close-symbolic", _("Cancel"), TRUE);
      bottom_item (shell, BOTTOM_PASTE, "edit-paste-symbolic", _("Paste"), can_paste);
    }
}

/* ---- the bars ------------------------------------------------------------------------------------------------------ */

static gboolean
has_back (FmShell *shell)
{
  FmList *list = fm_browser_get_list (shell->browser);

  return fm_drawer_is_open (shell->drawer) || shell->back->len > 0
         || (shell->page == FM_PAGE_BROWSER && (fm_list_get_selecting (list)
                                                || fm_browser_is_searching (shell->browser)))
         || (shell->page == FM_PAGE_REMOTE && fm_remote_page_is_editing (shell->remote_page));
}

static const char *
page_title (FmShell *shell)
{
  FmPlace *place;

  switch (shell->page)
    {
    case FM_PAGE_REMOTE:
      return _("Remote");
    case FM_PAGE_SETTINGS:
      return _("Settings");
    case FM_PAGE_BROWSER:
      place = fm_browser_get_place (shell->browser);
      return place ? place->name : "";
    case FM_PAGE_HOME:
    default:
      return _("Files");
    }
}

void
fm_shell_update_bars (FmShell *shell)
{
  Shell *s = SHELL (shell);
  FmList *list = fm_browser_get_list (shell->browser);
  gboolean browsing = shell->page == FM_PAGE_BROWSER;
  gboolean selecting = browsing && fm_list_get_selecting (list);
  gboolean searching = browsing && fm_browser_is_searching (shell->browser);
  gboolean trash = browsing && fm_browser_is_trash (shell->browser);
  FmPlace *place = fm_browser_get_place (shell->browser);
  gboolean network = browsing && place && place->kind == FM_PLACE_NETWORK;
  guint selected = selecting ? fm_list_count_selected (list) : 0;
  const char *mode = selecting ? "select" : searching ? "search" : shell->page == FM_PAGE_SETTINGS ? "settings"
                                                                                                   : "normal";

  gtk_stack_set_visible_child_name (GTK_STACK (shell->toolbar), mode);
  gtk_widget_remove_css_class (shell->toolbar, "fm-cab");
  if (selecting)
    {
      g_autofree char *title = g_strdup_printf ("%u/%u", selected, fm_list_count_items (list));

      gtk_widget_add_css_class (shell->toolbar, "fm-cab");
      gtk_label_set_text (GTK_LABEL (shell->select_title), title);
    }
  gtk_label_set_text (GTK_LABEL (shell->title), page_title (shell));
  if (shell->drawer)
    fm_drawer_sync_rail (shell->drawer);
  gtk_window_set_title (GTK_WINDOW (shell->window), shell->page == FM_PAGE_HOME ? _("Files") : page_title (shell));

  gtk_widget_set_visible (s->search, browsing && !trash && !network);
  gtk_widget_set_visible (s->view, browsing && !trash);
  /* the View button shows how the folder is shown: rows or tiles */
  gtk_image_set_from_icon_name (GTK_IMAGE (vibeos_pressable_get_child (VIBEOS_PRESSABLE (s->view))),
                                fm_list_get_grid (list) ? "view-grid-symbolic" : "view-list-symbolic");
  gtk_widget_set_visible (s->empty, trash);
  gtk_widget_set_visible (s->edit, shell->page == FM_PAGE_REMOTE && shell->remotes->len > 0);
  gtk_image_set_from_icon_name (GTK_IMAGE (vibeos_pressable_get_child (VIBEOS_PRESSABLE (s->edit))),
                                fm_remote_page_is_editing (shell->remote_page) ? "object-select-symbolic"
                                                                               : "document-edit-symbolic");
  gtk_widget_set_visible (s->add, shell->page == FM_PAGE_REMOTE);
  gtk_widget_set_visible (s->overflow, shell->page == FM_PAGE_HOME || browsing);

  gtk_widget_set_visible (shell->search_spinner, searching && fm_browser_search_busy (shell->browser));
  gtk_spinner_set_spinning (GTK_SPINNER (shell->search_spinner), searching && fm_browser_search_busy (shell->browser));

  update_bottom (shell, selecting, selected);
  gtk_widget_set_visible (s->back_marker, has_back (shell));
}

/* ---- Back ---------------------------------------------------------------------------------------------------------- */

gboolean
fm_shell_back (FmShell *shell)
{
  FmList *list = fm_browser_get_list (shell->browser);
  BackEntry *entry;

  if (fm_drawer_is_open (shell->drawer))
    {
      fm_drawer_open (shell->drawer, FALSE);
      fm_shell_update_bars (shell);
      return TRUE;
    }
  if (shell->page == FM_PAGE_BROWSER && fm_list_get_selecting (list))
    {
      end_selection (shell);
      return TRUE;
    }
  if (shell->page == FM_PAGE_BROWSER && fm_browser_is_searching (shell->browser))
    {
      Shell *s = SHELL (shell);

      fm_browser_search_end (shell->browser);
      g_signal_handler_block (shell->search_entry, s->search_changed);
      gtk_editable_set_text (GTK_EDITABLE (shell->search_entry), "");
      g_signal_handler_unblock (shell->search_entry, s->search_changed);
      fm_shell_update_bars (shell);
      return TRUE;
    }
  if (shell->page == FM_PAGE_REMOTE && fm_remote_page_is_editing (shell->remote_page))
    {
      fm_remote_page_toggle_edit (shell->remote_page);
      return TRUE;
    }
  if (!shell->back->len)
    return FALSE;
  entry = g_ptr_array_steal_index (shell->back, shell->back->len - 1);
  if (entry->page == FM_PAGE_BROWSER && entry->location)
    browse (shell, entry->location);
  else
    {
      if (entry->page == FM_PAGE_HOME)
        fm_home_refresh (shell->home);
      else if (entry->page == FM_PAGE_REMOTE)
        fm_remote_page_refresh (shell->remote_page);
      set_page (shell, entry->page);
    }
  back_entry_free (entry);
  return TRUE;
}

static gboolean
on_escape (GtkWidget *widget, GVariant *args, gpointer data)
{
  FmShell *shell = data;

  if (fm_popup_dismiss (shell->window))
    return TRUE;
  if (!fm_shell_back (shell))
    gtk_window_close (GTK_WINDOW (shell->window));
  return TRUE;
}

/* ---- the rest ------------------------------------------------------------------------------------------------------ */

void
fm_shell_reload (FmShell *shell)
{
  if (shell->page == FM_PAGE_BROWSER)
    fm_browser_reload (shell->browser, NULL);
  else if (shell->page == FM_PAGE_HOME)
    fm_home_refresh (shell->home);
  if (fm_drawer_is_open (shell->drawer) || fm_drawer_is_docked (shell->drawer))
    fm_drawer_refresh (shell->drawer);
  fm_shell_update_bars (shell);
}

static void
on_mounts_changed (GVolumeMonitor *monitor, GMount *mount, FmShell *shell)
{
  if (shell->page == FM_PAGE_HOME)
    fm_home_refresh (shell->home);
  if (fm_drawer_is_open (shell->drawer) || fm_drawer_is_docked (shell->drawer))
    fm_drawer_refresh (shell->drawer);
}

static void
on_mount_removed (GVolumeMonitor *monitor, GMount *mount, FmShell *shell)
{
  g_autoptr(GFile) root = g_mount_get_root (mount);
  GFile *location = fm_browser_get_location (shell->browser);

  /* a medium taken out under the folder that shows: Home */
  if (shell->page == FM_PAGE_BROWSER && location && fm_file_is_under (location, root) && g_file_is_native (root))
    fm_shell_show_page (shell, FM_PAGE_HOME);
  on_mounts_changed (monitor, mount, shell);
}

static void
on_settings_changed (GSettings *settings, const char *key, FmShell *shell)
{
  if (!g_strcmp0 (key, "show-hidden") || !g_strcmp0 (key, "sort-order") || !g_strcmp0 (key, "show-thumbnails")
      || !g_strcmp0 (key, "view-type"))
    fm_browser_settings_changed (shell->browser);
  if (!g_strcmp0 (key, "view-type"))
    fm_shell_update_bars (shell);
  else if (!g_strcmp0 (key, "show-system"))
    fm_drawer_refresh (shell->drawer);
}

static void
on_list_changed (FmList *list, FmShell *shell)
{
  fm_shell_update_bars (shell);
}

static void
shell_free (gpointer data)
{
  Shell *s = data;
  FmShell *shell = &s->pub;

  snack_clear (s);
  g_clear_handle_id (&s->toast_timeout, g_source_remove);
  g_clear_pointer (&s->toast_anim, vibeos_anim_free);
  g_signal_handlers_disconnect_by_data (shell->monitor, shell);
  g_signal_handlers_disconnect_by_data (shell->settings, shell);
  if (s->layout)
    g_signal_handlers_disconnect_by_data (s->layout, shell);
  g_clear_object (&s->layout);
  fm_browser_free (shell->browser);
  fm_home_free (shell->home);
  fm_remote_page_free (shell->remote_page);
  fm_drawer_free (shell->drawer);
  g_clear_object (&shell->monitor);
  g_clear_object (&shell->settings);
  fm_state_free (shell->state);
  g_ptr_array_unref (shell->remotes);
  g_free (shell->remotes_path);
  g_clear_pointer (&shell->clip, g_ptr_array_unref);
  g_ptr_array_unref (shell->back);
  g_free (s->bottom_mode);
  g_free (s);
}

static GtkWidget *
title_label (const char *text)
{
  GtkWidget *label = fm_label (text, "fm-title", 18, FALSE);

  gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_set_margin_start (label, 16);
  return label;
}

static GtkWidget *
toolbar_new (Shell *s)
{
  FmShell *shell = &s->pub;
  GtkWidget *stack = gtk_stack_new ();
  GtkWidget *normal = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *settings = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *search = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *select = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *button;

  gtk_widget_add_css_class (stack, "fm-toolbar");
  gtk_stack_set_hhomogeneous (GTK_STACK (stack), TRUE);
  gtk_stack_set_vhomogeneous (GTK_STACK (stack), TRUE);

  /* normal: ☰, the title, the actions and the overflow */
  shell->nav = fm_icon_button ("open-menu-symbolic", _("Navigation menu"), 56, 56);
  g_signal_connect (shell->nav, "clicked", G_CALLBACK (on_nav), shell);
  gtk_box_append (GTK_BOX (normal), shell->nav);
  shell->title = title_label (_("Files"));
  gtk_box_append (GTK_BOX (normal), shell->title);
  shell->actions = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  s->search = fm_icon_button ("system-search-symbolic", _("Search"), 48, 56);
  g_signal_connect (s->search, "clicked", G_CALLBACK (on_search), shell);
  s->view = fm_icon_button ("view-list-symbolic", _("View"), 48, 56);
  g_signal_connect (s->view, "clicked", G_CALLBACK (on_view), shell);
  s->empty = fm_icon_button ("edit-delete-symbolic", _("Empty Recycle Bin"), 48, 56);
  g_signal_connect (s->empty, "clicked", G_CALLBACK (on_empty), shell);
  s->edit = fm_icon_button ("document-edit-symbolic", _("Edit"), 48, 56);
  g_signal_connect (s->edit, "clicked", G_CALLBACK (on_edit), shell);
  /* the theme's list-add drawn larger than the other glyphs, as its + fills less of its box */
  s->add = fm_icon_button_sized ("list-add-symbolic", _("New location"), 48, 56, 20);
  g_signal_connect (s->add, "clicked", G_CALLBACK (on_add), shell);
  s->overflow = fm_icon_button ("view-more-symbolic", _("More options"), 36, 56);
  gtk_widget_set_margin_end (s->overflow, 4);
  g_signal_connect (s->overflow, "clicked", G_CALLBACK (on_overflow), shell);
  gtk_box_append (GTK_BOX (shell->actions), s->search);
  gtk_box_append (GTK_BOX (shell->actions), s->view);
  gtk_box_append (GTK_BOX (shell->actions), s->empty);
  gtk_box_append (GTK_BOX (shell->actions), s->edit);
  gtk_box_append (GTK_BOX (shell->actions), s->add);
  gtk_box_append (GTK_BOX (shell->actions), s->overflow);
  gtk_box_append (GTK_BOX (normal), shell->actions);
  gtk_stack_add_named (GTK_STACK (stack), normal, "normal");

  /* Settings: ← and the title */
  button = fm_icon_button ("go-previous-symbolic", _("Back"), 56, 56);
  g_signal_connect (button, "clicked", G_CALLBACK (on_back_button), shell);
  gtk_box_append (GTK_BOX (settings), button);
  gtk_box_append (GTK_BOX (settings), title_label (_("Settings")));
  gtk_stack_add_named (GTK_STACK (stack), settings, "settings");

  /* search: ←, the field with its hint, the spinner while the folders are searched */
  button = fm_icon_button ("go-previous-symbolic", _("Back"), 56, 56);
  g_signal_connect (button, "clicked", G_CALLBACK (on_back_button), shell);
  gtk_box_append (GTK_BOX (search), button);
  shell->search_entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (shell->search_entry), _("Search"));
  gtk_widget_add_css_class (shell->search_entry, "fm-search-entry");
  gtk_widget_set_hexpand (shell->search_entry, TRUE);
  gtk_widget_set_valign (shell->search_entry, GTK_ALIGN_CENTER);
  s->search_changed = g_signal_connect (shell->search_entry, "changed", G_CALLBACK (on_search_changed), shell);
  g_signal_connect (shell->search_entry, "activate", G_CALLBACK (on_search_activate), shell);
  gtk_box_append (GTK_BOX (search), shell->search_entry);
  shell->search_spinner = gtk_spinner_new ();
  gtk_widget_set_size_request (shell->search_spinner, 30, 30);
  gtk_widget_set_valign (shell->search_spinner, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (shell->search_spinner, "fm-search-spinner");
  gtk_box_append (GTK_BOX (search), shell->search_spinner);
  gtk_stack_add_named (GTK_STACK (stack), search, "search");

  /* selection: ✕, "N selected", Select All, Select a range */
  button = fm_icon_button ("window-close-symbolic", _("Done"), 56, 56);
  g_signal_connect (button, "clicked", G_CALLBACK (on_select_close), shell);
  gtk_box_append (GTK_BOX (select), button);
  shell->select_title = title_label (NULL);
  gtk_box_append (GTK_BOX (select), shell->select_title);
  shell->select_actions = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  button = fm_icon_button ("edit-select-all-symbolic", _("Select All"), 48, 56);
  g_signal_connect (button, "clicked", G_CALLBACK (on_select_all), shell);
  gtk_box_append (GTK_BOX (shell->select_actions), button);
  button = fm_icon_button ("selection-mode-symbolic", _("Select a range"), 48, 56);
  g_signal_connect (button, "clicked", G_CALLBACK (on_select_range), shell);
  gtk_box_append (GTK_BOX (shell->select_actions), button);
  gtk_box_append (GTK_BOX (select), shell->select_actions);
  gtk_stack_add_named (GTK_STACK (stack), select, "select");
  return stack;
}

/* a tablet held sideways: the drawer docked at the window's start as a sidebar; held upright: the rail of the places
 * there, with ☰ on it; either way the toolbar has no ☰ */
static void
on_layout_changed (VibeosLayout *layout, FmShell *shell)
{
  gboolean tablet = vibeos_layout_is_tablet (layout);
  gboolean sideways = tablet && vibeos_layout_is_landscape (layout);

  fm_drawer_set_docked (shell->drawer, SHELL (shell)->row, sideways);
  gtk_widget_set_visible (fm_drawer_get_rail (shell->drawer), tablet && !sideways);
  gtk_widget_set_visible (shell->nav, !tablet);
}

GtkWidget *
fm_window_new (GtkApplication *app, GSettings *settings)
{
  Shell *s = g_new0 (Shell, 1);
  FmShell *shell = &s->pub;
  GtkWidget *window = vibeos_window_new (app);
  GtkWidget *main = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *content = gtk_overlay_new ();
  GtkEventController *shortcuts = gtk_shortcut_controller_new ();
  g_autofree char *config_dir = g_build_filename (g_get_user_config_dir (), "vibeos-files", NULL);
  g_autofree char *state_dir = g_build_filename (g_get_user_state_dir (), "vibeos-files", NULL);
  GtkOverlay *overlay;

  shell->window = window;
  shell->settings = g_object_ref (settings);
  shell->remotes_path = g_build_filename (config_dir, "remotes.ini", NULL);
  shell->remotes = fm_remotes_load (shell->remotes_path);
  shell->state = fm_state_new (config_dir, state_dir);
  shell->monitor = g_volume_monitor_get ();
  shell->back = g_ptr_array_new_with_free_func (back_entry_free);
  g_object_set_data_full (G_OBJECT (window), "fm-shell", shell, shell_free);
  gtk_window_set_title (GTK_WINDOW (window), _("Files"));
  gtk_widget_add_css_class (window, "fm-window");

  shell->toolbar = toolbar_new (s);
  gtk_box_append (GTK_BOX (main), shell->toolbar);

  shell->browser = fm_browser_new (shell);
  shell->home = fm_home_new (shell);
  shell->remote_page = fm_remote_page_new (shell);
  shell->settings_page = fm_settings_page_new (shell);
  shell->pages = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (shell->pages), GTK_STACK_TRANSITION_TYPE_NONE);
  gtk_stack_add_named (GTK_STACK (shell->pages), fm_home_get_widget (shell->home), "home");
  gtk_stack_add_named (GTK_STACK (shell->pages), fm_remote_page_get_widget (shell->remote_page), "remote");
  gtk_stack_add_named (GTK_STACK (shell->pages), fm_browser_get_widget (shell->browser), "browser");
  gtk_stack_add_named (GTK_STACK (shell->pages), shell->settings_page, "settings");
  gtk_overlay_set_child (GTK_OVERLAY (content), shell->pages);
  /* a hidden job's thin progress line, at the bottom of the content */
  shell->job_line = fm_progress_new (2);
  gtk_widget_set_valign (shell->job_line, GTK_ALIGN_END);
  gtk_widget_set_visible (shell->job_line, FALSE);
  gtk_widget_set_can_target (shell->job_line, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (content), shell->job_line);
  gtk_widget_set_vexpand (content, TRUE);
  gtk_box_append (GTK_BOX (main), content);

  /* the selection or paste bar: the items in equal columns (its line on top is drawn by CSS) */
  shell->bottom = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (shell->bottom, "fm-bottom");
  shell->bottom_items = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_set_homogeneous (GTK_BOX (shell->bottom_items), TRUE);
  gtk_widget_add_css_class (shell->bottom_items, "fm-bottom-items");
  gtk_box_append (GTK_BOX (shell->bottom), shell->bottom_items);
  gtk_widget_set_visible (shell->bottom, FALSE);
  gtk_box_append (GTK_BOX (main), shell->bottom);
  /* the row: the docked drawer of a tablet comes in at its start */
  s->row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand (main, TRUE);
  gtk_box_append (GTK_BOX (s->row), main);
  vibeos_window_set_content (GTK_WINDOW (window), s->row);

  overlay = GTK_OVERLAY (g_object_get_data (G_OBJECT (window), "vibeos-overlay"));
  /* the marker the shell's back gesture sees while Back has something to leave */
  s->back_marker = g_object_new (GTK_TYPE_BOX, "accessible-role", GTK_ACCESSIBLE_ROLE_DIALOG, NULL);
  gtk_accessible_update_property (GTK_ACCESSIBLE (s->back_marker), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Files"), -1);
  gtk_widget_set_can_target (s->back_marker, FALSE);
  gtk_widget_set_visible (s->back_marker, FALSE);
  gtk_overlay_add_overlay (overlay, s->back_marker);
  shell->snackbar = vibeos_snackbar_new ();
  vibeos_snackbar_set_margin (VIBEOS_SNACKBAR (shell->snackbar), 0);
  gtk_widget_add_css_class (shell->snackbar, "fm-snackbar");
  gtk_overlay_add_overlay (overlay, shell->snackbar);
  gtk_overlay_add_overlay (overlay, toast_new (s));
  shell->drawer = fm_drawer_new (shell);
  gtk_overlay_add_overlay (overlay, fm_drawer_get_widget (shell->drawer));
  fm_drawer_attach_edge (shell->drawer, main);
  /* the upright tablet's rail, at the row's start (hidden until the layout asks for it) */
  gtk_widget_set_visible (fm_drawer_get_rail (shell->drawer), FALSE);
  gtk_box_prepend (GTK_BOX (s->row), fm_drawer_get_rail (shell->drawer));

  g_signal_connect (fm_browser_get_list (shell->browser), "selection-changed", G_CALLBACK (on_list_changed), shell);
  g_signal_connect (shell->monitor, "mount-added", G_CALLBACK (on_mounts_changed), shell);
  g_signal_connect (shell->monitor, "mount-changed", G_CALLBACK (on_mounts_changed), shell);
  g_signal_connect (shell->monitor, "mount-removed", G_CALLBACK (on_mount_removed), shell);
  g_signal_connect (shell->settings, "changed", G_CALLBACK (on_settings_changed), shell);

  gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (shortcuts),
                                        gtk_shortcut_new (gtk_alternative_trigger_new (
                                                            gtk_keyval_trigger_new (GDK_KEY_Escape, 0),
                                                            gtk_keyval_trigger_new (GDK_KEY_Back, 0)),
                                                          gtk_callback_action_new (on_escape, shell, NULL)));
  gtk_widget_add_controller (window, shortcuts);

  fm_home_refresh (shell->home);
  shell->page = FM_PAGE_HOME;
  gtk_stack_set_visible_child_name (GTK_STACK (shell->pages), "home");
  fm_shell_update_bars (shell);
  s->layout = g_object_ref (vibeos_layout_get_for_window (GTK_WINDOW (window)));
  g_signal_connect (s->layout, "changed", G_CALLBACK (on_layout_changed), shell);
  on_layout_changed (s->layout, shell);
  return window;
}
