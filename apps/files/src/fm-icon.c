/* FmIcon (fm-icon.h). Every drawing is laid out on a 48-unit grid and scaled to the size asked for. */
#include "fm-icon.h"

#include <neo.h>

struct _FmIcon {
  GtkWidget parent;
  FmGlyph glyph;
  int size;
  GdkTexture *texture;
  gboolean dimmed;
};

G_DEFINE_FINAL_TYPE (FmIcon, fm_icon, GTK_TYPE_WIDGET)

/* the colours of the spec's table, and the file types' */
#define C_FOLDER        0xffe2ac63
#define C_FOLDER_BACK   0xffc48a3f
#define C_FOLDER_MARK   0xff7a4d14
#define C_REMOTE        0xff5d9fe0
#define C_REMOTE_BACK   0xff3f7fbf
#define C_ROOF          0xffff4646
#define C_WALL          0xfff2f2f2
#define C_DRIVE         0xffcdcdcf
#define C_DRIVE_DARK    0xff9d9da2
#define C_LED           0xff78b73d
#define C_LED_SYSTEM    0xffffa726
#define C_BIN           0xff2a2a2a
#define C_BIN_EDGE      0xff6d6d6d
#define C_BIN_ARROWS    0xff0091e6
#define C_RECENT        0xff5c6bc0
#define C_SCREEN        0xff2f6db5
#define C_WHITE         0xffffffff

typedef struct {
  GtkSnapshot *snapshot;
  GtkWidget *widget;
  float x, y, s;                /* where the 48-unit grid's origin is, and its unit in dp */
} Ctx;

static GdkRGBA
rgba (guint32 argb)
{
  return (GdkRGBA) { ((argb >> 16) & 0xff) / 255.0f, ((argb >> 8) & 0xff) / 255.0f, (argb & 0xff) / 255.0f,
                     ((argb >> 24) & 0xff) / 255.0f };
}

#define X(v) (c->x + (v) * c->s)
#define Y(v) (c->y + (v) * c->s)

static void
fill (Ctx *c, GskPathBuilder *builder, guint32 color)
{
  g_autoptr(GskPath) path = gsk_path_builder_free_to_path (builder);
  GdkRGBA rgba_color = rgba (color);

  gtk_snapshot_append_fill (c->snapshot, path, GSK_FILL_RULE_WINDING, &rgba_color);
}

static void
stroke (Ctx *c, GskPathBuilder *builder, float width, guint32 color)
{
  g_autoptr(GskPath) path = gsk_path_builder_free_to_path (builder);
  g_autoptr(GskStroke) line = gsk_stroke_new (width * c->s);
  GdkRGBA rgba_color = rgba (color);

  gsk_stroke_set_line_cap (line, GSK_LINE_CAP_ROUND);
  gsk_stroke_set_line_join (line, GSK_LINE_JOIN_ROUND);
  gtk_snapshot_append_stroke (c->snapshot, path, line, &rgba_color);
}

static void
rrect (Ctx *c, float x, float y, float w, float h, float r, guint32 color)
{
  GskPathBuilder *builder = gsk_path_builder_new ();
  float rr = r * c->s;

  gsk_path_builder_add_rounded_rect (builder, &(GskRoundedRect) {
                                       .bounds = GRAPHENE_RECT_INIT (X (x), Y (y), w * c->s, h * c->s),
                                       .corner = { { rr, rr }, { rr, rr }, { rr, rr }, { rr, rr } } });
  fill (c, builder, color);
}

static void
circle (Ctx *c, float cx, float cy, float r, guint32 color)
{
  GskPathBuilder *builder = gsk_path_builder_new ();

  gsk_path_builder_add_circle (builder, &GRAPHENE_POINT_INIT (X (cx), Y (cy)), r * c->s);
  fill (c, builder, color);
}

static void
glyph (Ctx *c, const char *name, float x, float y, float size, guint32 color)
{
  GdkRGBA rgba_color = rgba (color);

  neo_snapshot_icon (c->snapshot, c->widget, name, X (x), Y (y), size * c->s, &rgba_color);
}

