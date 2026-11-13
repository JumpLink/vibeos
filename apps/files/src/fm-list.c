/* The file list (fm-list.h). */
#include "fm-list.h"
#include "fm-icon.h"
#include "fm-ui.h"
#include "fm-util.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gi18n.h>
#include <neo.h>
#include <spongy-gtk.h>

#define THUMB_PX       132      /* device px, a list row's icon on the phone (a tile's decodes at twice this) */
#define THUMB_MAX_SIZE (40 * 1000 * 1000)

struct _FmList {
  GtkWidget parent;
  GtkWidget *overlay, *scroll, *message;
  GtkWidget *view;              /* the one of the two that shows */
  GtkWidget *list_view, *grid_view;
  gboolean grid;
  GListStore *store;
  GtkFilterListModel *filtered;
  GtkSelectionModel *selection;
  GtkCustomFilter *filter;
  char *needle;                 /* the filter, case-folded */
  gboolean selecting, thumbnails;
  /* thumbnails: one decoded at a time, the last asked first (the rows on screen) */
  GQueue *thumb_queue;
  gboolean thumb_busy;
  GCancellable *thumb_cancel;
};

G_DEFINE_FINAL_TYPE (FmList, fm_list, GTK_TYPE_WIDGET)

enum { ACTIVATE, LONG_PRESS, SELECTION_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void thumb_request (FmList *self, FmItem *item);

/* ---- a row ------------------------------------------------------------------------------------------------------ */

typedef struct {
  GtkWidget *pressable, *icon, *name, *count, *date, *where, *check;
  FmItem *item;
  gulong changed;
  FmList *list;
} Row;

/* a file's icon: its content type's in the theme, unless the type says nothing (then the name's glyph) */
static GIcon *
content_icon (const char *content_type)
{
  static GHashTable *icons;
  GIcon *icon;

  if (!content_type || g_content_type_is_unknown (content_type))
    return NULL;
  if (!icons)
    icons = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  if (!(icon = g_hash_table_lookup (icons, content_type)))
    g_hash_table_insert (icons, g_strdup (content_type), icon = g_content_type_get_icon (content_type));
  return icon;
}

static void
set_text (GtkWidget *label, const char *text)
{
  if (label)
    gtk_label_set_text (GTK_LABEL (label), text ? text : "");
}

static void
row_fill (Row *row)
{
  FmItem *item = row->item;
  gboolean folder = fm_item_is_folder (item);
  GdkTexture *thumb = g_object_get_data (G_OBJECT (item), "fm-thumb");
  g_autofree char *count = NULL;
  g_autofree char *date = NULL;
  FmGlyph glyph;

  if (folder)
    {
      g_autoptr(GFile) location = fm_item_get_location (item);
      g_autofree char *scheme = g_file_get_uri_scheme (location);

      /* a share or a host on the network is a remote folder; a folder is a folder */
      if (item->type == G_FILE_TYPE_MOUNTABLE || (item->type == G_FILE_TYPE_SHORTCUT && fm_scheme_is_server (scheme)))
        glyph = fm_glyph_for_scheme (scheme);
      else
        glyph = fm_glyph_for_folder (item->file);
      count = item->count >= 0 ? fm_format_items (item->count)
                               : item->count == FM_COUNT_UNKNOWN ? fm_format_items (-1) : g_strdup ("");
    }
  else
    {
      glyph = fm_glyph_for_file (item->content_type, item->name);
      count = fm_format_size (item->size);
    }
  fm_icon_set_glyph (FM_ICON (row->icon), glyph);
  if (!folder)
    fm_icon_set_gicon (FM_ICON (row->icon), content_icon (item->content_type));
  fm_icon_set_texture (FM_ICON (row->icon), row->list->thumbnails ? thumb : NULL);
  fm_icon_set_dimmed (FM_ICON (row->icon), item->hidden);
  if (row->list->thumbnails && glyph == FM_GLYPH_IMAGE && !thumb)
    thumb_request (row->list, item);
  set_text (row->name, item->name);
  set_text (row->count, count);
  /* the Recycle Bin dates its rows by the deletion */
  if (item->deleted || item->mtime)
    date = fm_format_date (item->deleted ? item->deleted : item->mtime);
  set_text (row->date, date);
  set_text (row->where, item->where);
  if (row->where)
    gtk_widget_set_visible (row->where, item->where != NULL);
  gtk_widget_set_visible (row->check, row->list->selecting);
  fm_check_set_active (row->check, item->selected);
  if (item->selected && row->list->selecting)
    gtk_widget_add_css_class (row->pressable, "selected");
  else
    gtk_widget_remove_css_class (row->pressable, "selected");
}

static void
on_item_changed (FmItem *item, Row *row)
{
  row_fill (row);
}

static void
toggle (FmList *self, FmItem *item)
{
  item->selected = !item->selected;
  fm_item_changed (item);
  g_signal_emit (self, signals[SELECTION_CHANGED], 0);
}

static void
on_row_clicked (NeoPressable *pressable, Row *row)
{
  if (!row->item)
    return;
  if (row->list->selecting)
    toggle (row->list, row->item);
  else
    g_signal_emit (row->list, signals[ACTIVATE], 0, row->item);
}

static void
on_row_long_pressed (NeoPressable *pressable, Row *row)
{
  graphene_point_t at;

  if (!row->item)
    return;
  if (!gtk_widget_compute_point (GTK_WIDGET (pressable), GTK_WIDGET (row->list),
                                 &GRAPHENE_POINT_INIT (gtk_widget_get_width (GTK_WIDGET (pressable)) / 2.0f, 0), &at))
    at = GRAPHENE_POINT_INIT (0, 0);
  if (row->list->selecting)
    toggle (row->list, row->item);
  else
    g_signal_emit (row->list, signals[LONG_PRESS], 0, row->item, (double) at.x, (double) at.y);
}

static void
row_free (gpointer data)
{
  Row *row = data;

  if (row->item && row->changed)
    g_signal_handler_disconnect (row->item, row->changed);
  g_free (row);
}

static void
on_setup (GtkSignalListItemFactory *factory, GtkListItem *list_item, FmList *self)
{
  Row *row = g_new0 (Row, 1);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *sub = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GdkRGBA ripple = fm_rgba (FM_C_RIPPLE);

  row->list = self;
  row->icon = fm_icon_new (FM_GLYPH_FOLDER, 44);
  gtk_widget_add_css_class (row->icon, "fm-row-icon");
  gtk_widget_set_valign (row->icon, GTK_ALIGN_CENTER);
  row->name = fm_label (NULL, "fm-row-name", 18, TRUE);
  row->count = fm_label (NULL, "fm-row-sub", 14, FALSE);
  row->date = fm_label (NULL, "fm-row-sub", 14, FALSE);
  row->where = fm_label (NULL, "fm-row-where", 14, FALSE);
  gtk_label_set_ellipsize (GTK_LABEL (row->where), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_xalign (GTK_LABEL (row->date), 1);
  gtk_widget_set_hexpand (row->count, TRUE);
  gtk_box_append (GTK_BOX (sub), row->count);
  gtk_box_append (GTK_BOX (sub), row->date);
  gtk_box_append (GTK_BOX (text), row->name);
  gtk_box_append (GTK_BOX (text), sub);
  gtk_box_append (GTK_BOX (text), row->where);
  gtk_widget_add_css_class (text, "fm-row-text");
  gtk_widget_set_hexpand (text, TRUE);
  gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
  row->check = fm_check_new (FALSE);
  gtk_widget_add_css_class (row->check, "fm-row-check");
  gtk_widget_set_valign (row->check, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), row->icon);
  gtk_box_append (GTK_BOX (box), text);
  gtk_box_append (GTK_BOX (box), row->check);
  row->pressable = neo_pressable_new (box);
  gtk_widget_add_css_class (row->pressable, "fm-row");
  neo_pressable_set_ripple (NEO_PRESSABLE (row->pressable), NEO_RIPPLE_BOUNDED, 0);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (row->pressable), &ripple);
  g_signal_connect (row->pressable, "clicked", G_CALLBACK (on_row_clicked), row);
  g_signal_connect (row->pressable, "long-pressed", G_CALLBACK (on_row_long_pressed), row);
  g_object_set_data_full (G_OBJECT (row->pressable), "fm-row", row, row_free);
  gtk_list_item_set_child (list_item, row->pressable);
  gtk_list_item_set_activatable (list_item, FALSE);
  gtk_list_item_set_selectable (list_item, FALSE);
  gtk_list_item_set_focusable (list_item, FALSE);
}

