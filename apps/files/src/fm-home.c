/* Home (fm-home.h). */
#include "fm-home.h"
#include "fm-icon.h"
#include "fm-ui.h"
#include "fm-util.h"

#include <glib/gi18n.h>
#include <vibeos.h>
#include <spongy-gtk.h>

struct _FmHome {
  FmShell *shell;
  GtkWidget *scroll, *grid;
  GCancellable *cancellable;
};

typedef enum {
  TILE_FOLDER,                  /* a folder or a medium: browse it */
  TILE_REMOTE,                  /* the Remote page */
} TileKind;

static void
on_tile (VibeosPressable *tile, FmHome *home)
{
  GFile *location = g_object_get_data (G_OBJECT (tile), "fm-file");

  if (location)
    fm_shell_open_location (home->shell, location);
  else
    fm_shell_show_page (home->shell, FM_PAGE_REMOTE);
}

static void
on_free_space (GObject *source, GAsyncResult *result, gpointer data)
{
  GtkWidget *info = data;
  g_autoptr(GFileInfo) fs = g_file_query_filesystem_info_finish (G_FILE (source), result, NULL);

  if (fs && g_file_info_has_attribute (fs, G_FILE_ATTRIBUTE_FILESYSTEM_FREE))
    {
      g_autofree char *size = fm_format_size (g_file_info_get_attribute_uint64 (fs, G_FILE_ATTRIBUTE_FILESYSTEM_FREE));
      g_autofree char *text = g_strdup_printf (_("%s free"), size);

      gtk_label_set_text (GTK_LABEL (info), text);
    }
  g_object_unref (info);
}

/* the count of a folder's items, or the Recycle Bin's size, in a worker */
typedef struct {
  GFile *file;
  gboolean size;
} Measure;

static void
measure_free (gpointer data)
{
  Measure *measure = data;

  g_object_unref (measure->file);
  g_free (measure);
}

static void
measure_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  Measure *measure = data;

  if (measure->size)
    {
      FmCount count = { 0 };

      fm_count_tree (measure->file, cancellable, &count);
      g_task_return_pointer (task, fm_format_size (count.bytes), g_free);
    }
  else
    {
      g_autoptr(GFileEnumerator) children = g_file_enumerate_children (measure->file, "standard::name,standard::is-hidden",
                                                                       G_FILE_QUERY_INFO_NONE, cancellable, NULL);
      GFileInfo *info;
      int n = 0;

      while (children && g_file_enumerator_iterate (children, &info, NULL, cancellable, NULL) && info)
        if (!fm_info_is_hidden (info))
          n++;
      g_task_return_pointer (task, children ? fm_format_items (n) : NULL, g_free);
    }
}

static void
on_measured (GObject *source, GAsyncResult *result, gpointer data)
{
  GtkWidget *info = data;
  g_autofree char *text = g_task_propagate_pointer (G_TASK (result), NULL);

  if (text)
    gtk_label_set_text (GTK_LABEL (info), text);
  g_object_unref (info);
}

static void
measure (FmHome *home, GtkWidget *info, GFile *file, gboolean size)
{
  Measure *job = g_new0 (Measure, 1);
  g_autoptr(GTask) task = g_task_new (NULL, home->cancellable, on_measured, g_object_ref (info));

  job->file = g_object_ref (file);
  job->size = size;
  g_task_set_task_data (task, job, measure_free);
  g_task_set_priority (task, G_PRIORITY_LOW);
  g_task_run_in_thread (task, measure_thread);
}

/* a Home tile: the icon, the name and the info line, centred */
static GtkWidget *
tile (FmHome *home, FmGlyph glyph, const char *name, GFile *location, GtkWidget **info_out)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *icon = fm_icon_new (glyph, 64);
  GtkWidget *label = fm_label (name, "fm-tile-name", 16, FALSE);
  GtkWidget *info = fm_label (" ", "fm-tile-info", 12, FALSE);
  GtkWidget *pressable;

  gtk_label_set_xalign (GTK_LABEL (label), 0.5f);
  gtk_label_set_xalign (GTK_LABEL (info), 0.5f);
  /* a name too wide for the tile, at a large font size, goes on to a second line, and only then is cut short */
  gtk_label_set_single_line_mode (GTK_LABEL (label), FALSE);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD);
  gtk_label_set_lines (GTK_LABEL (label), 2);
  gtk_label_set_justify (GTK_LABEL (label), GTK_JUSTIFY_CENTER);
  gtk_widget_set_halign (icon, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), icon);
  gtk_box_append (GTK_BOX (box), label);
  gtk_box_append (GTK_BOX (box), info);
  pressable = vibeos_pressable_new (box);
  gtk_widget_add_css_class (pressable, "fm-tile");
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (pressable), VIBEOS_RIPPLE_BOUNDED, 2);
  if (location)
    g_object_set_data_full (G_OBJECT (pressable), "fm-file", g_object_ref (location), g_object_unref);
  g_signal_connect (pressable, "clicked", G_CALLBACK (on_tile), home);
  gtk_accessible_update_property (GTK_ACCESSIBLE (pressable), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
  gtk_flow_box_append (GTK_FLOW_BOX (home->grid), pressable);
  /* the flow box's own child takes no focus or highlight: the tile is the pressable */
  gtk_widget_set_focusable (gtk_widget_get_parent (pressable), FALSE);
  if (info_out)
    *info_out = info;
  return pressable;
}

