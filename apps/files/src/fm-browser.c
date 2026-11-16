/* The folder page (fm-browser.h). */
#include "fm-browser.h"
#include "fm-drawer.h"
#include "fm-icon.h"
#include "fm-mount.h"
#include "fm-scan.h"
#include "fm-ui.h"
#include "fm-util.h"

#include <glib/gi18n.h>
#include <vibeos.h>
#include <string.h>

#define BATCH          200      /* rows asked of the backend at a time */
#define RELOAD_DELAY   500      /* ms after the last change the monitor saw */
#define SEARCH_MAX     5000     /* results a search stops at */
#define SEARCH_FLUSH   (150 * G_TIME_SPAN_MILLISECOND)

struct _FmBrowser {
  FmShell *shell;
  GtkWidget *box, *crumb_scroll, *crumbs, *free_label, *up, *loading;
  FmList *list;
  GFile *location;
  FmPlace *place;
  GCancellable *load_cancel, *count_cancel, *search_cancel, *free_cancel, *scan_cancel;
  GPtrArray *pending;           /* the rows being listed */
  GFileMonitor *monitor;
  guint reload_source;
  gboolean keep_offset, record, mount_tried, dirty;
  char *select_name;
  GHashTable *names;            /* a folder's URI -> the name it was listed under (the crumbs) */
  gboolean searching, search_shown, search_busy;
  gboolean scanning;            /* the Local Network's subnet scan is running */
};

static void load_start (FmBrowser *b, gboolean keep_offset);

static gboolean
cancelled (GCancellable *cancellable)
{
  return !cancellable || g_cancellable_is_cancelled (cancellable);
}

static void
cancel (GCancellable **cancellable)
{
  if (*cancellable)
    {
      g_cancellable_cancel (*cancellable);
      g_clear_object (cancellable);
    }
}

/* a new cancellable that knows its browser: the callbacks of a cancelled one never touch it */
static GCancellable *
new_cancellable (FmBrowser *b)
{
  GCancellable *cancellable = g_cancellable_new ();

  g_object_set_data (G_OBJECT (cancellable), "fm-browser", b);
  return cancellable;
}

static FmBrowser *
browser_of (GCancellable *cancellable)
{
  return g_object_get_data (G_OBJECT (cancellable), "fm-browser");
}

/* ---- the path bar ------------------------------------------------------------------------------------------------ */

static void
on_home_crumb (VibeosPressable *crumb, FmBrowser *b)
{
  fm_shell_show_page (b->shell, FM_PAGE_HOME);
}

static void
on_crumb (VibeosPressable *crumb, FmBrowser *b)
{
  GFile *target = g_object_get_data (G_OBJECT (crumb), "fm-file");

  if (target && !g_file_equal (target, b->location))
    fm_shell_open_location (b->shell, target);
}

static void
on_up (VibeosPressable *up, FmBrowser *b)
{
  g_autoptr(GFile) parent = fm_parent (b->location);

  if (parent && b->place && fm_file_is_under (parent, b->place->root))
    fm_shell_open_location (b->shell, parent);
  else if (b->place)
    fm_shell_open_location (b->shell, b->place->root);
}

/* the path bar's separator: the theme's go-next icon, in place of the original's chevron */
static GtkWidget *
chevron (void)
{
  GtkWidget *image = gtk_image_new_from_icon_name ("go-next-symbolic");

  gtk_image_set_pixel_size (GTK_IMAGE (image), 12);
  gtk_widget_set_valign (image, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (image, "fm-crumb-chevron");
  return image;
}

static GtkWidget *
crumb (FmBrowser *b, GtkWidget *child, GFile *target, const char *css_class)
{
  GtkWidget *button = vibeos_pressable_new (child);
  GdkRGBA ripple = fm_rgba (FM_C_RIPPLE);

  gtk_widget_add_css_class (button, "fm-crumb");
  gtk_widget_add_css_class (button, css_class);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (button), VIBEOS_RIPPLE_BOUNDED, 0);
  vibeos_pressable_set_ripple_color (VIBEOS_PRESSABLE (button), &ripple);
  if (target)
    {
      g_object_set_data_full (G_OBJECT (button), "fm-file", g_object_ref (target), g_object_unref);
      g_signal_connect (button, "clicked", G_CALLBACK (on_crumb), b);
    }
  return button;
}

static gboolean
scroll_crumbs_to_end (gpointer data)
{
  FmBrowser *b = data;
  GtkAdjustment *adjustment = gtk_scrolled_window_get_hadjustment (GTK_SCROLLED_WINDOW (b->crumb_scroll));

  gtk_adjustment_set_value (adjustment, gtk_adjustment_get_upper (adjustment));
  return G_SOURCE_REMOVE;
}

static void
on_crumbs_changed (GtkAdjustment *adjustment, FmBrowser *b)
{
  /* the last crumb stays in view: a deep path shows its end, also when the free space comes in beside it and
   * narrows the crumbs */
  gtk_adjustment_set_value (adjustment, gtk_adjustment_get_upper (adjustment));
}

