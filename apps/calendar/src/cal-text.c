/* Text and rectangles in device pixels (cal-text.h). */
#include "cal-text.h"

#include <math.h>

void
cal_text_line (double size, int scale, double *baseline, double *height)
{
  double px = size * scale;
  double top = ceil (px * 2163 / 2048 - 0.001), bottom = ceil (px * 555 / 2048 - 0.001);

  /* the box rounded outward: 18 px text at scale 3 is 58 + 15 = 73 device pixels */
  if (baseline)
    *baseline = top / scale;
  if (height)
    *height = (top + bottom) / scale;
}

void
cal_paint_line (double size, int scale, double *ascent, double *height)
{
  double px = size * scale;

  if (ascent)
    *ascent = floor (px * 1900 / 2048 + 0.5) / scale;
  if (height)
    *height = floor (px * 2400 / 2048 + 0.5) / scale;
}

PangoLayout *
cal_text_layout (GtkWidget *widget, const char *text, double size, int weight)
{
  PangoLayout *layout = gtk_widget_create_pango_layout (widget, text);
  PangoFontDescription *font = pango_font_description_from_string (CAL_FONT);

  pango_font_description_set_absolute_size (font, size * PANGO_SCALE);
  pango_font_description_set_weight (font, weight);
  pango_layout_set_font_description (layout, font);
  pango_font_description_free (font);
  return layout;
}

void
cal_text_draw (GtkSnapshot *snapshot, PangoLayout *layout, double x, double baseline, double align,
               const GdkRGBA *color)
{
  int width, height;

  pango_layout_get_pixel_size (layout, &width, &height);
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT ((float) (x - align * width),
                                                          (float) (baseline - pango_layout_get_baseline (layout)
                                                                   / (double) PANGO_SCALE)));
  gtk_snapshot_append_layout (snapshot, layout, color);
  gtk_snapshot_restore (snapshot);
}

void
cal_text_ellipsize (PangoLayout *layout, double width)
{
  pango_layout_set_width (layout, (int) (MAX (0, width) * PANGO_SCALE));
  pango_layout_set_ellipsize (layout, PANGO_ELLIPSIZE_END);
  pango_layout_set_single_paragraph_mode (layout, TRUE);
}

void
cal_text_strike (PangoLayout *layout)
{
  PangoAttrList *attrs = pango_layout_get_attributes (layout) ? pango_attr_list_copy (pango_layout_get_attributes (layout))
                                                              : pango_attr_list_new ();

  pango_attr_list_insert (attrs, pango_attr_strikethrough_new (TRUE));
  pango_layout_set_attributes (layout, attrs);
  pango_attr_list_unref (attrs);
}

void
cal_fill_rect (GtkSnapshot *snapshot, int scale, double x, double y, double w, double h, const GdkRGBA *color)
{
  double x0 = round (x * scale) / scale, y0 = round (y * scale) / scale;
  double x1 = round ((x + w) * scale) / scale, y1 = round ((y + h) * scale) / scale;

  if (x1 > x0 && y1 > y0)
    gtk_snapshot_append_color (snapshot, color, &GRAPHENE_RECT_INIT ((float) x0, (float) y0, (float) (x1 - x0),
                                                                     (float) (y1 - y0)));
}
