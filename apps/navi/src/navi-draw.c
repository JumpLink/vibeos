/* navi-draw: see navi-draw.h. Every shape here is Navi's own: thick round strokes in a unit box. */
#include "navi-draw.h"

#include <librsvg/rsvg.h>
#include <pango/pangocairo.h>

#include <math.h>

#define DEG (G_PI / 180)

/* the arrow's head: a triangle whose tip is `len` ahead of (x, y) along (dx, dy) */
static void
head (cairo_t *cr, double x, double y, double dx, double dy, double len, double half)
{
  cairo_move_to (cr, x + dx * len, y + dy * len);
  cairo_line_to (cr, x - dy * half, y + dx * half);
  cairo_line_to (cr, x + dy * half, y - dx * half);
  cairo_close_path (cr);
  cairo_fill (cr);
}

/* a turn by `angle` degrees to the right (0 straight on, 180 back): up from the bottom, round a bend, then the head */
static void
bend (cairo_t *cr, double angle)
{
  double x0, pivot, r, run;
  if (angle < 1) {
    x0 = 0.5, pivot = 0.36, r = 0, run = 0;
  } else if (angle < 60) {
    x0 = 0.38, pivot = 0.58, r = 0.3, run = 0.08;
  } else if (angle < 120) {
    x0 = 0.28, pivot = 0.58, r = 0.2, run = 0.12;
  } else if (angle < 170) {
    x0 = 0.28, pivot = 0.42, r = 0.2, run = 0.04;
  } else {
    x0 = 0.25, pivot = 0.46, r = 0.2, run = 0.06;
  }
  double t = angle * DEG, dx = sin (t), dy = -cos (t);
  cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
  cairo_set_line_width (cr, 0.15);
  cairo_move_to (cr, x0, 0.94);
  cairo_line_to (cr, x0, pivot);
  if (r > 0)
    cairo_arc (cr, x0 + r, pivot, r, G_PI, G_PI + t);
  double x, y;
  cairo_get_current_point (cr, &x, &y);
  x += dx * run;
  y += dy * run;
  cairo_line_to (cr, x, y);
  cairo_stroke (cr);
  head (cr, x - dx * 0.02, y - dy * 0.02, dx, dy, 0.28, 0.2);
}