static void
pathbar_build (FmBrowser *b)
{
  GtkWidget *child, *home;
  g_autofree char *relative = fm_relative_path (b->place->root, b->location);
  g_auto(GStrv) parts = g_strsplit (relative, "/", -1);
  g_autoptr(GFile) at = g_object_ref (b->place->root);
  GtkWidget *last = NULL;
  gboolean at_root = TRUE;

  while ((child = gtk_widget_get_first_child (b->crumbs)))
    gtk_box_remove (GTK_BOX (b->crumbs), child);
  home = crumb (b, fm_icon_new (FM_GLYPH_HOME, 32), NULL, "fm-crumb-home");
  g_signal_connect (home, "clicked", G_CALLBACK (on_home_crumb), b);
  gtk_accessible_update_property (GTK_ACCESSIBLE (home), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Home"), -1);
  gtk_box_append (GTK_BOX (b->crumbs), home);
  gtk_box_append (GTK_BOX (b->crumbs), chevron ());
  last = crumb (b, fm_icon_new (b->place->glyph, 32), b->place->root, "fm-crumb-root");
  gtk_accessible_update_property (GTK_ACCESSIBLE (last), GTK_ACCESSIBLE_PROPERTY_LABEL, b->place->name, -1);
  gtk_box_append (GTK_BOX (b->crumbs), last);
  for (int i = 0; parts[i]; i++)
    {
      g_autoptr(GFile) next = NULL;
      g_autofree char *uri = NULL;
      const char *name;
      GtkWidget *label;

      if (!*parts[i])
        continue;
      next = g_file_get_child (at, parts[i]);
      g_set_object (&at, next);
      uri = g_file_get_uri (at);
      name = g_hash_table_lookup (b->names, uri);
      label = fm_label (name ? name : parts[i], "fm-crumb-text", 18, FALSE);
      gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_NONE);
      gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (b->crumbs), chevron ());
      last = crumb (b, label, at, "fm-crumb-folder");
      gtk_box_append (GTK_BOX (b->crumbs), last);
      at_root = FALSE;
    }
  gtk_widget_add_css_class (last, "last");
  gtk_widget_set_visible (b->up, !at_root);
  gtk_widget_set_visible (b->free_label, FALSE);
  g_idle_add (scroll_crumbs_to_end, b);
}

static GtkWidget *
pathbar_new (FmBrowser *b)
{
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

  gtk_widget_add_css_class (bar, "fm-pathbar");
  b->crumbs = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (b->crumbs, "fm-crumbs");
  b->crumb_scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (b->crumb_scroll), GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (b->crumb_scroll), b->crumbs);
  gtk_widget_set_hexpand (b->crumb_scroll, TRUE);
  /* "changed": the upper or the page size moved (gtk_adjustment_configure, on each allocation) */
  g_signal_connect (gtk_scrolled_window_get_hadjustment (GTK_SCROLLED_WINDOW (b->crumb_scroll)), "changed",
                    G_CALLBACK (on_crumbs_changed), b);
  b->free_label = fm_label (NULL, "fm-free", 14, FALSE);
  gtk_label_set_ellipsize (GTK_LABEL (b->free_label), PANGO_ELLIPSIZE_NONE);
  gtk_widget_set_valign (b->free_label, GTK_ALIGN_CENTER);
  b->up = fm_icon_button_sized ("go-up-symbolic", _("Up"), 44, 44, 22);
  gtk_widget_add_css_class (b->up, "fm-up");
  g_signal_connect (b->up, "clicked", G_CALLBACK (on_up), b);
  gtk_box_append (GTK_BOX (bar), b->crumb_scroll);
  gtk_box_append (GTK_BOX (bar), b->free_label);
  gtk_box_append (GTK_BOX (bar), b->up);
  return bar;
}

/* ---- counting the folders' items --------------------------------------------------------------------------------- */

typedef struct {
  GPtrArray *items;             /* FmItem */
  gboolean hidden;
} CountJob;

typedef struct {
  FmItem *item;
  int count;
  GCancellable *cancellable;
} Counted;

static void
count_job_free (gpointer data)
{
  CountJob *job = data;

  g_ptr_array_unref (job->items);
  g_free (job);
}

static gboolean
on_counted (gpointer data)
{
  Counted *counted = data;

  if (!cancelled (counted->cancellable))
    {
      counted->item->count = counted->count;
      fm_item_changed (counted->item);
    }
  g_object_unref (counted->item);
  g_object_unref (counted->cancellable);
  g_free (counted);
  return G_SOURCE_REMOVE;
}

