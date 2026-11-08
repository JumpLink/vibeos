/* Messaging's avatar (msg-avatar.h). */
#include "msg-avatar.h"

#include <math.h>

#define TILE_LETTER_RATIO 0.67          /* letter_to_tile_ratio */
#define TILE_ICON 32                    /* ic_person_light, 24 dp scaled 1.33x */
#define MAX_PEOPLE 4

/* letter_tile_colors (colors.xml:94-107) as the phone's dark palette resolves them */
static const char *tile_colors[] = {
  "#81b4bb", "#96b0b4", "#8dafd1", "#6799a0", "#7c9599", "#7394b5",
  "#4d7f86", "#627b7f", "#597a9a", "#33666d", "#4a6366", "#406180",
};

typedef struct {
  char *letter;                 /* NULL: the person glyph */
  GdkRGBA color;
  GdkTexture *photo;
} Person;

struct _MsgAvatar {
  GtkWidget parent;
  int size;
  Person people[MAX_PEOPLE];
  int count;
  gboolean checked;
};

G_DEFINE_FINAL_TYPE (MsgAvatar, msg_avatar, GTK_TYPE_WIDGET)

/* Java's String.hashCode over UTF-16 code units, and Math.abs of it (which leaves INT_MIN negative: the fallback) */
static int
tile_index (const char *identifier)
{
  g_autofree gunichar2 *units = NULL;
  glong n = 0;
  gint32 hash = 0;

  units = g_utf8_to_utf16 (identifier ? identifier : "", -1, NULL, &n, NULL);
  for (glong i = 0; i < n; i++)
    hash = (gint32) ((guint32) hash * 31u + units[i]);
  if (hash == G_MININT32)
    return -1;
  return (hash < 0 ? -hash : hash) % G_N_ELEMENTS (tile_colors);
}

GdkRGBA
msg_avatar_color (const char *identifier)
{
  int index = tile_index (identifier);
  GdkRGBA color;

  gdk_rgba_parse (&color, index >= 0 ? tile_colors[index] : "#33666d");
  return color;
}

static void
clear_people (MsgAvatar *self)
{
  for (int i = 0; i < self->count; i++)
    {
      g_clear_pointer (&self->people[i].letter, g_free);
      g_clear_object (&self->people[i].photo);
    }
  self->count = 0;
}

static void
set_one (Person *person, const char *name, const char *identifier, GBytes *photo)
{
  gunichar first = name && *name ? g_utf8_get_char_validated (name, -1) : 0;
  char letter[8] = { 0 };

  /* the first character, upper-cased, only if it is a letter (a number gets the glyph) */
  if (first != (gunichar) -1 && first != (gunichar) -2 && g_unichar_isalpha (first))
    {
      g_unichar_to_utf8 (g_unichar_toupper (first), letter);
      person->letter = g_strdup (letter);
    }
  person->color = msg_avatar_color (identifier ? identifier : name);
  if (photo)
    person->photo = gdk_texture_new_from_bytes (photo, NULL);
}