static void
roundabout (cairo_t *cr, int exit_number, const GdkRGBA *color)
{
  const double cx = 0.5, cy = 0.42, r = 0.2;
  cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_width (cr, 0.09);
  cairo_arc (cr, cx, cy, r, 0, 2 * G_PI);
  cairo_stroke (cr);
  cairo_set_line_width (cr, 0.13);
  cairo_move_to (cr, cx, 0.95);
  cairo_line_to (cr, cx, cy + r);
  cairo_stroke (cr);
  /* the exit: first to the right, second straight on, third to the left (driving on the right) */
  double a = exit_number <= 1 ? 0 : exit_number == 2 ? -90 : exit_number == 3 ? -180 : -225;
  double dx = cos (a * DEG), dy = sin (a * DEG);
  double x = cx + dx * r, y = cy + dy * r;
  cairo_move_to (cr, x, y);
  cairo_line_to (cr, x + dx * 0.08, y + dy * 0.08);
  cairo_stroke (cr);
  head (cr, x + dx * 0.06, y + dy * 0.06, dx, dy, 0.2, 0.15);
  if (exit_number > 0) {
    char text[8];
    g_snprintf (text, sizeof text, "%d", exit_number);
    cairo_select_font_face (cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size (cr, 0.22);
    cairo_text_extents_t e;
    cairo_text_extents (cr, text, &e);
    cairo_move_to (cr, cx - e.x_bearing - e.width / 2, cy - e.y_bearing - e.height / 2);
    cairo_show_text (cr, text);
  }
}

void
navi_draw_finish (cairo_t *cr, double size)
{
  cairo_save (cr);
  cairo_scale (cr, size, size);
  cairo_arc (cr, 0.5, 0.5, 0.46, 0, 2 * G_PI);
  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_fill_preserve (cr);
  cairo_clip_preserve (cr);
  cairo_new_path (cr);
  cairo_set_source_rgb (cr, 0.1, 0.1, 0.1);
  const int n = 5;
  for (int i = 0; i < n; i++)
    for (int j = 0; j < n; j++)
      if ((i + j) % 2 == 0)
        cairo_rectangle (cr, 0.04 + i * 0.92 / n, 0.04 + j * 0.92 / n, 0.92 / n, 0.92 / n);
  cairo_fill (cr);
  cairo_reset_clip (cr);
  cairo_arc (cr, 0.5, 0.5, 0.46, 0, 2 * G_PI);
  cairo_set_line_width (cr, 0.06);
  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_stroke (cr);
  cairo_restore (cr);
}

void
navi_draw_turn (cairo_t *cr, double size, NaviTurn type, int roundabout_exit, const GdkRGBA *color)
{
  gboolean left = FALSE;
  double angle = 0;
  switch (type) {
  case NAVI_TURN_SLIGHT_LEFT: case NAVI_TURN_RAMP_LEFT: case NAVI_TURN_EXIT_LEFT: case NAVI_TURN_STAY_LEFT:
  case NAVI_TURN_MERGE_LEFT:
    left = TRUE; /* fall through */
  case NAVI_TURN_SLIGHT_RIGHT: case NAVI_TURN_RAMP_RIGHT: case NAVI_TURN_EXIT_RIGHT: case NAVI_TURN_STAY_RIGHT:
  case NAVI_TURN_MERGE_RIGHT:
    angle = 45;
    break;
  case NAVI_TURN_LEFT: case NAVI_TURN_START_LEFT: case NAVI_TURN_DESTINATION_LEFT:
    left = TRUE; /* fall through */
  case NAVI_TURN_RIGHT: case NAVI_TURN_START_RIGHT: case NAVI_TURN_DESTINATION_RIGHT:
    angle = 90;
    break;
  case NAVI_TURN_SHARP_LEFT:
    left = TRUE; /* fall through */
  case NAVI_TURN_SHARP_RIGHT:
    angle = 135;
    break;
  case NAVI_TURN_UTURN_LEFT:
    left = TRUE; /* fall through */
  case NAVI_TURN_UTURN_RIGHT:
    angle = 180;
    break;
  default:
    break;
  }
  if (type == NAVI_TURN_DESTINATION) {
    navi_draw_finish (cr, size);
    return;
  }
  cairo_save (cr);
  cairo_scale (cr, size, size);
  gdk_cairo_set_source_rgba (cr, color);
  if (type == NAVI_TURN_ROUNDABOUT_ENTER || type == NAVI_TURN_ROUNDABOUT_EXIT) {
    roundabout (cr, type == NAVI_TURN_ROUNDABOUT_ENTER ? roundabout_exit : 0, color);
  } else {
    if (left) {
      cairo_translate (cr, 1, 0);
      cairo_scale (cr, -1, 1);
    }
    bend (cr, angle);
  }
  cairo_restore (cr);
}

void
navi_draw_car (cairo_t *cr, double size, double rotation, double squash, gboolean night)
{
  double r = size / 2.0;
  cairo_save (cr);
  cairo_translate (cr, r, r);
  cairo_scale (cr, 1, squash);
  /* the halo the car sits in */
  cairo_arc (cr, 0, 0, r - 1, 0, 2 * G_PI);
  if (night)
    cairo_set_source_rgba (cr, 0.6, 0.75, 0.9, 0.16);
  else
    cairo_set_source_rgba (cr, 0.35, 0.39, 0.45, 0.18);
  cairo_fill (cr);
  cairo_rotate (cr, rotation * DEG);
  double a = r * 0.62;
  /* two faces: the lit one on the left, the shaded one on the right, as a raised arrow catching the light */
  cairo_move_to (cr, 0, -a);
  cairo_line_to (cr, a * 0.78, a * 0.78);
  cairo_line_to (cr, 0, a * 0.42);
  cairo_line_to (cr, -a * 0.78, a * 0.78);
  cairo_close_path (cr);
  cairo_set_source_rgb (cr, 0x14 / 255.0, 0x8f / 255.0, 0xaa / 255.0);
  cairo_set_line_width (cr, MAX (1.5, size * 0.035));
  cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
  cairo_stroke_preserve (cr);
  cairo_set_source_rgb (cr, 0x1c / 255.0, 0xb5 / 255.0, 0xd0 / 255.0);
  cairo_fill (cr);
  cairo_move_to (cr, 0, -a);
  cairo_line_to (cr, -a * 0.78, a * 0.78);
  cairo_line_to (cr, 0, a * 0.42);
  cairo_close_path (cr);
  cairo_set_source_rgb (cr, 0x5c / 255.0, 0xdc / 255.0, 0xf0 / 255.0);
  cairo_fill (cr);
  cairo_restore (cr);
}

void
navi_draw_pin (cairo_t *cr, double width, double height)
{
  double r = width / 2.0 - 1, cx = width / 2.0, cy = r + 1;
  cairo_save (cr);
  /* a drop: the circle, its sides meeting at the point */
  double tip = height - 1, a = asin (r / (tip - cy));
  cairo_move_to (cr, cx, tip);
  cairo_arc (cr, cx, cy, r, G_PI / 2 + a, 2 * G_PI + G_PI / 2 - a);
  cairo_close_path (cr);
  cairo_set_source_rgb (cr, 0xf2 / 255.0, 0x3d / 255.0, 0x3d / 255.0);
  cairo_fill_preserve (cr);
  cairo_set_source_rgb (cr, 0xb8 / 255.0, 0x1f / 255.0, 0x1f / 255.0);
  cairo_set_line_width (cr, 1.2);
  cairo_stroke (cr);
  cairo_arc (cr, cx, cy, r * 0.42, 0, 2 * G_PI);
  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_fill (cr);
  cairo_restore (cr);
}

void
navi_draw_crosshair (cairo_t *cr, double size, const GdkRGBA *color)
{
  cairo_save (cr);
  cairo_scale (cr, size, size);
  gdk_cairo_set_source_rgba (cr, color);
  cairo_set_line_width (cr, 0.1);
  cairo_arc (cr, 0.5, 0.5, 0.3, 0, 2 * G_PI);
  cairo_stroke (cr);
  cairo_arc (cr, 0.5, 0.5, 0.11, 0, 2 * G_PI);
  cairo_fill (cr);
  cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
  for (int i = 0; i < 4; i++) {
    double a = i * G_PI / 2, dx = cos (a), dy = sin (a);
    cairo_move_to (cr, 0.5 + dx * 0.3, 0.5 + dy * 0.3);
    cairo_line_to (cr, 0.5 + dx * 0.46, 0.5 + dy * 0.46);
  }
  cairo_stroke (cr);
  cairo_restore (cr);
}

static void
rounded_rect (cairo_t *cr, double x, double y, double w, double h, double r)
{
  cairo_new_sub_path (cr);
  cairo_arc (cr, x + w - r, y + r, r, -G_PI / 2, 0);
  cairo_arc (cr, x + w - r, y + h - r, r, 0, G_PI / 2);
  cairo_arc (cr, x + r, y + h - r, r, G_PI / 2, G_PI);
  cairo_arc (cr, x + r, y + r, r, G_PI, 1.5 * G_PI);
  cairo_close_path (cr);
}

void
navi_draw_camera (cairo_t *cr, double size, const GdkRGBA *color)
{
  cairo_save (cr);
  cairo_scale (cr, size, size);
  /* in a group of its own, so that its holes are holes whatever it is drawn on */
  cairo_push_group (cr);
  gdk_cairo_set_source_rgba (cr, color);
  /* the body and its viewfinder on top */
  rounded_rect (cr, 0.08, 0.3, 0.84, 0.54, 0.1);
  cairo_fill (cr);
  rounded_rect (cr, 0.3, 0.18, 0.3, 0.2, 0.05);
  cairo_fill (cr);
  /* the lens, a ring with its glass; the flash */
  cairo_set_operator (cr, CAIRO_OPERATOR_CLEAR);
  cairo_arc (cr, 0.5, 0.57, 0.17, 0, 2 * G_PI);
  cairo_fill (cr);
  cairo_arc (cr, 0.79, 0.41, 0.045, 0, 2 * G_PI);
  cairo_fill (cr);
  cairo_set_operator (cr, CAIRO_OPERATOR_OVER);
  cairo_arc (cr, 0.5, 0.57, 0.1, 0, 2 * G_PI);
  cairo_fill (cr);
  cairo_pop_group_to_source (cr);
  cairo_paint (cr);
  cairo_restore (cr);
}

cairo_surface_t *
navi_draw_camera_marker (double size, int scale)
{
  int px = (int) ceil ((size + 4) * scale);
  cairo_surface_t *out = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, px, px);
  cairo_t *cr = cairo_create (out);
  cairo_scale (cr, scale, scale);
  double c = (size + 4) / 2, r = size / 2;
  cairo_arc (cr, c, c + 0.9, r + 0.4, 0, 2 * G_PI);
  cairo_set_source_rgba (cr, 0, 0, 0, 0.28);
  cairo_fill (cr);
  cairo_arc (cr, c, c, r, 0, 2 * G_PI);
  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_fill (cr);
  cairo_arc (cr, c, c, r - MAX (1.5, size * 0.08), 0, 2 * G_PI);
  cairo_set_source_rgb (cr, 0xF0 / 255.0, 0x8C / 255.0, 0);
  cairo_fill (cr);
  /* the camera alone, then into the disc (its lens and flash are holes) */
  double g = size * 0.58;
  cairo_translate (cr, c - g / 2, c - g / 2);
  GdkRGBA white = { 1, 1, 1, 1 };
  navi_draw_camera (cr, g, &white);
  cairo_destroy (cr);
  cairo_surface_flush (out);
  return out;
}