static void
count_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  CountJob *job = data;

  for (guint i = 0; i < job->items->len && !g_cancellable_is_cancelled (cancellable); i++)
    {
      FmItem *item = job->items->pdata[i];
      g_autoptr(GFileEnumerator) children =
        g_file_enumerate_children (item->file, "standard::name,standard::is-hidden,standard::is-backup",
                                   G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, NULL);
      Counted *counted = g_new0 (Counted, 1);
      GFileInfo *info;
      int n = 0;

      while (children && g_file_enumerator_iterate (children, &info, NULL, cancellable, NULL) && info)
        if (job->hidden || !fm_info_is_hidden (info))
          n++;
      counted->item = g_object_ref (item);
      counted->count = children ? n : FM_COUNT_UNKNOWN;
      counted->cancellable = g_object_ref (cancellable);
      g_main_context_invoke (NULL, on_counted, counted);
    }
  g_task_return_boolean (task, TRUE);
}

static void
count_start (FmBrowser *b, GPtrArray *items)
{
  CountJob *job = g_new0 (CountJob, 1);
  g_autoptr(GTask) task = NULL;

  cancel (&b->count_cancel);
  b->count_cancel = new_cancellable (b);
  job->items = g_ptr_array_new_with_free_func (g_object_unref);
  job->hidden = g_settings_get_boolean (b->shell->settings, "show-hidden");
  for (guint i = 0; i < items->len; i++)
    {
      FmItem *item = items->pdata[i];

      /* plain folders only: a share or a host would have to be mounted to be counted */
      if (item->type == G_FILE_TYPE_DIRECTORY && item->count == FM_COUNT_PENDING)
        g_ptr_array_add (job->items, g_object_ref (item));
      else if (item->count == FM_COUNT_PENDING)
        item->count = FM_COUNT_UNKNOWN;
    }
  if (!job->items->len)
    {
      count_job_free (job);
      return;
    }
  task = g_task_new (NULL, b->count_cancel, NULL, NULL);
  g_task_set_task_data (task, job, count_job_free);
  g_task_set_priority (task, G_PRIORITY_LOW);
  g_task_run_in_thread (task, count_thread);
}

/* ---- the free space ---------------------------------------------------------------------------------------------- */

static void
on_free_space (GObject *source, GAsyncResult *result, gpointer data)
{
  GCancellable *cancellable = data;
  g_autoptr(GFileInfo) info = g_file_query_filesystem_info_finish (G_FILE (source), result, NULL);

  if (!cancelled (cancellable) && info && g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE))
    {
      FmBrowser *b = browser_of (cancellable);
      g_autofree char *size = fm_format_size (g_file_info_get_attribute_uint64 (info,
                                                                                G_FILE_ATTRIBUTE_FILESYSTEM_FREE));
      g_autofree char *text = g_strdup_printf (_("%s free"), size);

      gtk_label_set_text (GTK_LABEL (b->free_label), text);
      gtk_widget_set_visible (b->free_label, TRUE);
    }
  g_object_unref (cancellable);
}

/* ---- watching for changes ---------------------------------------------------------------------------------------- */

static gboolean
on_reload_due (gpointer data)
{
  FmBrowser *b = data;

  b->reload_source = 0;
  if (fm_list_get_selecting (b->list) || b->search_shown)
    b->dirty = TRUE;
  else
    load_start (b, TRUE);
  return G_SOURCE_REMOVE;
}

static void
on_monitor_changed (GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event, FmBrowser *b)
{
  if (event == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT || event == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED
      || event == G_FILE_MONITOR_EVENT_PRE_UNMOUNT)
    return;
  g_clear_handle_id (&b->reload_source, g_source_remove);
  b->reload_source = g_timeout_add (RELOAD_DELAY, on_reload_due, b);
}