/* bold text centred on (cx, cy): the remote folders' FTP / SFTP / DAV */
static void
badge_text (Ctx *c, const char *text, float cx, float cy, float size, guint32 color)
{
  PangoLayout *layout = gtk_widget_create_pango_layout (c->widget, text);
  PangoFontDescription *font = pango_font_description_from_string ("Neo Roboto, Roboto, sans-serif");
  GdkRGBA rgba_color = rgba (color);
  PangoRectangle ink;

  pango_font_description_set_weight (font, 900);
  pango_font_description_set_absolute_size (font, size * c->s * PANGO_SCALE);
  pango_layout_set_font_description (layout, font);
  pango_layout_get_pixel_extents (layout, &ink, NULL);
  gtk_snapshot_save (c->snapshot);
  gtk_snapshot_translate (c->snapshot, &GRAPHENE_POINT_INIT (X (cx) - ink.x - ink.width / 2.0f,
                                                             Y (cy) - ink.y - ink.height / 2.0f));
  gtk_snapshot_append_layout (c->snapshot, layout, &rgba_color);
  gtk_snapshot_restore (c->snapshot);
  pango_font_description_free (font);
  g_object_unref (layout);
}

/* a folder: the back with its tab, the front over it; the whole width and 42 units of the height, the footprint
 * File Manager+'s folders have (44 x 38.7 dp in a 44 dp row icon) */
#define FOLDER_MID 28.25f       /* the front's middle, where a badge goes */
static void
folder (Ctx *c, guint32 front, guint32 back)
{
  GskPathBuilder *builder = gsk_path_builder_new ();

  gsk_path_builder_move_to (builder, X (0), Y (6));
  gsk_path_builder_quad_to (builder, X (0), Y (3), X (3), Y (3));
  gsk_path_builder_line_to (builder, X (15.5f), Y (3));
  gsk_path_builder_line_to (builder, X (20), Y (7.5f));
  gsk_path_builder_line_to (builder, X (45), Y (7.5f));
  gsk_path_builder_quad_to (builder, X (48), Y (7.5f), X (48), Y (10.5f));
  gsk_path_builder_line_to (builder, X (48), Y (40));
  gsk_path_builder_line_to (builder, X (0), Y (40));
  gsk_path_builder_close (builder);
  fill (c, builder, back);
  rrect (c, 0, 11.5f, 48, 33.5f, 3, front);
}

static void
draw_home (Ctx *c)
{
  GskPathBuilder *roof = gsk_path_builder_new ();

  /* the whole width and 45 units of the height, as the house File Manager+'s path bar starts with (31.3 x 30 dp
   * in its 32) */
  rrect (c, 6, 20, 36, 26.5f, 2, C_WALL);
  rrect (c, 19.5f, 30, 9, 16.5f, 1, C_ROOF);
  rrect (c, 35, 5, 5.5f, 11, 0.5f, C_ROOF);
  gsk_path_builder_move_to (roof, X (24), Y (1.5f));
  gsk_path_builder_line_to (roof, X (48), Y (22.5f));
  gsk_path_builder_line_to (roof, X (44.5f), Y (26));
  gsk_path_builder_line_to (roof, X (24), Y (7.5f));
  gsk_path_builder_line_to (roof, X (3.5f), Y (26));
  gsk_path_builder_line_to (roof, X (0), Y (22.5f));
  gsk_path_builder_close (roof);
  fill (c, roof, C_ROOF);
}

static void
draw_drive (Ctx *c, guint32 led)
{
  GskPathBuilder *top = gsk_path_builder_new ();

  /* a drive seen from above its front, filling the square: the top face narrowing away, the front with its light */
  gsk_path_builder_move_to (top, X (6), Y (0));
  gsk_path_builder_line_to (top, X (42), Y (0));
  gsk_path_builder_quad_to (top, X (44), Y (0), X (44.3f), Y (2));
  gsk_path_builder_line_to (top, X (48), Y (36));
  gsk_path_builder_line_to (top, X (0), Y (36));
  gsk_path_builder_line_to (top, X (3.7f), Y (2));
  gsk_path_builder_quad_to (top, X (4), Y (0), X (6), Y (0));
  gsk_path_builder_close (top);
  fill (c, top, C_DRIVE);
  rrect (c, 0, 35, 48, 13, 2, C_DRIVE_DARK);
  circle (c, 6.5f, 41.5f, 1.8f, led);
}