cairo_surface_t *
navi_draw_signal_marker (double height, int scale)
{
  double w = height * 0.46;
  int pw = (int) ceil ((w + 3) * scale), ph = (int) ceil ((height + 3) * scale);
  cairo_surface_t *out = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, pw, ph);
  cairo_t *cr = cairo_create (out);
  cairo_scale (cr, scale, scale);
  rounded_rect (cr, 1.5, 2.2, w, height, w * 0.3);
  cairo_set_source_rgba (cr, 0, 0, 0, 0.25);
  cairo_fill (cr);
  rounded_rect (cr, 1.5, 1.5, w, height, w * 0.3);
  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_fill (cr);
  rounded_rect (cr, 2.5, 2.5, w - 2, height - 2, w * 0.26);
  cairo_set_source_rgb (cr, 0x26 / 255.0, 0x2A / 255.0, 0x30 / 255.0);
  cairo_fill (cr);
  static const double lights[3][3] = { { 0.94, 0.24, 0.24 }, { 0.98, 0.69, 0.12 }, { 0.2, 0.78, 0.35 } };
  double r = (w - 2) * 0.3;
  for (int i = 0; i < 3; i++) {
    cairo_arc (cr, 1.5 + w / 2, 1.5 + height * (0.2 + 0.3 * i), r, 0, 2 * G_PI);
    cairo_set_source_rgb (cr, lights[i][0], lights[i][1], lights[i][2]);
    cairo_fill (cr);
  }
  cairo_destroy (cr);
  cairo_surface_flush (out);
  return out;
}