static void
monitor_start (FmBrowser *b)
{
  if (b->monitor)
    {
      g_signal_handlers_disconnect_by_data (b->monitor, b);
      g_file_monitor_cancel (b->monitor);
      g_clear_object (&b->monitor);
    }
  /* GVfs's smb and sftp backends do not watch: those folders show what was listed until Refresh */
  b->monitor = g_file_monitor_directory (b->location, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
  if (b->monitor)
    g_signal_connect (b->monitor, "changed", G_CALLBACK (on_monitor_changed), b);
}

/* ---- listing ------------------------------------------------------------------------------------------------------ */

static void
loading (FmBrowser *b, gboolean busy)
{
  gtk_widget_set_visible (b->loading, busy);
  fm_progress_set_fraction (b->loading, busy ? -1 : 0);
}

static void
record_visit (FmBrowser *b)
{
  g_autofree char *uri = g_file_get_uri (b->location);
  g_autofree char *root = g_file_get_uri (b->place->root);
  g_autofree char *relative = fm_relative_path (b->place->root, b->location);
  FmShell *shell = b->shell;

  if (g_settings_get_boolean (shell->settings, "show-history") && b->place->kind != FM_PLACE_NETWORK)
    fm_state_add_history (shell->state, uri, b->place->name);
  if (fm_place_is_visitable (b->place))
    fm_state_visit (shell->state, root, b->place->name, uri, relative);
  if (shell->drawer)
    fm_drawer_refresh (shell->drawer);
}

static FmSort
sort_order (FmBrowser *b)
{
  g_autofree char *value = g_settings_get_string (b->shell->settings, "sort-order");

  return fm_sort_from_string (value);
}

static void
list_message (FmBrowser *b)
{
  GListModel *items = fm_list_get_items (b->list);

  if (b->search_busy)
    fm_list_set_message (b->list, NULL);
  else if (b->search_shown && !g_list_model_get_n_items (items))
    fm_list_set_message (b->list, _("No results"));
  else if (b->scanning && !g_list_model_get_n_items (items))
    fm_list_set_message (b->list, NULL);
  else if (!g_list_model_get_n_items (items))
    fm_list_set_message (b->list, b->place && b->place->kind == FM_PLACE_TRASH ? _("The Recycle Bin is empty")
                                                                               : _("Empty folder"));
  else
    fm_list_set_message (b->list, NULL);
}

/* ---- the Local Network's hosts ------------------------------------------------------------------------------------- */

/* whether a listed host is @address or is called @name (GVfs's network:/// rows target smb://NAME/ or smb://host.local/) */
static gboolean
host_listed (FmBrowser *b, const char *address, const char *name)
{
  GListModel *items = fm_list_get_items (b->list);
  guint n = g_list_model_get_n_items (items);

  for (guint i = 0; i < n; i++)
    {
      g_autoptr(FmItem) item = g_list_model_get_item (items, i);
      g_autoptr(GUri) uri = item->target_uri ? g_uri_parse (item->target_uri, G_URI_FLAGS_NONE, NULL) : NULL;
      const char *host = uri ? g_uri_get_host (uri) : NULL;
      g_autofree char *label = NULL;

      if (!host)
        continue;
      label = g_strndup (host, strcspn (host, "."));
      if (!g_ascii_strcasecmp (host, address) || !g_ascii_strcasecmp (label, name)
          || !g_ascii_strcasecmp (item->name, name))
        return TRUE;
    }
  return FALSE;
}

/* a file server the subnet scan found, unless network:/// lists it already: its shares are at smb://address/ */
static void
on_lan_host (const char *address, const char *name, gpointer data)
{
  FmBrowser *b = data;
  g_autofree char *uri = NULL;
  g_autoptr(GFile) file = NULL;
  g_autoptr(GPtrArray) items = NULL;
  GListModel *listed;
  FmItem *item;

  if (host_listed (b, address, name))
    return;
  uri = g_uri_join (G_URI_FLAGS_NONE, "smb", NULL, address, -1, "/", NULL, NULL);
  file = g_file_new_for_uri (uri);
  item = fm_item_new_for_file (file, name, G_FILE_TYPE_MOUNTABLE);
  item->target_uri = g_steal_pointer (&uri);
  item->count = FM_COUNT_UNKNOWN;
  item->can_write = item->can_delete = item->can_trash = item->can_rename = FALSE;
  /* in the folder's order with the rest */
  listed = fm_list_get_items (b->list);
  items = g_ptr_array_new_with_free_func (g_object_unref);
  for (guint i = 0; i < g_list_model_get_n_items (listed); i++)
    g_ptr_array_add (items, g_list_model_get_item (listed, i));
  g_ptr_array_add (items, item);
  fm_items_sort (items, sort_order (b));
  fm_list_set_items (b->list, items, TRUE);
  list_message (b);
}

static void
on_lan_done (gpointer data)
{
  FmBrowser *b = data;

  b->scanning = FALSE;
  loading (b, FALSE);
  list_message (b);
}

/* File Manager+ does not wait for announcements (the phone may run no avahi-daemon or wsdd): it knocks on the
 * addresses of its networks. fm_scan_start calls nothing once scan_cancel is cancelled, so it is given the browser. */
static void
lan_scan_start (FmBrowser *b)
{
  cancel (&b->scan_cancel);
  b->scan_cancel = new_cancellable (b);
  b->scanning = TRUE;
  loading (b, TRUE);
  fm_scan_start (b->scan_cancel, on_lan_host, on_lan_done, b);
}

static void
load_finish (FmBrowser *b)
{
  GPtrArray *items = b->pending;
  FmItem *select = NULL;

  b->pending = NULL;
  fm_items_sort (items, sort_order (b));
  for (guint i = 0; i < items->len; i++)
    {
      FmItem *item = items->pdata[i];
      g_autofree char *uri = g_file_get_uri (item->file);

      if (fm_item_is_folder (item))
        g_hash_table_replace (b->names, g_steal_pointer (&uri), g_strdup (item->name));
      /* the Recycle Bin's rows say where they were, Recent files' where they are */
      if (item->orig_path)
        item->where = g_path_get_dirname (item->orig_path);
      else if (b->place->kind == FM_PLACE_RECENT && item->target_uri)
        {
          g_autoptr(GFile) target = g_file_new_for_uri (item->target_uri);
          g_autoptr(GFile) folder = fm_parent (target);

          item->where = folder ? fm_display_path (folder) : NULL;
        }
      if (b->select_name && !g_strcmp0 (item->name, b->select_name))
        select = item;
    }
  fm_list_set_items (b->list, items, b->keep_offset);
  list_message (b);
  loading (b, FALSE);
  if (select)
    fm_list_scroll_to (b->list, select);
  g_clear_pointer (&b->select_name, g_free);
  count_start (b, items);
  monitor_start (b);
  if (b->record)
    record_visit (b);
  b->record = FALSE;
  pathbar_build (b);
  /* the free space of the filesystem below a location's root */
  if (!g_file_equal (b->location, b->place->root) && b->place->kind != FM_PLACE_TRASH
      && b->place->kind != FM_PLACE_RECENT && b->place->kind != FM_PLACE_NETWORK)
    {
      cancel (&b->free_cancel);
      b->free_cancel = new_cancellable (b);
      g_file_query_filesystem_info_async (b->location, G_FILE_ATTRIBUTE_FILESYSTEM_FREE, G_PRIORITY_LOW,
                                          b->free_cancel, on_free_space, g_object_ref (b->free_cancel));
    }
  g_ptr_array_unref (items);
  if (b->place->kind == FM_PLACE_NETWORK && g_file_equal (b->location, b->place->root))
    lan_scan_start (b);
  fm_shell_update_bars (b->shell);
}

static void
load_failed (FmBrowser *b, const GError *error)
{
  g_clear_pointer (&b->pending, g_ptr_array_unref);
  fm_list_set_items (b->list, NULL, FALSE);
  fm_list_set_message (b->list, error->message);
  loading (b, FALSE);
  pathbar_build (b);
  /* without GVfs's network backend the Local Network is still the scan's */
  if (b->place && b->place->kind == FM_PLACE_NETWORK && g_file_equal (b->location, b->place->root))
    {
      fm_list_set_message (b->list, NULL);
      lan_scan_start (b);
    }
  fm_shell_update_bars (b->shell);
}

static void
on_closed (GObject *source, GAsyncResult *result, gpointer data)
{
  g_file_enumerator_close_finish (G_FILE_ENUMERATOR (source), result, NULL);
}

static void
on_next_files (GObject *source, GAsyncResult *result, gpointer data)
{
  GCancellable *cancellable = data;
  GFileEnumerator *enumerator = G_FILE_ENUMERATOR (source);
  g_autoptr(GError) error = NULL;
  GList *infos = g_file_enumerator_next_files_finish (enumerator, result, &error);
  FmBrowser *b;
  gboolean hidden;

  if (cancelled (cancellable))
    {
      g_list_free_full (infos, g_object_unref);
      g_file_enumerator_close_async (enumerator, G_PRIORITY_LOW, NULL, on_closed, NULL);
      g_object_unref (enumerator);
      g_object_unref (cancellable);
      return;
    }
  b = browser_of (cancellable);
  hidden = g_settings_get_boolean (b->shell->settings, "show-hidden");
  for (GList *l = infos; l; l = l->next)
    {
      FmItem *item = fm_item_new_from_info (b->location, l->data);

      if (item->hidden && !hidden)
        g_object_unref (item);
      else
        g_ptr_array_add (b->pending, item);
    }
  if (infos)
    {
      g_list_free_full (infos, g_object_unref);
      g_file_enumerator_next_files_async (enumerator, BATCH, G_PRIORITY_DEFAULT, cancellable, on_next_files,
                                          cancellable);
      return;
    }
  g_file_enumerator_close_async (enumerator, G_PRIORITY_LOW, NULL, on_closed, NULL);
  g_object_unref (enumerator);
  if (error)
    load_failed (b, error);
  else
    load_finish (b);
  g_object_unref (cancellable);
}

static void
on_mounted_for_load (gboolean ok, const GError *error, gpointer data)
{
  GCancellable *cancellable = data;
  FmBrowser *b;

  if (cancelled (cancellable))
    {
      g_object_unref (cancellable);
      return;
    }
  b = browser_of (cancellable);
  g_object_unref (cancellable);
  if (ok)
    {
      gboolean record = b->record;

      load_start (b, FALSE);
      b->record = record;
      return;
    }
  {
    FmRemote *remote = b->place && b->place->remote_id ? fm_shell_find_remote (b->shell, b->place->remote_id) : NULL;
    g_autofree char *message = fm_mount_error_message (b->location, remote);
    g_autoptr(GError) shown = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, message);

    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED))
      fm_shell_toast (b->shell, message);
    load_failed (b, shown);
  }
}

