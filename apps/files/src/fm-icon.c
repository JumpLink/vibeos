/* FmIcon (fm-icon.h). */
#include "fm-icon.h"

#include <neo.h>

struct _FmIcon {
  GtkWidget parent;
  FmGlyph glyph;
  GIcon *gicon;                 /* a content type's icon, in the glyph's place */
  int size;
  GdkTexture *texture;
  gboolean dimmed;
};

G_DEFINE_FINAL_TYPE (FmIcon, fm_icon, GTK_TYPE_WIDGET)

/* ---- the glyphs' theme names ---------------------------------------------------------------------------------- *
 * The Icon Naming Specification's names, the most particular first; GThemedIcon adds each one's -symbolic as the
 * last resort, so a theme with only the symbolic icon still has something to show. */

static const char *const *
glyph_names (FmGlyph glyph)
{
#define NAMES(...) { static const char *const names[] = { __VA_ARGS__, NULL }; return names; }
  switch (glyph)
    {
    case FM_GLYPH_HOME: NAMES ("user-home")
    case FM_GLYPH_STORAGE: NAMES ("drive-harddisk")
    case FM_GLYPH_SD: NAMES ("media-flash-sd", "media-flash", "drive-removable-media")
    case FM_GLYPH_USB: NAMES ("drive-removable-media-usb", "drive-removable-media")
    case FM_GLYPH_SYSTEM: NAMES ("computer", "drive-harddisk-system", "drive-harddisk")
    case FM_GLYPH_TRASH: NAMES ("user-trash")
    case FM_GLYPH_RECENT: NAMES ("document-open-recent")
    case FM_GLYPH_NETWORK: NAMES ("network-workgroup")
    case FM_GLYPH_REMOTE:
    case FM_GLYPH_REMOTE_SMB:
    case FM_GLYPH_REMOTE_FTP:
    case FM_GLYPH_REMOTE_FTPS:
    case FM_GLYPH_REMOTE_SFTP:
    case FM_GLYPH_REMOTE_DAV: NAMES ("folder-remote", "folder")
    case FM_GLYPH_SERVER: NAMES ("network-server", "computer")
    case FM_GLYPH_DOWNLOADS: NAMES ("folder-download", "folder")
    case FM_GLYPH_PICTURES: NAMES ("folder-pictures", "folder")
    case FM_GLYPH_MUSIC: NAMES ("folder-music", "folder")
    case FM_GLYPH_VIDEOS: NAMES ("folder-videos", "folder")
    case FM_GLYPH_DOCUMENTS: NAMES ("folder-documents", "folder")
    case FM_GLYPH_FILE: NAMES ("application-x-generic", "text-x-generic")
    case FM_GLYPH_IMAGE: NAMES ("image-x-generic")
    case FM_GLYPH_AUDIO: NAMES ("audio-x-generic")
    case FM_GLYPH_VIDEO: NAMES ("video-x-generic")
    case FM_GLYPH_TEXT: NAMES ("text-x-generic")
    case FM_GLYPH_PDF: NAMES ("application-pdf", "x-office-document", "text-x-generic")
    case FM_GLYPH_ARCHIVE: NAMES ("package-x-generic", "application-x-generic")
    case FM_GLYPH_CODE: NAMES ("text-x-script", "text-x-generic")
    case FM_GLYPH_WEB: NAMES ("text-html", "text-x-generic")
    case FM_GLYPH_PACKAGE: NAMES ("application-vnd.android.package-archive", "package-x-generic",
                                  "application-x-executable")
    case FM_GLYPH_FOLDER:
    default: NAMES ("folder")
    }
#undef NAMES
}

GIcon *
fm_glyph_get_icon (FmGlyph glyph)
{
  static GIcon *icons[FM_N_GLYPHS];

  if ((guint) glyph >= FM_N_GLYPHS)
    glyph = FM_GLYPH_FOLDER;
  if (!icons[glyph])
    icons[glyph] = g_themed_icon_new_from_names ((char **) glyph_names (glyph), -1);
  return icons[glyph];
}

