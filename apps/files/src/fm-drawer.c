/* The drawer (fm-drawer.h). */
#include "fm-drawer.h"
#include "fm-dialogs.h"
#include "fm-icon.h"
#include "fm-ui.h"
#include "fm-util.h"

#include <glib/gi18n.h>
#include <math.h>
#include <neo.h>
#include <spongy-gtk.h>

#define DRAWER_W        280
#define EDGE_W          20      /* DrawerLayout's edge size */
#define TOUCH_SLOP      8
#define SETTLE_BASE_MS  256     /* ViewDragHelper: (fraction + 1) x 256 ms */
#define SETTLE_MAX_MS   600

typedef enum { TAB_LOCATIONS, TAB_BOOKMARKS, TAB_HISTORY, N_TABS } Tab;

struct _FmDrawer {
  FmShell *shell;
  GtkWidget *layer, *scrim, *sheet, *tabs[N_TABS], *tab_line, *pages;
  GtkWidget *locations, *bookmarks, *history;
  Tab tab;
  NeoAnim *anim;
  double open, drag_from;
  gboolean editing_bookmarks;
  GCancellable *cancellable;    /* the storage and Recycle Bin sizes being asked */
};

/* ---- the layer: the scrim fills the window, the sheet slides by its offset --------------------------------------- */

#define FM_TYPE_DRAWER_LAYER (fm_drawer_layer_get_type ())
G_DECLARE_FINAL_TYPE (FmDrawerLayer, fm_drawer_layer, FM, DRAWER_LAYER, GtkWidget)

struct _FmDrawerLayer {
  GtkWidget parent;
  FmDrawer *drawer;
};

G_DEFINE_FINAL_TYPE (FmDrawerLayer, fm_drawer_layer, GTK_TYPE_WIDGET)

static void
fm_drawer_layer_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                         int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 0;
}

static void
fm_drawer_layer_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  FmDrawer *drawer = FM_DRAWER_LAYER (widget)->drawer;
  int sheet_w = MIN (DRAWER_W, width);

  gtk_widget_allocate (drawer->scrim, width, height, -1, NULL);
  gtk_widget_allocate (drawer->sheet, sheet_w, height, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT ((float) round (-sheet_w * (1 - drawer->open)),
                                                                            0)));
}

static void
fm_drawer_layer_dispose (GObject *object)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (GTK_WIDGET (object))))
    gtk_widget_unparent (child);
  G_OBJECT_CLASS (fm_drawer_layer_parent_class)->dispose (object);
}

static void
fm_drawer_layer_class_init (FmDrawerLayerClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = fm_drawer_layer_dispose;
  GTK_WIDGET_CLASS (klass)->measure = fm_drawer_layer_measure;
  GTK_WIDGET_CLASS (klass)->size_allocate = fm_drawer_layer_size_allocate;
  gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "fm-drawer");
  /* an open drawer is a dialog to the shell's back gesture, which then sends Escape */
  gtk_widget_class_set_accessible_role (GTK_WIDGET_CLASS (klass), GTK_ACCESSIBLE_ROLE_DIALOG);
}

static void
fm_drawer_layer_init (FmDrawerLayer *self)
{
}

/* ---- opening and closing ------------------------------------------------------------------------------------------ */

static void
place (FmDrawer *drawer)
{
  gtk_widget_set_visible (drawer->layer, drawer->open > 0);
  gtk_widget_set_opacity (drawer->scrim, drawer->open);
  gtk_widget_queue_allocate (drawer->layer);
}

static void
on_anim (NeoAnim *anim, double value, gpointer data)
{
  FmDrawer *drawer = data;

  drawer->open = value;
  place (drawer);
}

void
fm_drawer_open (FmDrawer *drawer, gboolean open)
{
  double to = open ? 1 : 0;
  double distance = fabs (to - drawer->open);

  if (distance <= 0 && !neo_anim_is_running (drawer->anim))
    return;
  if (open)
    {
      fm_popup_dismiss (drawer->layer);
      fm_drawer_refresh (drawer);
      gtk_widget_set_visible (drawer->layer, TRUE);
      gtk_widget_grab_focus (drawer->sheet);
    }
  neo_anim_start (drawer->anim, drawer->open, to, (guint) MIN ((distance + 1) * SETTLE_BASE_MS, SETTLE_MAX_MS), 0,
                  SPONGY_DECELERATE_QUINT);
}

gboolean
fm_drawer_is_open (FmDrawer *drawer)
{
  return drawer->open > 0 && !(neo_anim_is_running (drawer->anim) && neo_anim_get_to (drawer->anim) == 0);
}

static void
on_scrim_pressed (GtkGestureClick *gesture, int n, double x, double y, FmDrawer *drawer)
{
  fm_drawer_open (drawer, FALSE);
}

static void
on_sheet_drag_begin (GtkGestureDrag *gesture, double x, double y, FmDrawer *drawer)
{
  drawer->drag_from = drawer->open;
}