/* a grid tile: the icon, the name under it, centred, at most two lines; the check box over the icon's top end
 * while selecting */
static void
on_setup_tile (GtkSignalListItemFactory *factory, GtkListItem *list_item, FmList *self)
{
  Row *row = g_new0 (Row, 1);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *frame = gtk_overlay_new ();
  GdkRGBA ripple = fm_rgba (FM_C_RIPPLE);

  row->list = self;
  row->icon = fm_icon_new (FM_GLYPH_FOLDER, 64);
  gtk_widget_add_css_class (row->icon, "fm-grid-tile-icon");
  gtk_widget_set_halign (row->icon, GTK_ALIGN_CENTER);
  gtk_overlay_set_child (GTK_OVERLAY (frame), row->icon);
  row->check = fm_check_new (FALSE);
  gtk_widget_add_css_class (row->check, "fm-grid-tile-check");
  gtk_widget_set_halign (row->check, GTK_ALIGN_END);
  gtk_widget_set_valign (row->check, GTK_ALIGN_START);
  gtk_overlay_add_overlay (GTK_OVERLAY (frame), row->check);
  row->name = fm_label (NULL, "fm-grid-tile-name", 14, TRUE);
  gtk_label_set_lines (GTK_LABEL (row->name), 2);
  gtk_label_set_ellipsize (GTK_LABEL (row->name), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign (GTK_LABEL (row->name), 0.5f);
  gtk_label_set_justify (GTK_LABEL (row->name), GTK_JUSTIFY_CENTER);
  gtk_widget_set_valign (row->name, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (box), frame);
  gtk_box_append (GTK_BOX (box), row->name);
  row->pressable = neo_pressable_new (box);
  gtk_widget_add_css_class (row->pressable, "fm-grid-tile");
  neo_pressable_set_ripple (NEO_PRESSABLE (row->pressable), NEO_RIPPLE_BOUNDED, 0);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (row->pressable), &ripple);
  g_signal_connect (row->pressable, "clicked", G_CALLBACK (on_row_clicked), row);
  g_signal_connect (row->pressable, "long-pressed", G_CALLBACK (on_row_long_pressed), row);
  g_object_set_data_full (G_OBJECT (row->pressable), "fm-row", row, row_free);
  gtk_list_item_set_child (list_item, row->pressable);
  gtk_list_item_set_activatable (list_item, FALSE);
  gtk_list_item_set_selectable (list_item, FALSE);
  gtk_list_item_set_focusable (list_item, FALSE);
}