/* ---- the Recycle Bin: user-trash-full while it holds something ----------------------------------------------- */

static GFileMonitor *trash_monitor;
static GCancellable *trash_cancellable;
static GPtrArray *trash_icons;  /* the FmIcons showing the Recycle Bin, not held */
static gboolean trash_full;

static void
on_trash_counted (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GFileInfo) info = g_file_query_info_finish (G_FILE (source), result, NULL);
  gboolean full = info && g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_TRASH_ITEM_COUNT) > 0;

  if (!info || full == trash_full)
    return;
  trash_full = full;
  for (guint i = 0; trash_icons && i < trash_icons->len; i++)
    gtk_widget_queue_draw (trash_icons->pdata[i]);
}

static void
trash_count (void)
{
  g_autoptr(GFile) trash = g_file_new_for_uri ("trash:///");

  g_file_query_info_async (trash, G_FILE_ATTRIBUTE_TRASH_ITEM_COUNT, G_FILE_QUERY_INFO_NONE, G_PRIORITY_LOW,
                           trash_cancellable, on_trash_counted, NULL);
}

static void
on_trash_changed (GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event, gpointer data)
{
  trash_count ();
}

static void
trash_watch (FmIcon *self, gboolean watch)
{
  if (watch)
    {
      if (!trash_icons)
        trash_icons = g_ptr_array_new ();
      if (g_ptr_array_find (trash_icons, self, NULL))
        return;
      g_ptr_array_add (trash_icons, self);
      if (!trash_monitor)
        {
          g_autoptr(GFile) trash = g_file_new_for_uri ("trash:///");

          trash_cancellable = g_cancellable_new ();
          trash_monitor = g_file_monitor_directory (trash, G_FILE_MONITOR_NONE, NULL, NULL);
          if (trash_monitor)
            g_signal_connect (trash_monitor, "changed", G_CALLBACK (on_trash_changed), NULL);
          trash_count ();
        }
    }
  else if (trash_icons)
    g_ptr_array_remove_fast (trash_icons, self);
}

/* ---- the widget -------------------------------------------------------------------------------------------------- */

static GIcon *
shown_icon (FmIcon *self)
{
  static GIcon *full;

  if (self->gicon)
    return self->gicon;
  if (self->glyph == FM_GLYPH_TRASH && trash_full)
    {
      if (!full)
        {
          static const char *const names[] = { "user-trash-full", "user-trash", NULL };

          full = g_themed_icon_new_from_names ((char **) names, -1);
        }
      return full;
    }
  return fm_glyph_get_icon (self->glyph);
}

static void
fm_icon_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                 int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = FM_ICON (widget)->size;
}

static void
snapshot_texture (FmIcon *self, GtkSnapshot *snapshot, double x, double y, double size)
{
  /* a thumbnail fills the square, cropped to it, with slightly rounded corners */
  double tw = gdk_texture_get_width (self->texture), th = gdk_texture_get_height (self->texture);
  double scale = MAX (size / tw, size / th);
  GskRoundedRect clip;

  gsk_rounded_rect_init_from_rect (&clip, &GRAPHENE_RECT_INIT ((float) x, (float) y, (float) size, (float) size), 2);
  gtk_snapshot_push_rounded_clip (snapshot, &clip);
  gtk_snapshot_append_scaled_texture (snapshot, self->texture, GSK_SCALING_FILTER_TRILINEAR,
                                      &GRAPHENE_RECT_INIT ((float) (x + (size - tw * scale) / 2),
                                                           (float) (y + (size - th * scale) / 2),
                                                           (float) (tw * scale), (float) (th * scale)));
  gtk_snapshot_pop (snapshot);
}