void
msg_avatar_set_person (MsgAvatar *self, const char *name, const char *identifier, GBytes *photo)
{
  clear_people (self);
  set_one (&self->people[0], name, identifier, photo);
  self->count = 1;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
msg_avatar_set_group (MsgAvatar *self, const char * const *names, const char * const *identifiers,
                      GBytes * const *photos)
{
  clear_people (self);
  for (int i = 0; names && names[i] && i < MAX_PEOPLE; i++, self->count++)
    set_one (&self->people[i], names[i], identifiers ? identifiers[i] : NULL, photos ? photos[i] : NULL);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
msg_avatar_set_checked (MsgAvatar *self, gboolean checked)
{
  if (self->checked == checked)
    return;
  self->checked = checked;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* --- drawing */

static void
push_circle (GtkSnapshot *snapshot, double x, double y, double d)
{
  GskRoundedRect circle;

  gsk_rounded_rect_init_from_rect (&circle, &GRAPHENE_RECT_INIT (x, y, d, d), d / 2);
  gtk_snapshot_push_rounded_clip (snapshot, &circle);
}

/* the glyph, centred on its ink rather than on its line (AvatarRequest draws at the text bounds' centre) */
static void
draw_letter (MsgAvatar *self, GtkSnapshot *snapshot, const char *letter, double x, double y, double d)
{
  PangoLayout *layout = gtk_widget_create_pango_layout (GTK_WIDGET (self), letter);
  PangoFontDescription *font = pango_font_description_from_string ("Neo Roboto");
  PangoRectangle ink;
  GdkRGBA white = { 1, 1, 1, 1 };

  /* Paint text: sans-serif-medium, 500, which Bold text does not adjust */
  pango_font_description_set_weight (font, 500);
  pango_font_description_set_absolute_size (font, d * TILE_LETTER_RATIO * PANGO_SCALE);
  pango_layout_set_font_description (layout, font);
  pango_layout_get_pixel_extents (layout, &ink, NULL);
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (x + (d - ink.width) / 2 - ink.x,
                                                          y + (d - ink.height) / 2 - ink.y));
  gtk_snapshot_append_layout (snapshot, layout, &white);
  gtk_snapshot_restore (snapshot);
  pango_font_description_free (font);
  g_object_unref (layout);
}

static void
draw_icon (MsgAvatar *self, GtkSnapshot *snapshot, const char *name, double x, double y, double size,
           const GdkRGBA *color)
{
  GtkIconTheme *theme = gtk_icon_theme_get_for_display (gtk_widget_get_display (GTK_WIDGET (self)));
  g_autoptr(GtkIconPaintable) icon = gtk_icon_theme_lookup_icon (theme, name, NULL, (int) ceil (size),
                                                                 gtk_widget_get_scale_factor (GTK_WIDGET (self)),
                                                                 GTK_TEXT_DIR_NONE, 0);
  GdkRGBA colors[4] = { *color, *color, *color, *color };

  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (x, y));
  gtk_symbolic_paintable_snapshot_symbolic (GTK_SYMBOLIC_PAINTABLE (icon), snapshot, size, size, colors, 4);
  gtk_snapshot_restore (snapshot);
}

static void
draw_person (MsgAvatar *self, GtkSnapshot *snapshot, const Person *person, double x, double y, double d)
{
  GdkRGBA white = { 1, 1, 1, 1 };

  push_circle (snapshot, x, y, d);
  if (person->photo)
    gtk_snapshot_append_texture (snapshot, person->photo, &GRAPHENE_RECT_INIT (x, y, d, d));
  else
    {
      gtk_snapshot_append_color (snapshot, &person->color, &GRAPHENE_RECT_INIT (x, y, d, d));
      if (person->letter)
        draw_letter (self, snapshot, person->letter, x, y, d);
      else
        {
          /* its size on a whole tile; a group's smaller circles scale it with them (the 42 dp tile is the
           * smallest whole one) */
          double icon = d >= 42 ? TILE_ICON : d * TILE_ICON / 42;

          draw_icon (self, snapshot, "messaging-person-symbolic", x + (d - icon) / 2, y + (d - icon) / 2, icon,
                     &white);
        }
    }
  gtk_snapshot_pop (snapshot);
}

static void
msg_avatar_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  MsgAvatar *self = MSG_AVATAR (widget);
  double w = gtk_widget_get_width (widget);

  if (self->checked)
    {
      GdkRGBA accent, white = { 1, 1, 1, 1 };

      /* ic_checkmark_circle_blue: a lineage_accent disc with the check cut out of it, white behind */
      gdk_rgba_parse (&accent, "#b8ebf3");
      push_circle (snapshot, w * 0.2, w * 0.2, w * 0.6);
      gtk_snapshot_append_color (snapshot, &white, &GRAPHENE_RECT_INIT (0, 0, w, w));
      gtk_snapshot_pop (snapshot);
      draw_icon (self, snapshot, "messaging-checkmark-circle-symbolic", 0, 0, w, &accent);
      return;
    }
  switch (self->count)
    {
    case 0:
      break;
    case 1:
      draw_person (self, snapshot, &self->people[0], 0, 0, w);
      break;
    case 2:
      {
        /* AvatarGroupRequestDescriptor: two circles of (2 - √2) W, at the top-left and bottom-right corners */
        double d = (2 - G_SQRT2) * w;

        draw_person (self, snapshot, &self->people[0], 0, 0, d);
        draw_person (self, snapshot, &self->people[1], w - d, w - d, d);
        break;
      }
    case 3:
      {
        /* radius W/4: two at the bottom, the third above them touching both */
        double d = w / 2, top = (w - w / 4) - (w / 4) * sqrt (3) - w / 4;

        draw_person (self, snapshot, &self->people[0], w / 4, top, d);
        draw_person (self, snapshot, &self->people[1], 0, w / 2, d);
        draw_person (self, snapshot, &self->people[2], w / 2, w / 2, d);
        break;
      }
    default:
      for (int i = 0; i < MAX_PEOPLE; i++)
        draw_person (self, snapshot, &self->people[i], (i % 2) * w / 2, (i / 2) * w / 2, w / 2);
    }
}

static void
msg_avatar_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                    int *min_baseline, int *nat_baseline)
{
  *min = *nat = MSG_AVATAR (widget)->size;
}

static void
msg_avatar_finalize (GObject *object)
{
  clear_people (MSG_AVATAR (object));
  G_OBJECT_CLASS (msg_avatar_parent_class)->finalize (object);
}

static void
msg_avatar_class_init (MsgAvatarClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->finalize = msg_avatar_finalize;
  widget_class->snapshot = msg_avatar_snapshot;
  widget_class->measure = msg_avatar_measure;
  gtk_widget_class_set_css_name (widget_class, "msg-avatar");
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_IMG);
}

static void
msg_avatar_init (MsgAvatar *self)
{
  gtk_widget_set_halign (GTK_WIDGET (self), GTK_ALIGN_START);
  gtk_widget_set_valign (GTK_WIDGET (self), GTK_ALIGN_START);
}

GtkWidget *
msg_avatar_new (int size)
{
  MsgAvatar *self = g_object_new (MSG_TYPE_AVATAR, NULL);

  self->size = size;
  return GTK_WIDGET (self);
}
