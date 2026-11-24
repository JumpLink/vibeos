/* calc-text: see calc-text.h */
#include "calc-text.h"

#include <math.h>
#include <vibeos.h>

/* the letter spacing of a calc_text_ui_layout, px */
static double
letter_spacing (PangoLayout *layout)
{
  double *spacing = g_object_get_data (G_OBJECT (layout), "calc-letter-spacing");

  return spacing ? *spacing : 0;
}

void
calc_text_set_size (PangoLayout *layout, double size, int weight)
{
  PangoFontDescription *font = pango_font_description_new ();

  pango_font_description_set_family (font, CALC_FONT);
  /* the weight is Bold text's (CALC_WEIGHT_*): Bold text off draws it 300 lighter */
  pango_font_description_set_weight (font, vibeos_font_weight (weight - 300));
  pango_font_description_set_absolute_size (font, size * PANGO_SCALE);
  pango_layout_set_font_description (layout, font);
  pango_font_description_free (font);
}

PangoLayout *
calc_text_layout (GtkWidget *widget, const char *text, double size, int weight)
{
  PangoLayout *layout = gtk_widget_create_pango_layout (widget, text);
  PangoAttrList *attrs = pango_attr_list_new ();

  pango_attr_list_insert (attrs, pango_attr_font_features_new ("tnum, cv03, cv04"));
  pango_layout_set_attributes (layout, attrs);
  pango_attr_list_unref (attrs);
  calc_text_set_size (layout, size, weight);
  return layout;
}

PangoLayout *
calc_text_ui_layout (GtkWidget *widget, const char *text, double size, int weight, double spacing)
{
  PangoLayout *layout = gtk_widget_create_pango_layout (widget, text);
  double *px = g_new (double, 1);

  calc_text_set_size (layout, size, weight);
  *px = spacing * size;
  if (*px != 0) {
    PangoAttrList *attrs = pango_attr_list_new ();

    pango_attr_list_insert (attrs, pango_attr_letter_spacing_new ((int) round (*px * PANGO_SCALE)));
    pango_layout_set_attributes (layout, attrs);
    pango_attr_list_unref (attrs);
  }
  g_object_set_data_full (G_OBJECT (layout), "calc-letter-spacing", px, g_free);
  return layout;
}

double
calc_text_width (PangoLayout *layout)
{
  PangoRectangle logical;

  pango_layout_get_extents (layout, NULL, &logical);
  /* Pango spaces the letters apart; here each one is padded by half the spacing on either side, so the line is a
   * whole spacing wider */
  return logical.width / (double) PANGO_SCALE + letter_spacing (layout);
}

double
calc_text_bottom_baseline (double height, double padding_bottom, double size, int scale)
{
  /* in device pixels: min (padding, ceil (descent)) comes off the bottom padding, and the line's bottom (the
   * descent rounded) goes on what is left */
  double descent = CALC_DESCENT * size * scale;
  double padding = round (padding_bottom * scale);
  double compound = padding - MIN (padding, ceil (descent));

  return (round (height * scale) - compound - round (descent)) / scale;
}

/* the natural height of a text box holding one line of `size`, px */
double
calc_text_view_height (double padding_top, double padding_bottom, double size, int scale)
{
  double px = size * scale;
  double ascent = CALC_ASCENT * px, descent = CALC_DESCENT * px;
  double top = round (padding_top * scale), bottom = round (padding_bottom * scale);
  /* the top padding loses what lies between the ascent and the top of a capital H (rounded out) */
  double top_offset = MIN (top, ceil (ascent - ceil (CALC_CAP_HEIGHT * px)));
  double bottom_offset = MIN (bottom, ceil (descent));

  /* the line: the ascent and descent, rounded. A history row comes out 139 px */
  return (top - top_offset + round (ascent) + round (descent) + bottom - bottom_offset) / scale;
}

void
calc_text_draw (GtkSnapshot *snapshot, PangoLayout *layout, double x, double baseline, const GdkRGBA *color)
{
  double top = baseline - pango_layout_get_baseline (layout) / (double) PANGO_SCALE;

  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (x + letter_spacing (layout) / 2, top));
  gtk_snapshot_append_layout (snapshot, layout, color);
  gtk_snapshot_restore (snapshot);
}