static void
snapshot_theme_icon (FmIcon *self, GtkSnapshot *snapshot, double x, double y, double size)
{
  GtkWidget *widget = GTK_WIDGET (self);
  GtkIconTheme *theme = gtk_icon_theme_get_for_display (gtk_widget_get_display (widget));
  g_autoptr(GtkIconPaintable) paintable = gtk_icon_theme_lookup_by_gicon (theme, shown_icon (self), (int) size,
                                                                          neo_device_scale (widget),
                                                                          gtk_widget_get_direction (widget), 0);
  GdkRGBA color;

  if (!paintable)
    return;
  /* a coloured icon draws as it is; a symbolic one in the widget's colour */
  gtk_widget_get_color (widget, &color);
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT ((float) x, (float) y));
  gtk_symbolic_paintable_snapshot_symbolic (GTK_SYMBOLIC_PAINTABLE (paintable), snapshot, size, size, &color, 1);
  gtk_snapshot_restore (snapshot);
}

static void
fm_icon_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  FmIcon *self = FM_ICON (widget);
  double size = self->size;
  double x = (gtk_widget_get_width (widget) - size) / 2, y = (gtk_widget_get_height (widget) - size) / 2;

  if (self->dimmed)
    gtk_snapshot_push_opacity (snapshot, 0.5);
  if (self->texture)
    snapshot_texture (self, snapshot, x, y, size);
  else
    snapshot_theme_icon (self, snapshot, x, y, size);
  if (self->dimmed)
    gtk_snapshot_pop (snapshot);
}

/* another icon theme set (or the colour changed): the style of every widget changes with it */
static void
fm_icon_css_changed (GtkWidget *widget, GtkCssStyleChange *change)
{
  GTK_WIDGET_CLASS (fm_icon_parent_class)->css_changed (widget, change);
  gtk_widget_queue_draw (widget);
}

static void
fm_icon_dispose (GObject *object)
{
  FmIcon *self = FM_ICON (object);

  trash_watch (self, FALSE);
  g_clear_object (&self->texture);
  g_clear_object (&self->gicon);
  G_OBJECT_CLASS (fm_icon_parent_class)->dispose (object);
}

static void
fm_icon_class_init (FmIconClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = fm_icon_dispose;
  GTK_WIDGET_CLASS (klass)->measure = fm_icon_measure;
  GTK_WIDGET_CLASS (klass)->snapshot = fm_icon_snapshot;
  GTK_WIDGET_CLASS (klass)->css_changed = fm_icon_css_changed;
  gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "fm-icon");
}

static void
fm_icon_init (FmIcon *self)
{
  gtk_widget_set_can_target (GTK_WIDGET (self), FALSE);
}

GtkWidget *
fm_icon_new (FmGlyph glyph, int size)
{
  FmIcon *self = g_object_new (FM_TYPE_ICON, NULL);

  self->glyph = FM_GLYPH_FOLDER;
  self->size = size;
  fm_icon_set_glyph (self, glyph);
  return GTK_WIDGET (self);
}

void
fm_icon_set_glyph (FmIcon *self, FmGlyph glyph)
{
  if (self->glyph == glyph && !self->gicon)
    return;
  self->glyph = glyph;
  g_clear_object (&self->gicon);
  trash_watch (self, glyph == FM_GLYPH_TRASH);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

FmGlyph
fm_icon_get_glyph (FmIcon *self)
{
  return self->glyph;
}

void
fm_icon_set_gicon (FmIcon *self, GIcon *icon)
{
  if (self->gicon == icon || (self->gicon && icon && g_icon_equal (self->gicon, icon)))
    return;
  g_set_object (&self->gicon, icon);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
fm_icon_set_texture (FmIcon *self, GdkTexture *texture)
{
  if (self->texture == texture)
    return;
  g_set_object (&self->texture, texture);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
fm_icon_set_dimmed (FmIcon *self, gboolean dimmed)
{
  if (self->dimmed == !!dimmed)
    return;
  self->dimmed = !!dimmed;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}