void
navi_draw_limit_sign (cairo_t *cr, double size, int limit, gboolean mph)
{
  char text[8];
  g_snprintf (text, sizeof text, "%d", limit);
  cairo_save (cr);
  PangoLayout *layout = pango_cairo_create_layout (cr);
  PangoFontDescription *font = pango_font_description_from_string ("Sans Bold");
  if (mph && limit > 0) {
    /* the US's: a white board, a black edge, SPEED LIMIT over the number */
    double w = size * 0.82, x = (size - w) / 2;
    rounded_rect (cr, x, 0.5, w, size - 1, size * 0.1);
    cairo_set_source_rgb (cr, 1, 1, 1);
    cairo_fill_preserve (cr);
    cairo_set_source_rgba (cr, 0, 0, 0, 0.35);
    cairo_set_line_width (cr, 1);
    cairo_stroke (cr);
    rounded_rect (cr, x + size * 0.05, size * 0.05, w - size * 0.1, size * 0.9, size * 0.07);
    cairo_set_source_rgb (cr, 0.1, 0.1, 0.1);
    cairo_set_line_width (cr, MAX (1.2, size * 0.035));
    cairo_stroke (cr);
    pango_font_description_set_absolute_size (font, size * 0.15 * PANGO_SCALE);
    pango_layout_set_font_description (layout, font);
    pango_layout_set_alignment (layout, PANGO_ALIGN_CENTER);
    pango_layout_set_text (layout, "SPEED\nLIMIT", -1);
    int tw, th;
    pango_layout_get_pixel_size (layout, &tw, &th);
    cairo_move_to (cr, (size - tw) / 2.0, size * 0.1);
    pango_cairo_show_layout (cr, layout);
    pango_font_description_set_absolute_size (font, size * 0.4 * PANGO_SCALE);
    pango_layout_set_font_description (layout, font);
    pango_layout_set_text (layout, text, -1);
    pango_layout_get_pixel_size (layout, &tw, &th);
    cairo_move_to (cr, (size - tw) / 2.0, size * 0.45);
    pango_cairo_show_layout (cr, layout);
  } else {
    double c = size / 2, r = size / 2 - 0.5;
    cairo_arc (cr, c, c, r, 0, 2 * G_PI);
    cairo_set_source_rgb (cr, 1, 1, 1);
    cairo_fill (cr);
    if (limit > 0) {
      /* Europe's: a red ring */
      double ring = size * 0.11;
      cairo_arc (cr, c, c, r - ring / 2 - size * 0.02, 0, 2 * G_PI);
      cairo_set_source_rgb (cr, 0xD9 / 255.0, 0x1E / 255.0, 0x1E / 255.0);
      cairo_set_line_width (cr, ring);
      cairo_stroke (cr);
      pango_font_description_set_absolute_size (font, size * (limit >= 100 ? 0.33 : 0.42) * PANGO_SCALE);
      pango_layout_set_font_description (layout, font);
      pango_layout_set_text (layout, text, -1);
      int tw, th;
      pango_layout_get_pixel_size (layout, &tw, &th);
      cairo_move_to (cr, (size - tw) / 2.0, (size - th) / 2.0);
      cairo_set_source_rgb (cr, 0.08, 0.08, 0.08);
      pango_cairo_show_layout (cr, layout);
    } else {
      /* no limit: a thin dark edge, struck through by five fine lines */
      cairo_arc (cr, c, c, r - 0.5, 0, 2 * G_PI);
      cairo_set_source_rgba (cr, 0, 0, 0, 0.45);
      cairo_set_line_width (cr, 1);
      cairo_stroke (cr);
      cairo_arc (cr, c, c, r * 0.86, 0, 2 * G_PI);
      cairo_clip (cr);
      cairo_set_source_rgb (cr, 0.15, 0.15, 0.15);
      cairo_set_line_width (cr, size * 0.04);
      for (int i = -2; i <= 2; i++) {
        double o = i * size * 0.09;
        cairo_move_to (cr, c - r + o, c + r * 0.6 + o);
        cairo_line_to (cr, c + r * 0.6 + o, c - r + o);
      }
      cairo_stroke (cr);
    }
  }
  pango_font_description_free (font);
  g_object_unref (layout);
  cairo_restore (cr);
}