static void
on_bind (GtkSignalListItemFactory *factory, GtkListItem *list_item, FmList *self)
{
  Row *row = g_object_get_data (G_OBJECT (gtk_list_item_get_child (list_item)), "fm-row");
  FmItem *item = gtk_list_item_get_item (list_item);

  row->item = item;
  row->changed = g_signal_connect (item, "changed", G_CALLBACK (on_item_changed), row);
  gtk_accessible_update_property (GTK_ACCESSIBLE (row->pressable), GTK_ACCESSIBLE_PROPERTY_LABEL, item->name, -1);
  row_fill (row);
}

static void
on_unbind (GtkSignalListItemFactory *factory, GtkListItem *list_item, FmList *self)
{
  Row *row = g_object_get_data (G_OBJECT (gtk_list_item_get_child (list_item)), "fm-row");

  if (row->item && row->changed)
    g_signal_handler_disconnect (row->item, row->changed);
  row->changed = 0;
  row->item = NULL;
}

/* ---- thumbnails ------------------------------------------------------------------------------------------------- */

static void
thumb_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  GFile *file = data;
  g_autofree char *path = g_file_get_path (file);
  g_autoptr(GError) error = NULL;
  g_autoptr(GdkPixbuf) pixbuf = NULL, oriented = NULL;
  GBytes *bytes;
  GdkTexture *texture;

  if (!path || g_cancellable_is_cancelled (cancellable))
    {
      g_task_return_pointer (task, NULL, NULL);
      return;
    }
  /* the loaders decode JPEGs at the reduced scale straight away */
  pixbuf = gdk_pixbuf_new_from_file_at_scale (path, THUMB_PX * 2, THUMB_PX * 2, TRUE, &error);
  if (!pixbuf)
    {
      g_task_return_pointer (task, NULL, NULL);
      return;
    }
  oriented = gdk_pixbuf_apply_embedded_orientation (pixbuf);
  bytes = g_bytes_new (gdk_pixbuf_read_pixels (oriented),
                       (gsize) gdk_pixbuf_get_rowstride (oriented) * (gdk_pixbuf_get_height (oriented) - 1)
                           + gdk_pixbuf_get_width (oriented) * gdk_pixbuf_get_n_channels (oriented));
  texture = gdk_memory_texture_new (gdk_pixbuf_get_width (oriented), gdk_pixbuf_get_height (oriented),
                                    gdk_pixbuf_get_has_alpha (oriented) ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
                                    bytes, gdk_pixbuf_get_rowstride (oriented));
  g_bytes_unref (bytes);
  g_task_return_pointer (task, texture, g_object_unref);
}