static void
on_enumerated (GObject *source, GAsyncResult *result, gpointer data)
{
  GCancellable *cancellable = data;
  g_autoptr(GError) error = NULL;
  GFileEnumerator *enumerator = g_file_enumerate_children_finish (G_FILE (source), result, &error);
  FmBrowser *b;

  if (cancelled (cancellable))
    {
      g_clear_object (&enumerator);
      g_object_unref (cancellable);
      return;
    }
  b = browser_of (cancellable);
  if (!enumerator)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_MOUNTED) && !b->mount_tried)
        {
          FmRemote *remote = b->place->remote_id ? fm_shell_find_remote (b->shell, b->place->remote_id) : NULL;

          b->mount_tried = TRUE;
          fm_mount (b->shell->window, b->location, remote, NULL, on_mounted_for_load, cancellable);
          return;
        }
      load_failed (b, error);
      g_object_unref (cancellable);
      return;
    }
  g_file_enumerator_next_files_async (enumerator, BATCH, G_PRIORITY_DEFAULT, cancellable, on_next_files,
                                      cancellable);
}

static void
cancel_all (FmBrowser *b)
{
  cancel (&b->load_cancel);
  cancel (&b->count_cancel);
  cancel (&b->free_cancel);
  cancel (&b->search_cancel);
  cancel (&b->scan_cancel);
  g_clear_handle_id (&b->reload_source, g_source_remove);
  b->search_busy = FALSE;
  b->scanning = FALSE;
}