static void
draw_sd (Ctx *c)
{
  GskPathBuilder *card = gsk_path_builder_new ();

  gsk_path_builder_move_to (card, X (17), Y (5));
  gsk_path_builder_line_to (card, X (35), Y (5));
  gsk_path_builder_quad_to (card, X (38), Y (5), X (38), Y (8));
  gsk_path_builder_line_to (card, X (38), Y (40));
  gsk_path_builder_quad_to (card, X (38), Y (43), X (35), Y (43));
  gsk_path_builder_line_to (card, X (13), Y (43));
  gsk_path_builder_quad_to (card, X (10), Y (43), X (10), Y (40));
  gsk_path_builder_line_to (card, X (10), Y (12));
  gsk_path_builder_close (card);
  fill (c, card, C_DRIVE);
  for (int i = 0; i < 4; i++)
    rrect (c, 17 + i * 5, 8.5f, 3, 8, 0.8f, C_DRIVE_DARK);
  rrect (c, 15, 25, 18, 12, 1.5f, C_LED);
}

static void
draw_usb (Ctx *c)
{
  rrect (c, 18, 3, 12, 12, 1, C_DRIVE_DARK);
  rrect (c, 20.5f, 6.5f, 2.5f, 3, 0.4f, 0xff5a5a5e);
  rrect (c, 25, 6.5f, 2.5f, 3, 0.4f, 0xff5a5a5e);
  rrect (c, 15, 14, 18, 31, 3, C_DRIVE);
  circle (c, 24, 37, 2, C_LED);
}

static void
draw_trash (Ctx *c)
{
  GskPathBuilder *body = gsk_path_builder_new ();

  /* 42 x 39 of the 48 (28 x 26 dp in the drawer's 32, fmplus-5) */
  rrect (c, 18, 9, 12, 5, 1.5f, C_BIN_EDGE);
  rrect (c, 3, 13, 42, 5, 1.5f, C_BIN_EDGE);
  gsk_path_builder_move_to (body, X (6), Y (19));
  gsk_path_builder_line_to (body, X (42), Y (19));
  gsk_path_builder_line_to (body, X (38.5f), Y (45));
  gsk_path_builder_quad_to (body, X (38.2f), Y (47.5f), X (35.5f), Y (47.5f));
  gsk_path_builder_line_to (body, X (12.5f), Y (47.5f));
  gsk_path_builder_quad_to (body, X (9.8f), Y (47.5f), X (9.5f), Y (45));
  gsk_path_builder_close (body);
  {
    g_autoptr(GskPath) path = gsk_path_builder_free_to_path (body);
    g_autoptr(GskStroke) line = gsk_stroke_new (1.5f * c->s);
    GdkRGBA inside = rgba (C_BIN), outline = rgba (C_BIN_EDGE);

    gtk_snapshot_append_fill (c->snapshot, path, GSK_FILL_RULE_WINDING, &inside);
    gtk_snapshot_append_stroke (c->snapshot, path, line, &outline);
  }
  glyph (c, "fm-recycling-symbolic", 12, 21, 24, C_BIN_ARROWS);
}

static void
draw_recent (Ctx *c)
{
  GskPathBuilder *hands = gsk_path_builder_new ();

  circle (c, 24, 24, 19, C_RECENT);
  gsk_path_builder_move_to (hands, X (24), Y (13));
  gsk_path_builder_line_to (hands, X (24), Y (24));
  gsk_path_builder_line_to (hands, X (31.5f), Y (29));
  stroke (c, hands, 3.5f, C_WHITE);
}

static void
draw_server (Ctx *c)
{
  rrect (c, 5, 8, 38, 26, 2.5f, C_DRIVE);
  rrect (c, 8, 11, 32, 20, 1, C_SCREEN);
  rrect (c, 20, 34, 8, 5, 0, C_DRIVE_DARK);
  rrect (c, 14, 38.5f, 20, 3.5f, 1.5f, C_DRIVE);
}