void
navi_draw_routes (cairo_t *cr, double size, const GdkRGBA *color)
{
  cairo_save (cr);
  cairo_scale (cr, size, size);
  gdk_cairo_set_source_rgba (cr, color);
  cairo_set_line_width (cr, 0.09);
  cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
  /* a road from the bottom, forking in two */
  cairo_move_to (cr, 0.5, 0.88);
  cairo_line_to (cr, 0.5, 0.56);
  cairo_line_to (cr, 0.24, 0.24);
  cairo_move_to (cr, 0.5, 0.56);
  cairo_line_to (cr, 0.76, 0.24);
  cairo_stroke (cr);
  for (int s = -1; s <= 1; s += 2) {
    double dx = s * 0.63, dy = -0.77;
    head (cr, 0.5 + s * 0.26 - dx * 0.02, 0.24 + 0.02 * 0.77, dx, dy, 0.16, 0.11);
  }
  cairo_restore (cr);
}

void
navi_draw_compass (cairo_t *cr, double size, double bearing, const GdkRGBA *color)
{
  cairo_save (cr);
  cairo_scale (cr, size, size);
  cairo_translate (cr, 0.5, 0.5);
  cairo_rotate (cr, -bearing * DEG);
  cairo_move_to (cr, 0, -0.4);
  cairo_line_to (cr, 0.13, 0);
  cairo_line_to (cr, -0.13, 0);
  cairo_close_path (cr);
  cairo_set_source_rgb (cr, 0xf2 / 255.0, 0x3d / 255.0, 0x3d / 255.0);
  cairo_fill (cr);
  cairo_move_to (cr, 0, 0.4);
  cairo_line_to (cr, 0.13, 0);
  cairo_line_to (cr, -0.13, 0);
  cairo_close_path (cr);
  gdk_cairo_set_source_rgba (cr, color);
  cairo_fill (cr);
  cairo_restore (cr);
}