static void
load_start (FmBrowser *b, gboolean keep_offset)
{
  cancel_all (b);
  b->load_cancel = new_cancellable (b);
  b->keep_offset = keep_offset;
  b->dirty = FALSE;
  b->search_shown = FALSE;
  g_clear_pointer (&b->pending, g_ptr_array_unref);
  b->pending = g_ptr_array_new_with_free_func (g_object_unref);
  loading (b, TRUE);
  g_file_enumerate_children_async (b->location, FM_ITEM_ATTRIBUTES, G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT,
                                   b->load_cancel, on_enumerated, g_object_ref (b->load_cancel));
}

void
fm_browser_load (FmBrowser *b, GFile *location)
{
  g_set_object (&b->location, location);
  g_clear_pointer (&b->place, fm_place_free);
  b->place = fm_shell_place_for (b->shell, location);
  b->mount_tried = FALSE;
  b->record = TRUE;
  fm_list_set_selecting (b->list, FALSE);
  fm_list_set_filter (b->list, NULL);
  fm_list_set_items (b->list, NULL, FALSE);
  fm_list_set_message (b->list, NULL);
  pathbar_build (b);
  load_start (b, FALSE);
}

void
fm_browser_reload (FmBrowser *b, const char *select_name)
{
  if (!b->location)
    return;
  g_free (b->select_name);
  b->select_name = g_strdup (select_name);
  b->mount_tried = FALSE;
  /* the place may have changed name or kind (a remote saved, renamed) */
  g_clear_pointer (&b->place, fm_place_free);
  b->place = fm_shell_place_for (b->shell, b->location);
  load_start (b, TRUE);
}

/* ---- the rows ---------------------------------------------------------------------------------------------------- */

static void
on_activate (FmList *list, FmItem *item, FmBrowser *b)
{
  g_autoptr(GFile) location = fm_item_get_location (item);

  /* a file in the Recycle Bin is not opened from there: a tap selects it, to restore or delete */
  if (b->place->kind == FM_PLACE_TRASH)
    {
      fm_list_set_selecting (list, TRUE);
      fm_list_set_selected (list, item, TRUE);
      fm_shell_update_bars (b->shell);
      return;
    }
  if (fm_item_is_folder (item))
    {
      g_autofree char *uri = g_file_get_uri (location);

      g_hash_table_replace (b->names, g_steal_pointer (&uri), g_strdup (item->name));
    }
  fm_shell_open_item_location (b->shell, location, fm_item_is_folder (item));
}

/* a search result's long press: Open, Open location */
static void
on_result_menu (int index, gpointer data)
{
  FmItem *item = data;
  FmBrowser *b = g_object_get_data (G_OBJECT (item), "fm-browser");
  g_autoptr(GFile) location = fm_item_get_location (item);

  if (index == 0)
    fm_shell_open_item_location (b->shell, location, fm_item_is_folder (item));
  else
    {
      g_autoptr(GFile) parent = fm_parent (location);

      if (parent)
        {
          g_autofree char *name = g_file_get_basename (location);

          fm_shell_open_location (b->shell, parent);
          g_free (b->select_name);
          b->select_name = g_steal_pointer (&name);
        }
    }
}

static void
on_long_press (FmList *list, FmItem *item, double x, double y, FmBrowser *b)
{
  /* the Local Network's computers and shares are only opened */
  if (b->place && b->place->kind == FM_PLACE_NETWORK)
    return;
  if (b->search_shown && !fm_list_get_selecting (list))
    {
      const char *labels[] = { _("Open"), _("Open location") };

      /* the row stays the menu's while it shows (the menu's scrim keeps the list as it is) */
      g_object_set_data (G_OBJECT (item), "fm-browser", b);
      g_object_set_data_full (G_OBJECT (list), "fm-result-menu", g_object_ref (item), g_object_unref);
      fm_popup_show (GTK_WIDGET (list), labels, 2, FM_POPUP_AT, x, y, on_result_menu, item);
      return;
    }
  fm_list_set_selecting (list, TRUE);
  fm_list_set_selected (list, item, TRUE);
  fm_shell_update_bars (b->shell);
}

static void
on_selection_changed (FmList *list, FmBrowser *b)
{
  /* the last row unselected ends selection mode, as File Manager+'s action mode does */
  if (fm_list_get_selecting (list) && !fm_list_count_selected (list))
    fm_list_set_selecting (list, FALSE);
  if (!fm_list_get_selecting (list) && b->dirty)
    {
      b->dirty = FALSE;
      load_start (b, TRUE);
    }
  fm_shell_update_bars (b->shell);
}

