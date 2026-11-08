/* Text as the Dialer draws it: see text.h. */
#include "text.h"

/* includeFontPadding: the box above the ascent and below the descent, as fractions of the size. Roboto's head
 * yMax/yMin are 2163/-555 against its hhea 1900/-500 of 2048 units; Google Sans Flex's 2210/-605 against
 * 1932/-572 of 2000 (answer-view.c measured both against the stills) */
static double
pad_top (TextStyle style)
{
  return style.size * (style.family == TEXT_ROBOTO ? 263 / 2048.0 : 278 / 2000.0);
}

static double
pad_bottom (TextStyle style)
{
  return style.size * (style.family == TEXT_ROBOTO ? 55 / 2048.0 : 33 / 2000.0);
}

GdkRGBA
text_rgb (guint32 value)
{
  return (GdkRGBA) { ((value >> 16) & 0xff) / 255.0, ((value >> 8) & 0xff) / 255.0, (value & 0xff) / 255.0, 1 };
}

void
text_layout_set_style (PangoLayout *layout, TextStyle style)
{
  g_autoptr(PangoFontDescription) font =
    pango_font_description_from_string (style.family == TEXT_ROBOTO ? "Neo Roboto" : "Google Sans Flex, Neo Roboto");

  pango_font_description_set_absolute_size (font, style.size * PANGO_SCALE);
  pango_font_description_set_weight (font, style.weight);
  pango_layout_set_font_description (layout, font);
}

PangoLayout *
text_layout (GtkWidget *widget, TextStyle style, const char *text, double width)
{
  PangoLayout *layout = gtk_widget_create_pango_layout (widget, text);

  text_layout_set_style (layout, style);
  pango_layout_set_single_paragraph_mode (layout, TRUE);
  if (width > 0)
    {
      pango_layout_set_width (layout, (int) (width * PANGO_SCALE));
      pango_layout_set_ellipsize (layout, PANGO_ELLIPSIZE_END);
    }
  return layout;
}

double
text_box_height (PangoLayout *layout, TextStyle style)
{
  int width, height;

  pango_layout_get_size (layout, &width, &height);
  return pad_top (style) + height / (double) PANGO_SCALE + pad_bottom (style);
}

double
text_width (PangoLayout *layout)
{
  PangoRectangle logical;

  pango_layout_get_extents (layout, NULL, &logical);
  return logical.width / (double) PANGO_SCALE;
}

void
text_draw (GtkSnapshot *snapshot, PangoLayout *layout, TextStyle style, double x, double box_top,
           const GdkRGBA *color)
{
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (x, box_top + pad_top (style)));
  gtk_snapshot_append_layout (snapshot, layout, color);
  gtk_snapshot_restore (snapshot);
}