/* a remote folder with its badge: a monitor (SMB, the Local Network) or the protocol's name */
static void
draw_remote (Ctx *c, FmGlyph which)
{
  folder (c, C_REMOTE, C_REMOTE_BACK);
  switch (which)
    {
    case FM_GLYPH_REMOTE_FTP:
      badge_text (c, "FTP", 24, FOLDER_MID, 13, C_WHITE);
      break;
    case FM_GLYPH_REMOTE_FTPS:
      badge_text (c, "FTPS", 24, FOLDER_MID, 12, C_WHITE);
      break;
    case FM_GLYPH_REMOTE_SFTP:
      badge_text (c, "SFTP", 24, FOLDER_MID, 12, C_WHITE);
      break;
    case FM_GLYPH_REMOTE_DAV:
      badge_text (c, "DAV", 24, FOLDER_MID, 13, C_WHITE);
      break;
    case FM_GLYPH_REMOTE:
      glyph (c, "fm-web-symbolic", 14, FOLDER_MID - 10, 20, C_WHITE);
      break;
    case FM_GLYPH_NETWORK:
      glyph (c, "fm-lan-symbolic", 14, FOLDER_MID - 10, 20, C_WHITE);
      break;
    case FM_GLYPH_REMOTE_SMB:
    default:
      glyph (c, "fm-desktop-windows-symbolic", 14, FOLDER_MID - 10, 20, C_WHITE);
      break;
    }
}

/* a file: a page with its corner folded, in the type's colour, the type's glyph on it */
static void
draw_file (Ctx *c, guint32 color, const char *name)
{
  GskPathBuilder *page = gsk_path_builder_new ();
  GskPathBuilder *fold = gsk_path_builder_new ();

  gsk_path_builder_move_to (page, X (12), Y (4));
  gsk_path_builder_line_to (page, X (29), Y (4));
  gsk_path_builder_line_to (page, X (40), Y (15));
  gsk_path_builder_line_to (page, X (40), Y (41));
  gsk_path_builder_quad_to (page, X (40), Y (44), X (37), Y (44));
  gsk_path_builder_line_to (page, X (11), Y (44));
  gsk_path_builder_quad_to (page, X (8), Y (44), X (8), Y (41));
  gsk_path_builder_line_to (page, X (8), Y (7));
  gsk_path_builder_quad_to (page, X (8), Y (4), X (11), Y (4));
  gsk_path_builder_close (page);
  fill (c, page, color);
  gsk_path_builder_move_to (fold, X (29), Y (4));
  gsk_path_builder_line_to (fold, X (29), Y (12.5f));
  gsk_path_builder_quad_to (fold, X (29), Y (15), X (31.5f), Y (15));
  gsk_path_builder_line_to (fold, X (40), Y (15));
  gsk_path_builder_close (fold);
  fill (c, fold, 0x66ffffff);
  glyph (c, name, 13, 20, 22, C_WHITE);
}

void
fm_icon_snapshot (GtkSnapshot *snapshot, GtkWidget *widget, FmGlyph which, double x, double y, double size)
{
  Ctx ctx = { snapshot, widget, (float) x, (float) y, (float) (size / 48.0) }, *c = &ctx;

  switch (which)
    {
    case FM_GLYPH_HOME:
      draw_home (c);
      break;
    case FM_GLYPH_STORAGE:
      draw_drive (c, C_LED);
      break;
    case FM_GLYPH_SYSTEM:
      draw_drive (c, C_LED_SYSTEM);
      break;
    case FM_GLYPH_SD:
      draw_sd (c);
      break;
    case FM_GLYPH_USB:
      draw_usb (c);
      break;
    case FM_GLYPH_TRASH:
      draw_trash (c);
      break;
    case FM_GLYPH_RECENT:
      draw_recent (c);
      break;
    case FM_GLYPH_SERVER:
      draw_server (c);
      break;
    case FM_GLYPH_NETWORK:
    case FM_GLYPH_REMOTE:
    case FM_GLYPH_REMOTE_SMB:
    case FM_GLYPH_REMOTE_FTP:
    case FM_GLYPH_REMOTE_FTPS:
    case FM_GLYPH_REMOTE_SFTP:
    case FM_GLYPH_REMOTE_DAV:
      draw_remote (c, which);
      break;
    case FM_GLYPH_DOWNLOADS:
    case FM_GLYPH_PICTURES:
    case FM_GLYPH_MUSIC:
    case FM_GLYPH_VIDEOS:
    case FM_GLYPH_DOCUMENTS:
      {
        static const char *const marks[] = { "fm-download-symbolic", "fm-image-symbolic",
                                              "fm-music-note-symbolic", "fm-movie-symbolic",
                                              "fm-description-symbolic" };

        folder (c, C_FOLDER, C_FOLDER_BACK);
        glyph (c, marks[which - FM_GLYPH_DOWNLOADS], 15, FOLDER_MID - 9, 18, C_FOLDER_MARK);
        break;
      }
    case FM_GLYPH_FILE:
      draw_file (c, 0xff8a9ba5, "fm-draft-symbolic");
      break;
    case FM_GLYPH_IMAGE:
      draw_file (c, 0xff43a047, "fm-image-symbolic");
      break;
    case FM_GLYPH_AUDIO:
      draw_file (c, 0xfffb8c00, "fm-music-note-symbolic");
      break;
    case FM_GLYPH_VIDEO:
      draw_file (c, 0xffe53935, "fm-movie-symbolic");
      break;
    case FM_GLYPH_TEXT:
      draw_file (c, 0xff1e88e5, "fm-article-symbolic");
      break;
    case FM_GLYPH_PDF:
      draw_file (c, 0xffc62828, "fm-picture-as-pdf-symbolic");
      break;
    case FM_GLYPH_ARCHIVE:
      draw_file (c, 0xff8d6e63, "fm-folder-zip-symbolic");
      break;
    case FM_GLYPH_CODE:
      draw_file (c, 0xff00897b, "fm-code-symbolic");
      break;
    case FM_GLYPH_WEB:
      draw_file (c, 0xff039be5, "fm-web-symbolic");
      break;
    case FM_GLYPH_PACKAGE:
      draw_file (c, 0xff7cb342, "fm-android-symbolic");
      break;
    case FM_GLYPH_FOLDER:
    default:
      folder (c, C_FOLDER, C_FOLDER_BACK);
      break;
    }
}