static void
draw_fuel (cairo_t *cr, double size, const GdkRGBA *color)
{
  cairo_save (cr);
  cairo_scale (cr, size, size);
  gdk_cairo_set_source_rgba (cr, color);
  /* the pump: a body with a window cut out, its hose curling round to the right */
  cairo_rectangle (cr, 0.22, 0.16, 0.38, 0.7);
  cairo_rectangle (cr, 0.3, 0.24, 0.22, 0.18);
  cairo_set_fill_rule (cr, CAIRO_FILL_RULE_EVEN_ODD);
  cairo_fill (cr);
  cairo_rectangle (cr, 0.16, 0.82, 0.5, 0.06);
  cairo_fill (cr);
  cairo_set_line_width (cr, 0.07);
  cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
  cairo_move_to (cr, 0.6, 0.48);
  cairo_line_to (cr, 0.7, 0.48);
  cairo_line_to (cr, 0.7, 0.7);
  cairo_curve_to (cr, 0.7, 0.78, 0.82, 0.78, 0.82, 0.7);
  cairo_line_to (cr, 0.82, 0.34);
  cairo_line_to (cr, 0.72, 0.22);
  cairo_stroke (cr);
  cairo_restore (cr);
}

static void
draw_parking (cairo_t *cr, double size, const GdkRGBA *color)
{
  cairo_save (cr);
  cairo_scale (cr, size, size);
  gdk_cairo_set_source_rgba (cr, color);
  double r = 0.16, x = 0.14, y = 0.14, w = 0.72;
  cairo_new_sub_path (cr);
  cairo_arc (cr, x + w - r, y + r, r, -G_PI / 2, 0);
  cairo_arc (cr, x + w - r, y + w - r, r, 0, G_PI / 2);
  cairo_arc (cr, x + r, y + w - r, r, G_PI / 2, G_PI);
  cairo_arc (cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
  cairo_close_path (cr);
  cairo_set_line_width (cr, 0.08);
  cairo_stroke (cr);
  /* the P: a stem and its bowl */
  cairo_set_line_width (cr, 0.11);
  cairo_set_line_cap (cr, CAIRO_LINE_CAP_BUTT);
  cairo_move_to (cr, 0.4, 0.74);
  cairo_line_to (cr, 0.4, 0.28);
  cairo_line_to (cr, 0.52, 0.28);
  cairo_arc (cr, 0.52, 0.415, 0.135, -G_PI / 2, G_PI / 2);
  cairo_line_to (cr, 0.4, 0.55);
  cairo_stroke (cr);
  cairo_restore (cr);
}

static void
draw_work (cairo_t *cr, double size, const GdkRGBA *color)
{
  cairo_save (cr);
  cairo_scale (cr, size, size);
  gdk_cairo_set_source_rgba (cr, color);
  cairo_set_line_width (cr, 0.08);
  cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
  /* the handle, then the case */
  cairo_move_to (cr, 0.36, 0.3);
  cairo_line_to (cr, 0.36, 0.2);
  cairo_line_to (cr, 0.64, 0.2);
  cairo_line_to (cr, 0.64, 0.3);
  cairo_stroke (cr);
  cairo_rectangle (cr, 0.14, 0.3, 0.72, 0.5);
  cairo_fill (cr);
  cairo_restore (cr);
}

/* ---- the icon widget ---- */

typedef struct {
  NaviIconKind kind;
  NaviTurn turn;
  int exit_number;
  GdkRGBA color;
  gboolean follow; /* drawn in the widget's CSS colour */
} Icon;

static void
draw_icon (GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data)
{
  Icon *icon = data;
  double size = MIN (w, h);
  if (icon->follow)
    gtk_widget_get_color (GTK_WIDGET (area), &icon->color);
  cairo_translate (cr, (w - size) / 2, (h - size) / 2);
  switch (icon->kind) {
  case NAVI_ICON_TURN: navi_draw_turn (cr, size, icon->turn, icon->exit_number, &icon->color); break;
  case NAVI_ICON_CROSSHAIR: navi_draw_crosshair (cr, size, &icon->color); break;
  case NAVI_ICON_ROUTES: navi_draw_routes (cr, size, &icon->color); break;
  case NAVI_ICON_FINISH: navi_draw_finish (cr, size); break;
  case NAVI_ICON_FUEL: draw_fuel (cr, size, &icon->color); break;
  case NAVI_ICON_PARKING: draw_parking (cr, size, &icon->color); break;
  case NAVI_ICON_WORK: draw_work (cr, size, &icon->color); break;
  case NAVI_ICON_PIN:
    cairo_translate (cr, size * 0.2, 0);
    navi_draw_pin (cr, size * 0.6, size);
    break;
  case NAVI_ICON_CAMERA: navi_draw_camera (cr, size, &icon->color); break;
  }
}

GtkWidget *
navi_icon_new (NaviIconKind kind, int size)
{
  GtkWidget *area = gtk_drawing_area_new ();
  Icon *icon = g_new0 (Icon, 1);
  icon->kind = kind;
  icon->color = (GdkRGBA) { 1, 1, 1, 1 };
  g_object_set_data_full (G_OBJECT (area), "navi-icon", icon, g_free);
  gtk_drawing_area_set_content_width (GTK_DRAWING_AREA (area), size);
  gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (area), size);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (area), draw_icon, icon, NULL);
  gtk_widget_set_can_target (area, FALSE);
  return area;
}