/* ---- search -------------------------------------------------------------------------------------------------------- */

typedef struct {
  GFile *from, *root;
  char *needle;
  gboolean hidden;
  GPtrArray *batch;
  gint64 flushed;
  guint found;
} Search;

typedef struct {
  GPtrArray *items;
  gboolean done;
  GCancellable *cancellable;
} Found;

static void
search_free (gpointer data)
{
  Search *search = data;

  g_object_unref (search->from);
  g_object_unref (search->root);
  g_free (search->needle);
  g_clear_pointer (&search->batch, g_ptr_array_unref);
  g_free (search);
}

static gboolean
on_found (gpointer data)
{
  Found *found = data;

  if (!cancelled (found->cancellable))
    {
      FmBrowser *b = browser_of (found->cancellable);

      if (found->items && found->items->len)
        fm_list_append_items (b->list, found->items);
      if (found->done)
        {
          b->search_busy = FALSE;
          list_message (b);
          fm_shell_update_bars (b->shell);
        }
    }
  g_clear_pointer (&found->items, g_ptr_array_unref);
  g_object_unref (found->cancellable);
  g_free (found);
  return G_SOURCE_REMOVE;
}

static void
search_flush (Search *search, GCancellable *cancellable, gboolean done)
{
  Found *found = g_new0 (Found, 1);

  found->items = search->batch;
  found->done = done;
  found->cancellable = g_object_ref (cancellable);
  search->batch = g_ptr_array_new_with_free_func (g_object_unref);
  search->flushed = g_get_monotonic_time ();
  g_main_context_invoke (NULL, on_found, found);
}

static void
search_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  Search *search = data;
  GQueue folders = G_QUEUE_INIT;
  GFile *folder;

  search->batch = g_ptr_array_new_with_free_func (g_object_unref);
  g_queue_push_tail (&folders, g_object_ref (search->from));
  while ((folder = g_queue_pop_head (&folders)) && !g_cancellable_is_cancelled (cancellable)
         && search->found < SEARCH_MAX)
    {
      g_autoptr(GFileEnumerator) children =
        g_file_enumerate_children (folder, FM_ITEM_ATTRIBUTES, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, NULL);
      g_autofree char *where = fm_relative_path (search->root, folder);
      GFileInfo *info;
      GFile *child;

      while (children && g_file_enumerator_iterate (children, &info, &child, cancellable, NULL) && info)
        {
          const char *name = g_file_info_get_display_name (info);
          gboolean hidden = fm_info_is_hidden (info);

          if (hidden && !search->hidden)
            continue;
          if (fm_name_matches (name ? name : g_file_info_get_name (info), search->needle))
            {
              FmItem *item = fm_item_new_from_info (folder, info);

              item->where = g_strdup (where);
              item->count = FM_COUNT_UNKNOWN;
              g_ptr_array_add (search->batch, item);
              search->found++;
            }
          if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY && !g_file_info_get_is_symlink (info))
            g_queue_push_tail (&folders, g_object_ref (child));
          if (search->batch->len && g_get_monotonic_time () - search->flushed > SEARCH_FLUSH)
            search_flush (search, cancellable, FALSE);
        }
      g_object_unref (folder);
    }
  if (folder)
    g_object_unref (folder);
  g_queue_clear_full (&folders, g_object_unref);
  search_flush (search, cancellable, TRUE);
  g_task_return_boolean (task, TRUE);
}

void
fm_browser_search_begin (FmBrowser *b)
{
  b->searching = TRUE;
}

void
fm_browser_search_filter (FmBrowser *b, const char *text)
{
  GListModel *items = fm_list_get_items (b->list);
  guint n = g_list_model_get_n_items (items);
  g_autofree char *needle = text && *text ? g_utf8_casefold (text, -1) : NULL;
  gboolean any = FALSE;

  fm_list_set_filter (b->list, text);
  if (!needle || b->search_busy)
    {
      list_message (b);
      return;
    }
  /* "No results" when the filter leaves nothing */
  for (guint i = 0; i < n && !any; i++)
    {
      g_autoptr(FmItem) item = g_list_model_get_item (items, i);

      any = fm_name_matches (item->name, needle);
    }
  fm_list_set_message (b->list, any ? NULL : _("No results"));
}

void
fm_browser_search_run (FmBrowser *b, const char *text)
{
  Search *search;
  g_autoptr(GTask) task = NULL;

  if (!text || !*text || !b->location)
    return;
  cancel (&b->search_cancel);
  cancel (&b->count_cancel);
  b->search_cancel = new_cancellable (b);
  b->search_shown = TRUE;
  b->search_busy = TRUE;
  fm_list_set_selecting (b->list, FALSE);
  fm_list_set_filter (b->list, NULL);
  fm_list_set_items (b->list, NULL, FALSE);
  fm_list_set_message (b->list, NULL);
  search = g_new0 (Search, 1);
  search->from = g_object_ref (b->location);
  search->root = g_object_ref (b->place->root);
  search->needle = g_utf8_casefold (text, -1);
  search->hidden = g_settings_get_boolean (b->shell->settings, "show-hidden");
  task = g_task_new (NULL, b->search_cancel, NULL, NULL);
  g_task_set_task_data (task, search, search_free);
  g_task_run_in_thread (task, search_thread);
  fm_shell_update_bars (b->shell);
}