static void
on_sheet_drag_update (GtkGestureDrag *gesture, double dx, double dy, FmDrawer *drawer)
{
  GdkEventSequence *sequence = gtk_gesture_single_get_current_sequence (GTK_GESTURE_SINGLE (gesture));

  if (gtk_gesture_get_sequence_state (GTK_GESTURE (gesture), sequence) != GTK_EVENT_SEQUENCE_CLAIMED
      && (fabs (dx) < TOUCH_SLOP || fabs (dy) > fabs (dx)))
    return;
  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  neo_anim_stop (drawer->anim);
  drawer->open = CLAMP (drawer->drag_from + dx / DRAWER_W, 0, 1);
  place (drawer);
}

static void
on_sheet_drag_end (GtkGestureDrag *gesture, double dx, double dy, FmDrawer *drawer)
{
  if (drawer->open > 0 && drawer->open < 1)
    fm_drawer_open (drawer, drawer->open > 0.5);
}

static void
on_edge_drag_begin (GtkGestureDrag *gesture, double x, double y, FmDrawer *drawer)
{
  FmShell *shell = drawer->shell;

  /* not over a dialog or a menu, nor while selecting or searching (the toolbar is not ☰ then) */
  if (x > EDGE_W || drawer->open > 0 || g_strcmp0 (gtk_stack_get_visible_child_name (GTK_STACK (shell->toolbar)),
                                                   "normal"))
    {
      gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_DENIED);
      return;
    }
}

static void
on_edge_drag_update (GtkGestureDrag *gesture, double dx, double dy, FmDrawer *drawer)
{
  if (dx < TOUCH_SLOP && drawer->open == 0)
    return;
  if (drawer->open == 0)
    fm_drawer_refresh (drawer);
  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  drawer->open = CLAMP (dx / DRAWER_W, 0, 1);
  place (drawer);
}

static void
on_edge_drag_end (GtkGestureDrag *gesture, double dx, double dy, FmDrawer *drawer)
{
  if (drawer->open > 0)
    fm_drawer_open (drawer, drawer->open > 0.3);
}

void
fm_drawer_attach_edge (FmDrawer *drawer, GtkWidget *content)
{
  GtkGesture *edge = gtk_gesture_drag_new ();

  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (edge), GTK_PHASE_CAPTURE);
  g_signal_connect (edge, "drag-begin", G_CALLBACK (on_edge_drag_begin), drawer);
  g_signal_connect (edge, "drag-update", G_CALLBACK (on_edge_drag_update), drawer);
  g_signal_connect (edge, "drag-end", G_CALLBACK (on_edge_drag_end), drawer);
  gtk_widget_add_controller (content, GTK_EVENT_CONTROLLER (edge));
}

/* ---- going somewhere ------------------------------------------------------------------------------------------------ */

static void
go (FmDrawer *drawer, GFile *location)
{
  fm_drawer_open (drawer, FALSE);
  fm_shell_open_location (drawer->shell, location);
}

static void
on_location_row (NeoPressable *row, FmDrawer *drawer)
{
  GFile *location = g_object_get_data (G_OBJECT (row), "fm-file");

  if (location)
    go (drawer, location);
  else
    {
      fm_drawer_open (drawer, FALSE);
      fm_shell_show_page (drawer->shell, FM_PAGE_HOME);
    }
}

/* ---- the rows ---------------------------------------------------------------------------------------------------- */

static GtkWidget *
row_new (FmDrawer *drawer, GtkWidget *content, GFile *location, const char *name, const char *css_class)
{
  GtkWidget *row = neo_pressable_new (content);
  GdkRGBA ripple = fm_rgba (FM_C_RIPPLE);

  gtk_widget_add_css_class (row, "fm-nav-row");
  if (css_class)
    gtk_widget_add_css_class (row, css_class);
  neo_pressable_set_ripple (NEO_PRESSABLE (row), NEO_RIPPLE_BOUNDED, 0);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (row), &ripple);
  if (location)
    g_object_set_data_full (G_OBJECT (row), "fm-file", g_object_ref (location), g_object_unref);
  gtk_accessible_update_property (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
  return row;
}

static GtkWidget *
line (const char *css_class)
{
  GtkWidget *line = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  gtk_widget_add_css_class (line, css_class);
  return line;
}

typedef struct {
  GtkWidget *description;
  GtkWidget *bar;
  double used;
} Storage;

static void
draw_storage_bar (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  double used = *(double *) data;

  /* the used part #3c87ee on #474f58: 1.33 dp (4 px) at the foot of its 2 dp, 38.67 dp under the row's top */
  double bar = MIN (height, 4 / 3.0);

  cairo_set_source_rgb (cr, 0x47 / 255.0, 0x4f / 255.0, 0x58 / 255.0);
  cairo_rectangle (cr, 0, height - bar, width, bar);
  cairo_fill (cr);
  cairo_set_source_rgb (cr, 0x3c / 255.0, 0x87 / 255.0, 0xee / 255.0);
  cairo_rectangle (cr, 0, height - bar, width * used, bar);
  cairo_fill (cr);
}