static void thumb_next (FmList *self);

static void
on_thumb (GObject *source, GAsyncResult *result, gpointer data)
{
  FmItem *item = data;
  g_autoptr(GdkTexture) texture = g_task_propagate_pointer (G_TASK (result), NULL);
  FmList *self = g_object_get_data (G_OBJECT (result), "fm-list");

  if (texture)
    {
      g_object_set_data_full (G_OBJECT (item), "fm-thumb", g_object_ref (texture), g_object_unref);
      fm_item_changed (item);
    }
  g_object_unref (item);
  if (self)
    {
      self->thumb_busy = FALSE;
      thumb_next (self);
      g_object_unref (self);
    }
}

static void
thumb_next (FmList *self)
{
  FmItem *item;
  g_autoptr(GTask) task = NULL;

  if (self->thumb_busy || !(item = g_queue_pop_tail (self->thumb_queue)))
    return;
  self->thumb_busy = TRUE;
  task = g_task_new (NULL, self->thumb_cancel, on_thumb, item);
  g_object_set_data (G_OBJECT (task), "fm-list", g_object_ref (self));
  g_task_set_task_data (task, g_object_ref (item->file), g_object_unref);
  g_task_set_priority (task, G_PRIORITY_LOW);
  g_task_run_in_thread (task, thumb_thread);
}

static void
thumb_request (FmList *self, FmItem *item)
{
  /* local files only: a remote image would be read whole over the network for its thumbnail */
  if (g_object_get_data (G_OBJECT (item), "fm-thumb-asked") || !g_file_is_native (item->file)
      || item->size > THUMB_MAX_SIZE)
    return;
  g_object_set_data (G_OBJECT (item), "fm-thumb-asked", GINT_TO_POINTER (1));
  g_queue_push_tail (self->thumb_queue, g_object_ref (item));
  thumb_next (self);
}

