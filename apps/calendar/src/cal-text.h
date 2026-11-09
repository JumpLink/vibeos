/* Text the way Etar's views draw it, for the widgets that draw themselves (Month, Agenda, the date picker).
 *
 * Two kinds of text are on screen, and they are measured differently:
 *  - a TextView's line, with includeFontPadding: Roboto's bounding box (yMax 2163, yMin −555 of 2048 units) rounded
 *    outward to device pixels. Its top is where the TextView's bounds start; cal_text_line gives the baseline and
 *    the height. The "Bold text" setting adds 300 to its weight.
 *  - a canvas Paint's text (Month): Paint.ascent()/descent() are the hhea ascender and descender (1900, −500), and
 *    the Bold-text setting does not reach it; setFakeBoldText draws Regular as bold.
 * Sizes in dp; @scale is the device pixels per dp (neo_device_scale), so lines land on whole device pixels as on the
 * phone. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CAL_FONT "Neo Roboto, Roboto"

/* A TextView line of @size sp: its baseline below the line's top, and the line's height, in dp */
void         cal_text_line   (double size, int scale, double *baseline, double *height);
/* Paint's FontMetricsInt-style ascent and height ((int)(x + 0.5) of −ascent and descent − ascent), in dp */
void         cal_paint_line  (double size, int scale, double *ascent, double *height);

PangoLayout *cal_text_layout (GtkWidget *widget, const char *text, double size, int weight);
/* draws @layout with its first baseline at (@x, @baseline); @align 0 left, 1 right of @x, 0.5 centred on it */
void         cal_text_draw   (GtkSnapshot *snapshot, PangoLayout *layout, double x, double baseline, double align,
                              const GdkRGBA *color);
/* a one-line layout cut to @width with an ellipsis at the end (TextUtils.ellipsize, TruncateAt.END) */
void         cal_text_ellipsize (PangoLayout *layout, double width);

/* a device-pixel-exact rectangle: [x, x + w) × [y, y + h) rounded to device pixels */
void         cal_fill_rect   (GtkSnapshot *snapshot, int scale, double x, double y, double w, double h,
                              const GdkRGBA *color);

G_END_DECLS