static void
on_storage_info (GObject *source, GAsyncResult *result, gpointer data)
{
  GtkWidget *row = data;
  g_autoptr(GFileInfo) info = g_file_query_filesystem_info_finish (G_FILE (source), result, NULL);
  Storage *storage = g_object_get_data (G_OBJECT (row), "fm-storage");

  if (storage && info && g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE)
      && g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE))
    {
      guint64 size = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
      guint64 free_space = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
      g_autofree char *percent = NULL;

      storage->used = size ? (double) (size - MIN (free_space, size)) / size : 0;
      percent = g_strdup_printf ("%d%%", (int) round (storage->used * 100));
      gtk_label_set_text (GTK_LABEL (storage->description), percent);
      gtk_widget_set_visible (storage->bar, TRUE);
      gtk_widget_queue_draw (storage->bar);
    }
  g_object_unref (row);
}

/* a fixed row (nav_fixed_list_item): the 32 dp icon, the name and the description at its end, and for a storage the
 * bar under them */
static GtkWidget *
fixed_row (FmDrawer *drawer, FmGlyph glyph, const char *name, GFile *location, gboolean storage)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *top = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *icon = fm_icon_new (glyph, 32);
  GtkWidget *label = fm_label (name, "fm-nav-name", 16, FALSE);
  GtkWidget *description = fm_label (NULL, "fm-nav-description", 14, FALSE);
  GtkWidget *row;

  gtk_widget_add_css_class (icon, "fm-nav-icon");
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_label_set_xalign (GTK_LABEL (description), 1);
  /* the description's digits 1 dp under the name's capitals, on its baseline (fmplus-5) */
  gtk_widget_set_valign (description, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (top), label);
  gtk_box_append (GTK_BOX (top), description);
  gtk_box_append (GTK_BOX (text), top);
  gtk_widget_add_css_class (text, "fm-nav-text");
  gtk_widget_set_hexpand (text, TRUE);
  /* the name centred; a storage's name stands 9 dp down with its bar 38.7 dp down (fmplus-5) */
  gtk_widget_set_valign (text, storage ? GTK_ALIGN_START : GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), icon);
  gtk_box_append (GTK_BOX (box), text);
  row = row_new (drawer, box, location, name, storage ? "fm-nav-storage" : "fm-nav-fixed");
  if (storage)
    gtk_widget_add_css_class (row, "fm-nav-fixed");
  g_object_set_data (G_OBJECT (row), "fm-description", description);
  if (storage)
    {
      Storage *info = g_new0 (Storage, 1);
      GtkWidget *bar = gtk_drawing_area_new ();

      info->description = description;
      info->bar = bar;
      gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (bar), 2);
      gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (bar), draw_storage_bar, &info->used, NULL);
      gtk_widget_add_css_class (bar, "fm-nav-bar");
      gtk_widget_set_visible (bar, FALSE);
      gtk_box_append (GTK_BOX (text), bar);
      g_object_set_data_full (G_OBJECT (row), "fm-storage", info, g_free);
      g_file_query_filesystem_info_async (location, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE ","
                                          G_FILE_ATTRIBUTE_FILESYSTEM_FREE, G_PRIORITY_LOW, drawer->cancellable,
                                          on_storage_info, g_object_ref (row));
    }
  g_signal_connect (row, "clicked", G_CALLBACK (on_location_row), drawer);
  return row;
}

/* the Recycle Bin's size, added up in a worker */
static void
trash_size_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  g_autoptr(GFile) trash = g_file_new_for_uri ("trash:///");
  FmCount count = { 0 };

  fm_count_tree (trash, cancellable, &count);
  g_task_return_int (task, (gssize) MIN (count.bytes, G_MAXSSIZE));
}

static void
on_trash_size (GObject *source, GAsyncResult *result, gpointer data)
{
  GtkWidget *description = data;
  g_autoptr(GError) error = NULL;
  gssize bytes = g_task_propagate_int (G_TASK (result), &error);

  if (!error)
    {
      g_autofree char *size = fm_format_size ((guint64) bytes);

      gtk_label_set_text (GTK_LABEL (description), size);
    }
  g_object_unref (description);
}

/* ---- ⋮ on a last-visited row (context_lastvisited) and a medium's Eject ----------------------------------------------- */

typedef struct {
  FmDrawer *drawer;
  char *root_uri;
  gboolean pinned;
} VisitedMenu;

static void
visited_menu_free (gpointer data)
{
  VisitedMenu *menu = data;

  g_free (menu->root_uri);
  g_free (menu);
}

static void
on_visited_chosen (int index, gpointer data)
{
  VisitedMenu *menu = data;

  if (index == 0)
    fm_state_pin_visited (menu->drawer->shell->state, menu->root_uri, !menu->pinned);
  else
    fm_state_remove_visited (menu->drawer->shell->state, menu->root_uri);
  fm_drawer_refresh (menu->drawer);
}