void
navi_icon_set_turn (GtkWidget *area, NaviTurn type, int roundabout_exit)
{
  Icon *icon = g_object_get_data (G_OBJECT (area), "navi-icon");
  if (icon->turn == type && icon->exit_number == roundabout_exit)
    return;
  icon->turn = type;
  icon->exit_number = roundabout_exit;
  gtk_widget_queue_draw (area);
}

void
navi_icon_follow_ink (GtkWidget *area)
{
  Icon *icon = g_object_get_data (G_OBJECT (area), "navi-icon");
  icon->follow = TRUE;
  gtk_widget_add_css_class (area, "navi-ink");
  gtk_widget_queue_draw (area);
}

void
navi_icon_set_color (GtkWidget *area, const GdkRGBA *color)
{
  Icon *icon = g_object_get_data (G_OBJECT (area), "navi-icon");
  icon->follow = FALSE;
  if (gdk_rgba_equal (&icon->color, color))
    return;
  icon->color = *color;
  gtk_widget_queue_draw (area);
}

/* ---- points of interest ---- */

cairo_surface_t *
navi_draw_poi_marker (const char *svg_path, guint32 color, double size, int scale)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GObject) svg_ref = G_OBJECT (rsvg_handle_new_from_file (svg_path, &error));
  RsvgHandle *svg = (RsvgHandle *) svg_ref;
  if (!svg) {
    g_message ("poi icon: %s", error->message);
    return NULL;
  }
  int px = (int) ceil ((size + 4) * scale);
  cairo_surface_t *out = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, px, px);
  cairo_t *cr = cairo_create (out);
  cairo_scale (cr, scale, scale);
  double c = (size + 4) / 2, r = size / 2;
  /* the shadow, a little below */
  cairo_arc (cr, c, c + 0.9, r + 0.4, 0, 2 * G_PI);
  cairo_set_source_rgba (cr, 0, 0, 0, 0.26);
  cairo_fill (cr);
  /* the white ring, the disc */
  cairo_arc (cr, c, c, r, 0, 2 * G_PI);
  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_fill (cr);
  double ring = MAX (1.5, size * 0.075);
  cairo_arc (cr, c, c, r - ring, 0, 2 * G_PI);
  cairo_set_source_rgb (cr, ((color >> 16) & 0xff) / 255.0, ((color >> 8) & 0xff) / 255.0, (color & 0xff) / 255.0);
  cairo_fill (cr);
  /* the icon in white: drawn alone, then its shape used as a mask */
  double g = (r - ring) * 2 * 0.62;
  int gpx = (int) ceil (g * scale);
  cairo_surface_t *glyph = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, gpx, gpx);
  cairo_t *gc = cairo_create (glyph);
  RsvgRectangle viewport = { 0, 0, gpx, gpx };
  gboolean drawn = rsvg_handle_render_document (svg, gc, &viewport, &error);
  cairo_destroy (gc);
  if (drawn) {
    cairo_identity_matrix (cr);
    cairo_set_source_rgb (cr, 1, 1, 1);
    cairo_mask_surface (cr, glyph, round ((c - g / 2) * scale), round ((c - g / 2) * scale));
  } else {
    g_message ("poi icon: %s", error->message);
  }
  cairo_surface_destroy (glyph);
  cairo_destroy (cr);
  cairo_surface_flush (out);
  return out;
}

GdkTexture *
navi_poi_marker_texture (const char *svg_path, guint32 color, double size, int scale)
{
  cairo_surface_t *s = navi_draw_poi_marker (svg_path, color, size, scale);
  if (!s)
    return NULL;
  int w = cairo_image_surface_get_width (s), h = cairo_image_surface_get_height (s);
  int stride = cairo_image_surface_get_stride (s);
  g_autoptr (GBytes) bytes = g_bytes_new (cairo_image_surface_get_data (s), (gsize) stride * h);
  cairo_surface_destroy (s);
  /* cairo's ARGB32 is a native-endian word: B, G, R, A in memory on these little-endian machines */
  return gdk_memory_texture_new (w, h, GDK_MEMORY_DEFAULT, bytes, stride);
}