void
fm_browser_search_end (FmBrowser *b)
{
  gboolean shown = b->search_shown;

  b->searching = FALSE;
  cancel (&b->search_cancel);
  b->search_busy = FALSE;
  fm_list_set_filter (b->list, NULL);
  if (shown || b->dirty)
    load_start (b, FALSE);
  else
    list_message (b);
}

gboolean
fm_browser_is_searching (FmBrowser *b)
{
  return b->searching;
}

gboolean
fm_browser_search_busy (FmBrowser *b)
{
  return b->search_busy;
}

/* ---- the rest ------------------------------------------------------------------------------------------------------ */

void
fm_browser_settings_changed (FmBrowser *b)
{
  g_autofree char *type = g_settings_get_string (b->shell->settings, "view-type");

  fm_list_set_show_thumbnails (b->list, g_settings_get_boolean (b->shell->settings, "show-thumbnails"));
  fm_list_set_grid (b->list, !g_strcmp0 (type, "grid"));
  if (b->location && !b->search_shown)
    load_start (b, TRUE);
}

GFile *
fm_browser_get_location (FmBrowser *b)
{
  return b->location;
}

FmPlace *
fm_browser_get_place (FmBrowser *b)
{
  return b->place;
}

gboolean
fm_browser_is_trash (FmBrowser *b)
{
  return b->place && b->place->kind == FM_PLACE_TRASH;
}

gboolean
fm_browser_is_virtual (FmBrowser *b)
{
  return !b->place || b->place->kind == FM_PLACE_TRASH || b->place->kind == FM_PLACE_RECENT
         || b->place->kind == FM_PLACE_NETWORK || b->search_shown;
}

gboolean
fm_browser_has_name (FmBrowser *b, const char *name)
{
  GListModel *items = fm_list_get_items (b->list);
  guint n = g_list_model_get_n_items (items);

  for (guint i = 0; i < n; i++)
    {
      g_autoptr(FmItem) item = g_list_model_get_item (items, i);
      g_autofree char *base = g_file_get_basename (item->file);

      if (!g_strcmp0 (item->name, name) || !g_strcmp0 (base, name))
        return TRUE;
    }
  return FALSE;
}

FmList *
fm_browser_get_list (FmBrowser *b)
{
  return b->list;
}

GtkWidget *
fm_browser_get_widget (FmBrowser *b)
{
  return b->box;
}

FmBrowser *
fm_browser_new (FmShell *shell)
{
  FmBrowser *b = g_new0 (FmBrowser, 1);
  GtkWidget *list_box = gtk_overlay_new ();

  b->shell = shell;
  b->names = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  b->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (b->box, "fm-browser");
  gtk_box_append (GTK_BOX (b->box), pathbar_new (b));
  b->list = FM_LIST (fm_list_new ());
  fm_list_set_show_thumbnails (b->list, g_settings_get_boolean (shell->settings, "show-thumbnails"));
  {
    g_autofree char *type = g_settings_get_string (shell->settings, "view-type");

    fm_list_set_grid (b->list, !g_strcmp0 (type, "grid"));
  }
  gtk_widget_set_vexpand (GTK_WIDGET (b->list), TRUE);
  g_signal_connect (b->list, "activate", G_CALLBACK (on_activate), b);
  g_signal_connect (b->list, "long-press", G_CALLBACK (on_long_press), b);
  g_signal_connect (b->list, "selection-changed", G_CALLBACK (on_selection_changed), b);
  gtk_overlay_set_child (GTK_OVERLAY (list_box), GTK_WIDGET (b->list));
  /* the thin progress bar under the path bar while a folder is being listed */
  b->loading = fm_progress_new (2.67);
  gtk_widget_set_valign (b->loading, GTK_ALIGN_START);
  gtk_widget_set_visible (b->loading, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (list_box), b->loading);
  gtk_widget_set_vexpand (list_box, TRUE);
  gtk_box_append (GTK_BOX (b->box), list_box);
  return b;
}

void
fm_browser_free (FmBrowser *b)
{
  cancel_all (b);
  if (b->monitor)
    {
      g_signal_handlers_disconnect_by_data (b->monitor, b);
      g_file_monitor_cancel (b->monitor);
      g_clear_object (&b->monitor);
    }
  g_clear_pointer (&b->pending, g_ptr_array_unref);
  g_clear_object (&b->location);
  g_clear_pointer (&b->place, fm_place_free);
  g_clear_pointer (&b->names, g_hash_table_unref);
  g_free (b->select_name);
  g_free (b);
}
