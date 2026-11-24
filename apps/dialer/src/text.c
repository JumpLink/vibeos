/* Text as the Dialer draws it: see text.h. */
#include "text.h"

double
text_px (TextStyle style)
{
  return vibeos_sp (style.size);
}

/* font padding: the box above the ascent and below the descent, as fractions of the size. Roboto's head
 * yMax/yMin are 2163/-555 against its hhea 1900/-500 of 2048 units; Google Sans Flex's 2210/-605 against
 * 1932/-572 of 2000 */
static double
pad_top (TextStyle style)
{
  return text_px (style) * (style.family == TEXT_ROBOTO ? 263 / 2048.0 : 278 / 2000.0);
}

static double
pad_bottom (TextStyle style)
{
  return text_px (style) * (style.family == TEXT_ROBOTO ? 55 / 2048.0 : 33 / 2000.0);
}

GdkRGBA
text_rgb (VibeosColor color)
{
  return vibeos_color (color);
}

void
text_layout_set_style (PangoLayout *layout, TextStyle style)
{
  g_autoptr(PangoFontDescription) font =
    pango_font_description_from_string (style.family == TEXT_ROBOTO ? "VibeOS Roboto" : "Google Sans Flex, VibeOS Roboto");

  pango_font_description_set_absolute_size (font, text_px (style) * PANGO_SCALE);
  /* the style's weight is Bold text's: the regular one is 300 lighter */
  pango_font_description_set_weight (font, vibeos_font_weight (style.weight - 300));
  pango_layout_set_font_description (layout, font);
}

/* a layout made before Font size or Bold text changed gets the style's new size and weight as it is next measured
 * or drawn */
static void
text_layout_update (PangoLayout *layout, TextStyle style)
{
  const PangoFontDescription *font = pango_layout_get_font_description (layout);

  if (!font || pango_font_description_get_size (font) != (int) (text_px (style) * PANGO_SCALE)
      || (int) pango_font_description_get_weight (font) != vibeos_font_weight (style.weight - 300))
    text_layout_set_style (layout, style);
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

  text_layout_update (layout, style);
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
  text_layout_update (layout, style);
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (x, box_top + pad_top (style)));
  gtk_snapshot_append_layout (snapshot, layout, color);
  gtk_snapshot_restore (snapshot);
}