void
fm_home_refresh (FmHome *home)
{
  static const struct {
    GUserDirectory dir;
    const char *name;
    FmGlyph glyph;
  } folders[] = {
    { G_USER_DIRECTORY_DOWNLOAD, N_("Downloads"), FM_GLYPH_DOWNLOADS },
    { G_USER_DIRECTORY_PICTURES, N_("Pictures"), FM_GLYPH_PICTURES },
    { G_USER_DIRECTORY_MUSIC, N_("Music"), FM_GLYPH_MUSIC },
    { G_USER_DIRECTORY_VIDEOS, N_("Videos"), FM_GLYPH_VIDEOS },
    { G_USER_DIRECTORY_DOCUMENTS, N_("Documents"), FM_GLYPH_DOCUMENTS },
  };
  g_autoptr(GFile) home_dir = g_file_new_for_path (g_get_home_dir ());
  g_autoptr(GFile) recent = g_file_new_for_uri ("recent:///");
  g_autoptr(GFile) trash = g_file_new_for_uri ("trash:///");
  g_autoptr(GFile) network = g_file_new_for_uri ("network:///");
  GList *mounts = fm_shell_mounts (home->shell);
  GtkWidget *info;

  if (home->cancellable)
    g_cancellable_cancel (home->cancellable);
  g_clear_object (&home->cancellable);
  home->cancellable = g_cancellable_new ();
  gtk_flow_box_remove_all (GTK_FLOW_BOX (home->grid));

  tile (home, FM_GLYPH_STORAGE, _("Main storage"), home_dir, &info);
  g_file_query_filesystem_info_async (home_dir, G_FILE_ATTRIBUTE_FILESYSTEM_FREE, G_PRIORITY_LOW, home->cancellable,
                                      on_free_space, g_object_ref (info));
  for (GList *l = mounts; l; l = l->next)
    {
      g_autoptr(GFile) root = NULL;
      g_autofree char *label = NULL;
      FmGlyph glyph;

      if (!fm_mount_is_media (l->data))
        continue;
      root = g_mount_get_root (l->data);
      label = fm_mount_label (l->data, &glyph);
      tile (home, glyph, label, root, &info);
      g_file_query_filesystem_info_async (root, G_FILE_ATTRIBUTE_FILESYSTEM_FREE, G_PRIORITY_LOW, home->cancellable,
                                          on_free_space, g_object_ref (info));
    }
  g_list_free_full (mounts, g_object_unref);
  /* the XDG folders that exist (and are not $HOME itself, as an unset one is) */
  for (guint i = 0; i < G_N_ELEMENTS (folders); i++)
    {
      const char *path = g_get_user_special_dir (folders[i].dir);
      g_autoptr(GFile) folder = NULL;

      if (!path || g_str_equal (path, g_get_home_dir ()) || !g_file_test (path, G_FILE_TEST_IS_DIR))
        continue;
      folder = g_file_new_for_path (path);
      tile (home, folders[i].glyph, _(folders[i].name), folder, &info);
      measure (home, info, folder, FALSE);
    }
  tile (home, FM_GLYPH_RECENT, _("Recent files"), recent, &info);
  measure (home, info, recent, FALSE);
  tile (home, FM_GLYPH_TRASH, _("Recycle Bin"), trash, &info);
  measure (home, info, trash, TRUE);
  tile (home, FM_GLYPH_REMOTE, _("Remote"), NULL, &info);
  if (home->shell->remotes->len)
    {
      g_autofree char *count = fm_format_items ((int) home->shell->remotes->len);

      gtk_label_set_text (GTK_LABEL (info), count);
    }
  tile (home, FM_GLYPH_NETWORK, _("Local Network"), network, NULL);
}

FmHome *
fm_home_new (FmShell *shell)
{
  FmHome *home = g_new0 (FmHome, 1);

  home->shell = shell;
  home->grid = gtk_flow_box_new ();
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (home->grid), GTK_SELECTION_NONE);
  gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (home->grid), TRUE);
  /* the original's column width fits three on a phone; a wider window gets more, stretched */
  gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (home->grid), 3);
  gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (home->grid), 8);
  gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (home->grid), 4);
  gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (home->grid), 5);
  gtk_flow_box_set_activate_on_single_click (GTK_FLOW_BOX (home->grid), FALSE);
  gtk_widget_set_valign (home->grid, GTK_ALIGN_START);
  gtk_widget_add_css_class (home->grid, "fm-home-grid");
  home->scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (home->scroll), home->grid);
  gtk_widget_add_css_class (home->scroll, "fm-home");
  return home;
}

GtkWidget *
fm_home_get_widget (FmHome *home)
{
  return home->scroll;
}

void
fm_home_free (FmHome *home)
{
  if (home->cancellable)
    g_cancellable_cancel (home->cancellable);
  g_clear_object (&home->cancellable);
  g_free (home);
}