#undef X
#undef Y

/* ---- the widget ------------------------------------------------------------------------------------------------ */

static void
fm_icon_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                 int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = FM_ICON (widget)->size;
}

static void
fm_icon_snapshot_widget (GtkWidget *widget, GtkSnapshot *snapshot)
{
  FmIcon *self = FM_ICON (widget);
  double size = self->size;
  double x = (gtk_widget_get_width (widget) - size) / 2, y = (gtk_widget_get_height (widget) - size) / 2;

  if (self->dimmed)
    gtk_snapshot_push_opacity (snapshot, 0.5);
  if (self->texture)
    {
      /* a thumbnail fills the square, cropped to it, with 2 dp corners */
      double tw = gdk_texture_get_width (self->texture), th = gdk_texture_get_height (self->texture);
      double scale = MAX (size / tw, size / th);
      GskRoundedRect clip;

      gsk_rounded_rect_init_from_rect (&clip, &GRAPHENE_RECT_INIT ((float) x, (float) y, (float) size, (float) size),
                                       2);
      gtk_snapshot_push_rounded_clip (snapshot, &clip);
      gtk_snapshot_append_scaled_texture (snapshot, self->texture, GSK_SCALING_FILTER_TRILINEAR,
                                          &GRAPHENE_RECT_INIT ((float) (x + (size - tw * scale) / 2),
                                                               (float) (y + (size - th * scale) / 2),
                                                               (float) (tw * scale), (float) (th * scale)));
      gtk_snapshot_pop (snapshot);
    }
  else
    fm_icon_snapshot (snapshot, widget, self->glyph, x, y, size);
  if (self->dimmed)
    gtk_snapshot_pop (snapshot);
}

static void
fm_icon_dispose (GObject *object)
{
  g_clear_object (&FM_ICON (object)->texture);
  G_OBJECT_CLASS (fm_icon_parent_class)->dispose (object);
}

static void
fm_icon_class_init (FmIconClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = fm_icon_dispose;
  GTK_WIDGET_CLASS (klass)->measure = fm_icon_measure;
  GTK_WIDGET_CLASS (klass)->snapshot = fm_icon_snapshot_widget;
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

  self->glyph = glyph;
  self->size = size;
  return GTK_WIDGET (self);
}

void
fm_icon_set_glyph (FmIcon *self, FmGlyph glyph)
{
  if (self->glyph == glyph)
    return;
  self->glyph = glyph;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

FmGlyph
fm_icon_get_glyph (FmIcon *self)
{
  return self->glyph;
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