static void
on_visited_more (NeoPressable *more, FmDrawer *drawer)
{
  VisitedMenu *menu = g_object_get_data (G_OBJECT (more), "fm-menu");
  const char *labels[] = { menu->pinned ? _("Unpin") : _("Pin"), _("Remove") };

  fm_popup_show (GTK_WIDGET (more), labels, 2, FM_POPUP_AT, 0, 0, on_visited_chosen, menu);
}

static void
on_ejected (GObject *source, GAsyncResult *result, gpointer data)
{
  FmShell *shell = data;
  g_autoptr(GError) error = NULL;

  if (!g_mount_eject_with_operation_finish (G_MOUNT (source), result, &error)
      && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED))
    fm_shell_toast (shell, error->message);
}

static void
on_unmounted (GObject *source, GAsyncResult *result, gpointer data)
{
  FmShell *shell = data;
  g_autoptr(GError) error = NULL;

  if (!g_mount_unmount_with_operation_finish (G_MOUNT (source), result, &error)
      && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED))
    fm_shell_toast (shell, error->message);
}

static void
on_eject_chosen (int index, gpointer data)
{
  GMount *mount = data;
  FmShell *shell = g_object_get_data (G_OBJECT (mount), "fm-shell");
  g_autoptr(GMountOperation) op = g_mount_operation_new ();

  if (g_mount_can_eject (mount))
    g_mount_eject_with_operation (mount, G_MOUNT_UNMOUNT_NONE, op, NULL, on_ejected, shell);
  else
    g_mount_unmount_with_operation (mount, G_MOUNT_UNMOUNT_NONE, op, NULL, on_unmounted, shell);
}

static void
on_media_long_pressed (NeoPressable *row, FmDrawer *drawer)
{
  GMount *mount = g_object_get_data (G_OBJECT (row), "fm-mount");
  const char *labels[] = { _("Eject") };

  if (mount && (g_mount_can_eject (mount) || g_mount_can_unmount (mount)))
    {
      g_object_set_data (G_OBJECT (mount), "fm-shell", drawer->shell);
      fm_popup_show (GTK_WIDGET (row), labels, 1, FM_POPUP_AT, 46, 40, on_eject_chosen, mount);
    }
}

/* ---- the Locations tab ------------------------------------------------------------------------------------------------- */