static void
thumbs_cancel (FmList *self)
{
  FmItem *item;

  while ((item = g_queue_pop_head (self->thumb_queue)))
    {
      g_object_set_data (G_OBJECT (item), "fm-thumb-asked", NULL);
      g_object_unref (item);
    }
  g_cancellable_cancel (self->thumb_cancel);
  g_clear_object (&self->thumb_cancel);
  self->thumb_cancel = g_cancellable_new ();
  /* the one decoding finishes on its own and starts nothing new for this list's old rows */
}

/* ---- the widget -------------------------------------------------------------------------------------------------- */

static gboolean
filter_func (gpointer object, gpointer data)
{
  FmList *self = data;

  return fm_name_matches (FM_ITEM (object)->name, self->needle);
}

static void
fm_list_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                 int *minimum_baseline, int *natural_baseline)
{
  gtk_widget_measure (FM_LIST (widget)->overlay, orientation, for_size, minimum, natural, NULL, NULL);
  /* the window does not grow with the rows: the list scrolls */
  *minimum = 0;
}

static void
fm_list_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  gtk_widget_allocate (FM_LIST (widget)->overlay, width, height, baseline, NULL);
}

static void
fm_list_dispose (GObject *object)
{
  FmList *self = FM_LIST (object);

  if (self->thumb_queue)
    {
      thumbs_cancel (self);
      g_queue_free (self->thumb_queue);
      self->thumb_queue = NULL;
    }
  g_clear_object (&self->thumb_cancel);
  g_clear_pointer (&self->overlay, gtk_widget_unparent);
  g_clear_object (&self->list_view);
  g_clear_object (&self->grid_view);
  g_clear_object (&self->selection);
  g_clear_object (&self->store);
  g_clear_object (&self->filtered);
  g_clear_pointer (&self->needle, g_free);
  G_OBJECT_CLASS (fm_list_parent_class)->dispose (object);
}

