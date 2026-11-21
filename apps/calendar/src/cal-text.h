/* Text for the widgets that draw themselves (Month, Agenda, the date picker).
 *
 * Two kinds of text are on screen, and they are measured differently:
 *  - a line with font padding (vibeos_label_set_font_padding): Roboto's bounding box (yMax 2163, yMin −555 of 2048
 *    units) rounded outward to device pixels. Its top is where the text's box starts; cal_text_line gives the
 *    baseline and the height. The "Bold text" setting adds 300 to its weight.
 *  - text drawn straight on a view (Month): its ascent and descent are the hhea ascender and descender (1900,
 *    −500), and the Bold text setting does not reach it; fake bold draws Regular as bold.
 * Sizes in sp: both kinds follow Font size (vibeos_sp); @scale is the device pixels per px (vibeos_device_scale), so
 * lines land on whole device pixels. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CAL_FONT "VibeOS Roboto, Roboto"

/* A padded line of @size sp text: its baseline below the line's top, and the line's height, in px */
void         cal_text_line   (double size, int scale, double *baseline, double *height);
/* A drawn line's ascent and height (the hhea ascender, and ascender − descender, each rounded to whole device
 * pixels), in px */
void         cal_paint_line  (double size, int scale, double *ascent, double *height);

/* a padded line's text: @weight as drawn with Bold text on (700 for a regular 400), which Bold text off draws 300
 * lighter */
PangoLayout *cal_text_layout (GtkWidget *widget, const char *text, double size, int weight);
/* text drawn straight on a view: @weight as it is, whatever Bold text says */
PangoLayout *cal_paint_layout (GtkWidget *widget, const char *text, double size, int weight);
/* draws @layout with its first baseline at (@x, @baseline); @align 0 left, 1 right of @x, 0.5 centred on it */
void         cal_text_draw   (GtkSnapshot *snapshot, PangoLayout *layout, double x, double baseline, double align,
                              const GdkRGBA *color);
/* a one-line layout cut to @width with an ellipsis at the end */
void         cal_text_ellipsize (PangoLayout *layout, double width);
/* a line struck through the whole text (a cancelled event's title) */
void         cal_text_strike (PangoLayout *layout);

/* a device-pixel-exact rectangle: [x, x + w) × [y, y + h) rounded to device pixels */
void         cal_fill_rect   (GtkSnapshot *snapshot, int scale, double x, double y, double w, double h,
                              const GdkRGBA *color);

G_END_DECLS