static void
build_locations (FmDrawer *drawer)
{
  FmShell *shell = drawer->shell;
  GtkWidget *box = drawer->locations, *child, *row;
  g_autoptr(GFile) home = g_file_new_for_path (g_get_home_dir ());
  g_autoptr(GFile) trash = g_file_new_for_uri ("trash:///");
  GList *mounts = fm_shell_mounts (shell);
  GPtrArray *visited = fm_state_visited (shell->state);
  g_autoptr(GTask) task = NULL;

  while ((child = gtk_widget_get_first_child (box)))
    gtk_box_remove (GTK_BOX (box), child);
  gtk_box_append (GTK_BOX (box), fixed_row (drawer, FM_GLYPH_HOME, _("Home"), NULL, FALSE));
  gtk_box_append (GTK_BOX (box), line ("fm-nav-divider"));
  gtk_box_append (GTK_BOX (box), fixed_row (drawer, FM_GLYPH_STORAGE, _("Main storage"), home, TRUE));
  gtk_box_append (GTK_BOX (box), line ("fm-nav-divider"));
  for (GList *l = mounts; l; l = l->next)
    {
      g_autoptr(GFile) root = NULL;
      g_autofree char *label = NULL;
      FmGlyph glyph;

      if (!fm_mount_is_media (l->data))
        continue;
      root = g_mount_get_root (l->data);
      label = fm_mount_label (l->data, &glyph);
      row = fixed_row (drawer, glyph, label, root, g_file_is_native (root));
      g_object_set_data_full (G_OBJECT (row), "fm-mount", g_object_ref (l->data), g_object_unref);
      g_signal_connect (row, "long-pressed", G_CALLBACK (on_media_long_pressed), drawer);
      gtk_box_append (GTK_BOX (box), row);
      gtk_box_append (GTK_BOX (box), line ("fm-nav-divider"));
    }
  g_list_free_full (mounts, g_object_unref);
  if (g_settings_get_boolean (shell->settings, "show-system"))
    {
      g_autoptr(GFile) slash = g_file_new_for_path ("/");

      gtk_box_append (GTK_BOX (box), fixed_row (drawer, FM_GLYPH_SYSTEM, _("System"), slash, TRUE));
      gtk_box_append (GTK_BOX (box), line ("fm-nav-divider"));
    }
  row = fixed_row (drawer, FM_GLYPH_TRASH, _("Recycle Bin"), trash, FALSE);
  gtk_box_append (GTK_BOX (box), row);
  task = g_task_new (NULL, drawer->cancellable, on_trash_size,
                     g_object_ref (g_object_get_data (G_OBJECT (row), "fm-description")));
  g_task_set_priority (task, G_PRIORITY_LOW);
  g_task_run_in_thread (task, trash_size_thread);

  if (!visited->len)
    return;
  /* the last fixed row keeps its divider's 1 dp, unpainted */
  gtk_box_append (GTK_BOX (box), line ("fm-nav-gap"));
  gtk_box_append (GTK_BOX (box), line ("fm-nav-separator"));
  /* the last-visited locations (nav_opened_list_item) */
  for (guint i = 0; i < visited->len; i++)
    {
      FmVisited *entry = visited->pdata[i];
      g_autoptr(GFile) location = g_file_new_for_uri (entry->uri);
      g_autoptr(FmPlace) where = fm_shell_place_for (shell, location);
      GtkWidget *line_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      GtkWidget *text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
      GtkWidget *icon = fm_icon_new (where->glyph, 32);
      /* a smaller ⋮ than the toolbar's: 12 dp of dots, centred 254 dp in (fmplus-5) */
      GtkWidget *more = fm_icon_button_sized ("fm-more-vert-symbolic", _("More options"), 48, 48, 18);
      GtkWidget *name_line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      GtkWidget *name = fm_label (where->kind == FM_PLACE_REMOTE ? where->name : entry->name, "fm-nav-name", 16,
                                  FALSE);
      VisitedMenu *menu = g_new0 (VisitedMenu, 1);
      const char *title = gtk_label_get_text (GTK_LABEL (name));

      gtk_widget_add_css_class (icon, "fm-nav-icon");
      gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (name_line), name);
      /* a pinned one stays whatever else is visited: a pin after its name */
      if (entry->pinned)
        {
          GtkWidget *pin = gtk_image_new_from_icon_name ("fm-push-pin-symbolic");

          gtk_image_set_pixel_size (GTK_IMAGE (pin), 14);
          gtk_widget_add_css_class (pin, "fm-nav-pin");
          gtk_box_append (GTK_BOX (name_line), pin);
        }
      gtk_box_append (GTK_BOX (text), name_line);
      gtk_box_append (GTK_BOX (text), fm_label (entry->path, "fm-nav-path", 14, FALSE));
      gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
      gtk_widget_set_hexpand (text, TRUE);
      gtk_widget_add_css_class (text, "fm-nav-opened-text");
      menu->drawer = drawer;
      menu->root_uri = g_strdup (entry->root_uri);
      menu->pinned = entry->pinned;
      g_object_set_data_full (G_OBJECT (more), "fm-menu", menu, visited_menu_free);
      g_signal_connect (more, "clicked", G_CALLBACK (on_visited_more), drawer);
      gtk_widget_add_css_class (more, "fm-nav-more");
      gtk_box_append (GTK_BOX (line_box), icon);
      gtk_box_append (GTK_BOX (line_box), text);
      gtk_box_append (GTK_BOX (line_box), more);
      row = row_new (drawer, line_box, location, title, "fm-nav-opened");
      g_signal_connect (row, "clicked", G_CALLBACK (on_location_row), drawer);
      gtk_box_append (GTK_BOX (box), row);
      gtk_box_append (GTK_BOX (box), line ("fm-nav-divider"));
    }
}

/* ---- the Bookmarks tab ------------------------------------------------------------------------------------------- */

static void build_bookmarks (FmDrawer *drawer);

static void
on_bookmark_renamed (const char *text, gpointer data)
{
  FmDrawer *drawer = g_object_get_data (G_OBJECT (data), "fm-drawer");
  guint index = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (data), "fm-index"));

  if (text)
    {
      fm_state_rename_bookmark (drawer->shell->state, index, text);
      fm_shell_toast (drawer->shell, _("Renamed successfully"));
      build_bookmarks (drawer);
    }
  g_object_unref (data);
}

static const char *
check_bookmark_name (const char *text, gpointer data)
{
  g_autofree char *stripped = g_strstrip (g_strdup (text));

  return *stripped ? NULL : _("File name cannot be empty.");
}

static void
remove_bookmark (FmDrawer *drawer, guint index)
{
  GPtrArray *bookmarks = fm_state_bookmarks (drawer->shell->state);
  g_autofree char *message = NULL;

  if (index >= bookmarks->len)
    return;
  message = g_strdup_printf (_("Bookmark [%s] deleted successfully"), ((FmBookmark *) bookmarks->pdata[index])->name);
  fm_state_remove_bookmark (drawer->shell->state, index);
  fm_shell_toast (drawer->shell, message);
  build_bookmarks (drawer);
}

static void
on_bookmark_chosen (int choice, gpointer data)
{
  GtkWidget *row = data;
  FmDrawer *drawer = g_object_get_data (G_OBJECT (row), "fm-drawer");
  guint index = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (row), "fm-index"));
  GPtrArray *bookmarks = fm_state_bookmarks (drawer->shell->state);

  if (index >= bookmarks->len)
    return;
  if (choice == 0)
    fm_text_dialog (drawer->shell->window, _("Rename"), ((FmBookmark *) bookmarks->pdata[index])->name, -1,
                    check_bookmark_name, on_bookmark_renamed, g_object_ref (row));
  else
    remove_bookmark (drawer, index);
}