static void
fm_list_class_init (FmListClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = fm_list_dispose;
  GTK_WIDGET_CLASS (klass)->measure = fm_list_measure;
  GTK_WIDGET_CLASS (klass)->size_allocate = fm_list_size_allocate;
  gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "fm-list");

  signals[ACTIVATE] = g_signal_new ("activate", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                    G_TYPE_NONE, 1, FM_TYPE_ITEM);
  signals[LONG_PRESS] = g_signal_new ("long-press", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                      NULL, G_TYPE_NONE, 3, FM_TYPE_ITEM, G_TYPE_DOUBLE, G_TYPE_DOUBLE);
  signals[SELECTION_CHANGED] = g_signal_new ("selection-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                             NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static GtkListItemFactory *
factory_new (GCallback setup, FmList *self)
{
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();

  g_signal_connect (factory, "setup", setup, self);
  g_signal_connect (factory, "bind", G_CALLBACK (on_bind), self);
  g_signal_connect (factory, "unbind", G_CALLBACK (on_unbind), self);
  return factory;
}

static void
fm_list_init (FmList *self)
{
  self->thumb_queue = g_queue_new ();
  self->thumb_cancel = g_cancellable_new ();
  self->thumbnails = TRUE;
  self->store = g_list_store_new (FM_TYPE_ITEM);
  self->filter = gtk_custom_filter_new (filter_func, self, NULL);
  self->filtered = gtk_filter_list_model_new (G_LIST_MODEL (g_object_ref (self->store)), GTK_FILTER (self->filter));
  self->selection = GTK_SELECTION_MODEL (gtk_no_selection_new (G_LIST_MODEL (g_object_ref (self->filtered))));
  /* both views are kept (the scroll view drops the one it stops showing); the one not shown has no model, so no
   * rows bound */
  self->list_view = g_object_ref_sink (gtk_list_view_new (g_object_ref (self->selection),
                                                          factory_new (G_CALLBACK (on_setup), self)));
  gtk_widget_add_css_class (self->list_view, "fm-list-view");
  self->grid_view = g_object_ref_sink (gtk_grid_view_new (NULL, factory_new (G_CALLBACK (on_setup_tile), self)));
  gtk_grid_view_set_max_columns (GTK_GRID_VIEW (self->grid_view), 16);
  /* the columns counted by the tiles' minimum width (the icon's), not their natural one (a long name's) */
  gtk_scrollable_set_hscroll_policy (GTK_SCROLLABLE (self->grid_view), GTK_SCROLL_MINIMUM);
  gtk_widget_add_css_class (self->grid_view, "fm-grid-view");
  self->view = self->list_view;
  self->scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (self->scroll), self->view);
  self->message = fm_label (NULL, "fm-list-message", 18, TRUE);
  gtk_label_set_xalign (GTK_LABEL (self->message), 0.5f);
  gtk_label_set_justify (GTK_LABEL (self->message), GTK_JUSTIFY_CENTER);
  gtk_widget_set_halign (self->message, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (self->message, GTK_ALIGN_CENTER);
  gtk_widget_set_visible (self->message, FALSE);
  self->overlay = gtk_overlay_new ();
  gtk_overlay_set_child (GTK_OVERLAY (self->overlay), self->scroll);
  gtk_overlay_add_overlay (GTK_OVERLAY (self->overlay), self->message);
  gtk_widget_set_parent (self->overlay, GTK_WIDGET (self));
}

GtkWidget *
fm_list_new (void)
{
  return g_object_new (FM_TYPE_LIST, NULL);
}

void
fm_list_set_grid (FmList *self, gboolean grid)
{
  if (self->grid == !!grid)
    return;
  self->grid = !!grid;
  if (grid)
    {
      gtk_list_view_set_model (GTK_LIST_VIEW (self->list_view), NULL);
      gtk_grid_view_set_model (GTK_GRID_VIEW (self->grid_view), self->selection);
      self->view = self->grid_view;
    }
  else
    {
      gtk_grid_view_set_model (GTK_GRID_VIEW (self->grid_view), NULL);
      gtk_list_view_set_model (GTK_LIST_VIEW (self->list_view), self->selection);
      self->view = self->list_view;
    }
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (self->scroll), self->view);
}

gboolean
fm_list_get_grid (FmList *self)
{
  return self->grid;
}

void
fm_list_set_items (FmList *self, GPtrArray *items, gboolean keep_offset)
{
  GtkAdjustment *adjustment = spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (self->scroll));
  double offset = keep_offset ? gtk_adjustment_get_value (adjustment) : 0;

  thumbs_cancel (self);
  g_list_store_splice (self->store, 0, g_list_model_get_n_items (G_LIST_MODEL (self->store)),
                       items ? items->pdata : NULL, items ? items->len : 0);
  spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (self->scroll), offset);
}

void
fm_list_append_items (FmList *self, GPtrArray *items)
{
  g_list_store_splice (self->store, g_list_model_get_n_items (G_LIST_MODEL (self->store)), 0, items->pdata,
                       items->len);
}

GListModel *
fm_list_get_items (FmList *self)
{
  return G_LIST_MODEL (self->store);
}

void
fm_list_set_filter (FmList *self, const char *text)
{
  g_autofree char *folded = text && *text ? g_utf8_casefold (text, -1) : NULL;

  if (!g_strcmp0 (folded, self->needle))
    return;
  g_free (self->needle);
  self->needle = g_steal_pointer (&folded);
  gtk_filter_changed (GTK_FILTER (self->filter), GTK_FILTER_CHANGE_DIFFERENT);
}