static void
on_bookmark_long_pressed (NeoPressable *row, FmDrawer *drawer)
{
  const char *labels[] = { _("Rename"), _("Remove") };

  fm_popup_show (GTK_WIDGET (row), labels, 2, FM_POPUP_AT, 46, 40, on_bookmark_chosen, row);
}

static void
on_bookmark_remove (NeoPressable *button, FmDrawer *drawer)
{
  remove_bookmark (drawer, GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "fm-index")));
}

static void
on_bookmarks_edit (NeoPressable *button, FmDrawer *drawer)
{
  drawer->editing_bookmarks = !drawer->editing_bookmarks;
  build_bookmarks (drawer);
}

static void
build_bookmarks (FmDrawer *drawer)
{
  GtkWidget *box = drawer->bookmarks, *child, *edit;
  GPtrArray *bookmarks = fm_state_bookmarks (drawer->shell->state);

  while ((child = gtk_widget_get_first_child (box)))
    gtk_box_remove (GTK_BOX (box), child);
  for (guint i = 0; i < bookmarks->len; i++)
    {
      FmBookmark *bookmark = bookmarks->pdata[i];
      g_autoptr(GFile) location = g_file_new_for_uri (bookmark->uri);
      g_autoptr(FmPlace) where = fm_shell_place_for (drawer->shell, location);
      g_autofree char *shown = fm_display_path (location);
      GtkWidget *line_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      GtkWidget *text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
      GtkWidget *icons = gtk_overlay_new ();
      GtkWidget *icon = fm_icon_new (fm_glyph_for_folder (location), 32);
      GtkWidget *row;

      gtk_overlay_set_child (GTK_OVERLAY (icons), icon);
      /* a remote's bookmark wears its location's 16 dp icon */
      if (where->kind != FM_PLACE_STORAGE)
        {
          GtkWidget *sub = fm_icon_new (where->glyph, 16);

          gtk_widget_set_halign (sub, GTK_ALIGN_END);
          gtk_widget_set_valign (sub, GTK_ALIGN_END);
          gtk_overlay_add_overlay (GTK_OVERLAY (icons), sub);
        }
      gtk_widget_add_css_class (icons, "fm-nav-icon");
      gtk_widget_set_valign (icons, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (text), fm_label (bookmark->name, "fm-nav-name", 16, FALSE));
      gtk_box_append (GTK_BOX (text), fm_label (shown, "fm-nav-location", 12, FALSE));
      gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
      gtk_widget_set_hexpand (text, TRUE);
      gtk_box_append (GTK_BOX (line_box), icons);
      gtk_box_append (GTK_BOX (line_box), text);
      if (drawer->editing_bookmarks)
        {
          GtkWidget *remove = fm_icon_button ("fm-close-symbolic", _("Remove"), 48, 48);

          g_object_set_data (G_OBJECT (remove), "fm-index", GUINT_TO_POINTER (i));
          g_signal_connect (remove, "clicked", G_CALLBACK (on_bookmark_remove), drawer);
          gtk_box_append (GTK_BOX (line_box), remove);
        }
      row = row_new (drawer, line_box, location, bookmark->name, "fm-nav-bookmark");
      g_object_set_data (G_OBJECT (row), "fm-drawer", drawer);
      g_object_set_data (G_OBJECT (row), "fm-index", GUINT_TO_POINTER (i));
      g_signal_connect (row, "clicked", G_CALLBACK (on_location_row), drawer);
      g_signal_connect (row, "long-pressed", G_CALLBACK (on_bookmark_long_pressed), drawer);
      gtk_box_append (GTK_BOX (box), row);
    }
  gtk_box_append (GTK_BOX (box), line ("fm-nav-separator-2"));
  edit = fm_text_button (drawer->editing_bookmarks ? _("Done") : _("Edit"));
  gtk_widget_add_css_class (edit, "fm-nav-button");
  gtk_widget_set_halign (edit, GTK_ALIGN_START);
  g_signal_connect (edit, "clicked", G_CALLBACK (on_bookmarks_edit), drawer);
  gtk_widget_set_sensitive (edit, bookmarks->len > 0);
  gtk_box_append (GTK_BOX (box), edit);
}

/* ---- the History tab --------------------------------------------------------------------------------------------- */

static void
on_history_clear (NeoPressable *button, FmDrawer *drawer)
{
  fm_state_clear_history (drawer->shell->state);
  fm_drawer_refresh (drawer);
}

static void
build_history (FmDrawer *drawer)
{
  GtkWidget *box = drawer->history, *child, *clear;
  GPtrArray *history = fm_state_history (drawer->shell->state);

  while ((child = gtk_widget_get_first_child (box)))
    gtk_box_remove (GTK_BOX (box), child);
  if (!history->len)
    {
      GtkWidget *empty = fm_label (_("No history"), "fm-nav-empty", 16, FALSE);

      gtk_label_set_xalign (GTK_LABEL (empty), 0.5f);
      gtk_box_append (GTK_BOX (box), empty);
    }
  for (guint i = 0; i < history->len; i++)
    {
      FmHistoryEntry *entry = history->pdata[i];
      g_autoptr(GFile) location = g_file_new_for_uri (entry->uri);
      g_autoptr(FmPlace) where = fm_shell_place_for (drawer->shell, location);
      g_autofree char *path = fm_relative_path (where->root, location);
      GtkWidget *rows = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
      GtkWidget *band = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      GtkWidget *line_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      GtkWidget *icon = fm_icon_new (fm_glyph_for_folder (location), 32);
      GtkWidget *row;

      /* the location's band: its 16 dp icon and name on #303030 */
      gtk_box_append (GTK_BOX (band), fm_icon_new (where->glyph, 16));
      gtk_box_append (GTK_BOX (band), fm_label (*entry->root_name ? entry->root_name : where->name,
                                                "fm-nav-band-text", 12, FALSE));
      gtk_widget_add_css_class (band, "fm-nav-band");
      gtk_widget_add_css_class (icon, "fm-nav-icon");
      gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (line_box), icon);
      gtk_box_append (GTK_BOX (line_box), fm_label (path, "fm-nav-path-main", 14, FALSE));
      gtk_widget_add_css_class (line_box, "fm-nav-history-line");
      gtk_box_append (GTK_BOX (rows), band);
      gtk_box_append (GTK_BOX (rows), line_box);
      row = row_new (drawer, rows, location, path, "fm-nav-history");
      g_signal_connect (row, "clicked", G_CALLBACK (on_location_row), drawer);
      gtk_box_append (GTK_BOX (box), row);
    }
  gtk_box_append (GTK_BOX (box), line ("fm-nav-separator-2"));
  clear = fm_text_button (_("Clear"));
  gtk_widget_add_css_class (clear, "fm-nav-button");
  gtk_widget_set_halign (clear, GTK_ALIGN_START);
  gtk_widget_set_sensitive (clear, history->len > 0);
  g_signal_connect (clear, "clicked", G_CALLBACK (on_history_clear), drawer);
  gtk_box_append (GTK_BOX (box), clear);
}

/* ---- the tabs ------------------------------------------------------------------------------------------------------ */

static void
draw_tab_line (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  FmDrawer *drawer = data;
  double w = width / (double) N_TABS;

  /* nav_tab_separator: 1 dp #666666 at the bottom; the 4 dp accent indicator over it, under the selected tab */
  cairo_set_source_rgb (cr, 0x66 / 255.0, 0x66 / 255.0, 0x66 / 255.0);
  cairo_rectangle (cr, 0, height - 1, width, 1);
  cairo_fill (cr);
  cairo_set_source_rgb (cr, 0x80 / 255.0, 0xcb / 255.0, 0xc4 / 255.0);
  cairo_rectangle (cr, round (w * drawer->tab), height - 4, round (w), 4);
  cairo_fill (cr);
}

static void
select_tab (FmDrawer *drawer, Tab tab)
{
  static const char *const names[] = { "locations", "bookmarks", "history" };

  drawer->tab = tab;
  for (int i = 0; i < N_TABS; i++)
    if (i == (int) tab)
      gtk_widget_add_css_class (drawer->tabs[i], "selected");
    else
      gtk_widget_remove_css_class (drawer->tabs[i], "selected");
  gtk_stack_set_visible_child_name (GTK_STACK (drawer->pages), names[tab]);
  gtk_widget_queue_draw (drawer->tab_line);
}

static void
on_tab (NeoPressable *tab, FmDrawer *drawer)
{
  select_tab (drawer, GPOINTER_TO_INT (g_object_get_data (G_OBJECT (tab), "fm-tab")));
}

static GtkWidget *
page (GtkWidget **box, const char *css_class)
{
  GtkWidget *scroll = spongy_scroll_view_new ();

  *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (*box, css_class);
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), *box);
  gtk_widget_set_vexpand (scroll, TRUE);
  return scroll;
}

static gboolean
on_sheet_key (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, FmDrawer *drawer)
{
  if (keyval != GDK_KEY_Escape && keyval != GDK_KEY_Back)
    return FALSE;
  fm_drawer_open (drawer, FALSE);
  return TRUE;
}

void
fm_drawer_refresh (FmDrawer *drawer)
{
  /* a refresh while sizes are still being asked drops those */
  if (drawer->cancellable)
    g_cancellable_cancel (drawer->cancellable);
  g_clear_object (&drawer->cancellable);
  drawer->cancellable = g_cancellable_new ();
  build_locations (drawer);
  build_bookmarks (drawer);
  build_history (drawer);
}