void
fm_list_set_message (FmList *self, const char *message)
{
  gtk_label_set_text (GTK_LABEL (self->message), message ? message : "");
  gtk_widget_set_visible (self->message, message != NULL);
}

static void
changed_all (FmList *self)
{
  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->store));

  for (guint i = 0; i < n; i++)
    {
      g_autoptr(FmItem) item = g_list_model_get_item (G_LIST_MODEL (self->store), i);

      fm_item_changed (item);
    }
}

void
fm_list_set_show_thumbnails (FmList *self, gboolean show)
{
  if (self->thumbnails == !!show)
    return;
  self->thumbnails = !!show;
  changed_all (self);
}

void
fm_list_set_selecting (FmList *self, gboolean selecting)
{
  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->store));

  if (self->selecting == !!selecting)
    return;
  self->selecting = !!selecting;
  if (!selecting)
    for (guint i = 0; i < n; i++)
      {
        g_autoptr(FmItem) item = g_list_model_get_item (G_LIST_MODEL (self->store), i);

        item->selected = FALSE;
      }
  changed_all (self);
  g_signal_emit (self, signals[SELECTION_CHANGED], 0);
}

gboolean
fm_list_get_selecting (FmList *self)
{
  return self->selecting;
}

void
fm_list_set_selected (FmList *self, FmItem *item, gboolean selected)
{
  if (item->selected == !!selected)
    return;
  item->selected = !!selected;
  fm_item_changed (item);
  g_signal_emit (self, signals[SELECTION_CHANGED], 0);
}

GPtrArray *
fm_list_get_selected (FmList *self)
{
  GPtrArray *selected = g_ptr_array_new_with_free_func (g_object_unref);
  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->filtered));

  for (guint i = 0; i < n; i++)
    {
      FmItem *item = g_list_model_get_item (G_LIST_MODEL (self->filtered), i);

      if (item->selected)
        g_ptr_array_add (selected, item);
      else
        g_object_unref (item);
    }
  return selected;
}

guint
fm_list_count_selected (FmList *self)
{
  g_autoptr(GPtrArray) selected = fm_list_get_selected (self);

  return selected->len;
}

void
fm_list_select_all (FmList *self)
{
  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->filtered));

  for (guint i = 0; i < n; i++)
    {
      g_autoptr(FmItem) item = g_list_model_get_item (G_LIST_MODEL (self->filtered), i);

      if (!item->selected)
        {
          item->selected = TRUE;
          fm_item_changed (item);
        }
    }
  g_signal_emit (self, signals[SELECTION_CHANGED], 0);
}

void
fm_list_select_range (FmList *self)
{
  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->filtered));
  int first = -1, last = -1;

  for (guint i = 0; i < n; i++)
    {
      g_autoptr(FmItem) item = g_list_model_get_item (G_LIST_MODEL (self->filtered), i);

      if (item->selected)
        {
          if (first < 0)
            first = (int) i;
          last = (int) i;
        }
    }
  for (int i = first; first >= 0 && i <= last; i++)
    {
      g_autoptr(FmItem) item = g_list_model_get_item (G_LIST_MODEL (self->filtered), (guint) i);

      if (!item->selected)
        {
          item->selected = TRUE;
          fm_item_changed (item);
        }
    }
  g_signal_emit (self, signals[SELECTION_CHANGED], 0);
}

void
fm_list_scroll_to (FmList *self, FmItem *item)
{
  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->filtered));

  for (guint i = 0; i < n; i++)
    {
      g_autoptr(FmItem) shown = g_list_model_get_item (G_LIST_MODEL (self->filtered), i);

      if (shown == item)
        {
          if (self->grid)
            gtk_grid_view_scroll_to (GTK_GRID_VIEW (self->view), i, GTK_LIST_SCROLL_NONE, NULL);
          else
            gtk_list_view_scroll_to (GTK_LIST_VIEW (self->view), i, GTK_LIST_SCROLL_NONE, NULL);
          return;
        }
    }
}