FmDrawer *
fm_drawer_new (FmShell *shell)
{
  static const char *const icons[] = { "fm-folder-fill-symbolic", "fm-star-fill-symbolic", "fm-schedule-symbolic" };
  const char *names[] = { _("Locations"), _("Bookmarks"), _("History") };
  FmDrawer *drawer = g_new0 (FmDrawer, 1);
  FmDrawerLayer *layer = g_object_new (FM_TYPE_DRAWER_LAYER, NULL);
  GtkWidget *sheet = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *bar = gtk_overlay_new ();
  GtkWidget *tabs = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkGesture *press = gtk_gesture_click_new ();
  GtkGesture *drag = gtk_gesture_drag_new ();
  GtkEventController *keys = gtk_event_controller_key_new ();

  drawer->shell = shell;
  drawer->layer = GTK_WIDGET (layer);
  layer->drawer = drawer;
  drawer->anim = neo_anim_new (drawer->layer, on_anim, drawer);

  drawer->scrim = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (drawer->scrim, "fm-drawer-scrim");
  g_signal_connect (press, "pressed", G_CALLBACK (on_scrim_pressed), drawer);
  gtk_widget_add_controller (drawer->scrim, GTK_EVENT_CONTROLLER (press));
  gtk_widget_set_parent (drawer->scrim, drawer->layer);

  /* the tab bar: three equal tabs of a 24 dp glyph, the line and indicator drawn under them */
  gtk_box_set_homogeneous (GTK_BOX (tabs), TRUE);
  for (int i = 0; i < N_TABS; i++)
    {
      GtkWidget *image = gtk_image_new_from_icon_name (icons[i]);
      GdkRGBA ripple = fm_rgba (FM_C_RIPPLE);

      gtk_image_set_pixel_size (GTK_IMAGE (image), 24);
      drawer->tabs[i] = neo_pressable_new (image);
      gtk_widget_add_css_class (drawer->tabs[i], "fm-nav-tab");
      neo_pressable_set_ripple (NEO_PRESSABLE (drawer->tabs[i]), NEO_RIPPLE_BOUNDED, 0);
      neo_pressable_set_ripple_color (NEO_PRESSABLE (drawer->tabs[i]), &ripple);
      g_object_set_data (G_OBJECT (drawer->tabs[i]), "fm-tab", GINT_TO_POINTER (i));
      g_signal_connect (drawer->tabs[i], "clicked", G_CALLBACK (on_tab), drawer);
      gtk_accessible_update_property (GTK_ACCESSIBLE (drawer->tabs[i]), GTK_ACCESSIBLE_PROPERTY_LABEL, names[i], -1);
      gtk_box_append (GTK_BOX (tabs), drawer->tabs[i]);
    }
  drawer->tab_line = gtk_drawing_area_new ();
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (drawer->tab_line), draw_tab_line, drawer, NULL);
  gtk_widget_set_can_target (drawer->tab_line, FALSE);
  gtk_overlay_set_child (GTK_OVERLAY (bar), tabs);
  gtk_overlay_add_overlay (GTK_OVERLAY (bar), drawer->tab_line);
  gtk_widget_add_css_class (bar, "fm-nav-tabs");
  gtk_box_append (GTK_BOX (sheet), bar);

  drawer->pages = gtk_stack_new ();
  gtk_stack_add_named (GTK_STACK (drawer->pages), page (&drawer->locations, "fm-nav-locations"), "locations");
  gtk_stack_add_named (GTK_STACK (drawer->pages), page (&drawer->bookmarks, "fm-nav-bookmarks"), "bookmarks");
  gtk_stack_add_named (GTK_STACK (drawer->pages), page (&drawer->history, "fm-nav-history-list"), "history");
  gtk_widget_set_vexpand (drawer->pages, TRUE);
  gtk_box_append (GTK_BOX (sheet), drawer->pages);
  gtk_widget_add_css_class (sheet, "fm-drawer-sheet");
  gtk_widget_set_focusable (sheet, TRUE);
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_sheet_key), drawer);
  gtk_widget_add_controller (sheet, keys);
  drawer->sheet = sheet;
  gtk_widget_set_parent (sheet, drawer->layer);

  /* a horizontal drag anywhere on it closes it (claimed only past the slop, so rows still take taps and scrolls) */
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (drag), GTK_PHASE_CAPTURE);
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_sheet_drag_begin), drawer);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_sheet_drag_update), drawer);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_sheet_drag_end), drawer);
  gtk_widget_add_controller (drawer->layer, GTK_EVENT_CONTROLLER (drag));
  gtk_accessible_update_property (GTK_ACCESSIBLE (drawer->layer), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Files"), -1);
  gtk_widget_set_visible (drawer->layer, FALSE);
  select_tab (drawer, TAB_LOCATIONS);
  return drawer;
}

GtkWidget *
fm_drawer_get_widget (FmDrawer *drawer)
{
  return drawer->layer;
}

void
fm_drawer_free (FmDrawer *drawer)
{
  if (drawer->cancellable)
    g_cancellable_cancel (drawer->cancellable);
  g_clear_object (&drawer->cancellable);
  g_clear_pointer (&drawer->anim, neo_anim_free);
  g_free (drawer);
}
