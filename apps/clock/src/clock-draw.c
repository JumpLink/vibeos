/* The rings' shapes: see clock-draw.h. */
#include "clock-draw.h"

#include <math.h>
#include <neo.h>

#define MAX_RINGS 16     /* textures kept: a ring per size, and a size or two more while one changes */
#define MAX_BOUNDS 16    /* where colours change: two per arc */

/* --- the ring's shape, rasterised once */

/* the ring, white, in device pixels: a square from (x, y) `size` wide, its centre at the given device position */
static GdkTexture *
ring_texture (double cx, double cy, double radius, double width, int *x, int *y, int *size)
{
  static GHashTable *rings;
  double outer = radius + width / 2 + 1;  /* a pixel for the antialiasing */
  g_autofree char *key = NULL;
  GdkTexture *texture;
  cairo_surface_t *surface;
  cairo_t *cr;
  GBytes *bytes;

  *x = floor (cx - outer);
  *y = floor (cy - outer);
  *size = ceil (MAX (cx + outer - *x, cy + outer - *y));
  /* the same ring at another sub-pixel position is not the same picture */
  key = g_strdup_printf ("%.2f %.2f %.2f %.2f", radius, width, cx - *x, cy - *y);
  if (!rings)
    rings = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  texture = g_hash_table_lookup (rings, key);
  if (texture)
    return texture;
  if (g_hash_table_size (rings) >= MAX_RINGS)
    g_hash_table_remove_all (rings);

  surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, *size, *size);
  cr = cairo_create (surface);
  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_set_line_width (cr, width);
  cairo_arc (cr, cx - *x, cy - *y, radius, 0, 2 * G_PI);
  cairo_stroke (cr);
  cairo_destroy (cr);
  cairo_surface_flush (surface);
  bytes = g_bytes_new_with_free_func (cairo_image_surface_get_data (surface),
                                      cairo_image_surface_get_stride (surface) * *size,
                                      (GDestroyNotify) cairo_surface_destroy, surface);
  /* cairo's ARGB32 is GDK's default format: premultiplied, in native order */
  texture = gdk_memory_texture_new (*size, *size, GDK_MEMORY_DEFAULT, bytes, cairo_image_surface_get_stride (surface));
  g_bytes_unref (bytes);
  g_hash_table_insert (rings, g_steal_pointer (&key), texture);
  return texture;
}

/* --- the colours on it */

/* Canvas degrees as a fraction of the circle clockwise from 12 o'clock, in [0, 1) */
static double
fraction (double degrees)
{
  double f = fmod ((degrees - 270) / 360, 1);

  return f < 0 ? f + 1 : f;
}

/* the arc as the part of the circle from `from` clockwise for `length`, both fractions */
static void
arc_span (const ClockArc *arc, double *from, double *length)
{
  *from = fraction (arc->sweep < 0 ? arc->start + arc->sweep : arc->start);
  *length = MIN (fabs (arc->sweep) / 360, 1);
}

/* the colour at `t` (a fraction): the last arc drawn there, or none */
static GdkRGBA
color_at (const ClockArc *arcs, guint n_arcs, double t)
{
  for (guint i = n_arcs; i-- > 0;)
    {
      double from, length, d;

      arc_span (&arcs[i], &from, &length);
      d = t - from;
      if (length >= 1 || (d < 0 ? d + 1 : d) < length)
        return arcs[i].color;
    }
  return (GdkRGBA) { 0, 0, 0, 0 };
}

static int
compare_doubles (gconstpointer a, gconstpointer b)
{
  double x = *(const double *) a, y = *(const double *) b;

  return (x > y) - (x < y);
}

void
clock_snapshot_arcs (GtkSnapshot *snapshot, int scale, double cx, double cy, double radius, double width,
                     const ClockArc *arcs, guint n_arcs)
{
  double bounds[MAX_BOUNDS];
  GdkRGBA colors[MAX_BOUNDS];
  guint n_bounds = 0, kept = 0;
  GdkTexture *texture;
  graphene_rect_t box;
  int x, y, size;

  if (radius <= 0 || !n_arcs)
    return;
  texture = ring_texture (cx * scale, cy * scale, radius * scale, width * scale, &x, &y, &size);
  box = GRAPHENE_RECT_INIT ((double) x / scale, (double) y / scale, (double) size / scale, (double) size / scale);

  /* where the colour may change: the ends of the arcs that are not whole circles, each once */
  for (guint i = 0; i < n_arcs && n_bounds + 2 <= MAX_BOUNDS; i++)
    {
      double from, length;

      arc_span (&arcs[i], &from, &length);
      if (length <= 0 || length >= 1)
        continue;
      bounds[n_bounds++] = from;
      bounds[n_bounds++] = fmod (from + length, 1);
    }
  qsort (bounds, n_bounds, sizeof *bounds, compare_doubles);
  for (guint i = 0; i < n_bounds; i++)
    if (!kept || bounds[i] - bounds[kept - 1] > 1e-9)
      bounds[kept++] = bounds[i];
  n_bounds = kept;
  /* the colour of each part, from one bound to the next and from the last round to the first */
  for (guint i = 0; i < n_bounds; i++)
    colors[i] = color_at (arcs, n_arcs, fmod ((bounds[i] + (i + 1 < n_bounds ? bounds[i + 1] : bounds[0] + 1)) / 2, 1));

  gtk_snapshot_push_mask (snapshot, GSK_MASK_MODE_ALPHA);
  gtk_snapshot_append_texture (snapshot, texture, &box);
  gtk_snapshot_pop (snapshot);
  /* The ring in the last part's colour, and every other part over it as a wedge: a conic gradient from the part's
   * start, its colour up to the part's length and nothing after. One gradient with a stop for every change would
   * do, but under a mask GTK's GL renderer on this phone's Mali gets the fourth stop's colour wrong (it comes out
   * blue); two stops never use it. The gradient's own supersampling smooths both edges. */
  if (n_bounds < 2)
    {
      GdkRGBA color = color_at (arcs, n_arcs, 0);

      gtk_snapshot_append_color (snapshot, &color, &box);
    }
  else
    {
      gtk_snapshot_append_color (snapshot, &colors[n_bounds - 1], &box);
      for (guint i = 0; i + 1 < n_bounds; i++)
        {
          double length = bounds[i + 1] - bounds[i];

          if (gdk_rgba_equal (&colors[i], &colors[n_bounds - 1]))
            continue;
          gtk_snapshot_append_conic_gradient (snapshot, &box, &GRAPHENE_POINT_INIT (cx, cy), bounds[i] * 360,
                                              (GskColorStop[]) { { length, colors[i] },
                                                                 { length, { 0, 0, 0, 0 } } }, 2);
        }
    }
  gtk_snapshot_pop (snapshot);
}

void
clock_snapshot_dot (GtkSnapshot *snapshot, double cx, double cy, double radius, const GdkRGBA *color)
{
  GskRoundedRect dot;

  gsk_rounded_rect_init_from_rect (&dot, &GRAPHENE_RECT_INIT (cx - radius, cy - radius, 2 * radius, 2 * radius),
                                   radius);
  gtk_snapshot_push_rounded_clip (snapshot, &dot);
  gtk_snapshot_append_color (snapshot, color, &dot.bounds);
  gtk_snapshot_pop (snapshot);
}

static int
text_width (PangoLayout *layout, double size)
{
  g_autoptr(PangoAttrList) attributes = pango_attr_list_new ();
  int width;

  pango_attr_list_insert (attributes, pango_attr_size_new_absolute (size * PANGO_SCALE));
  pango_layout_set_attributes (layout, attributes);
  pango_layout_get_pixel_size (layout, &width, NULL);
  return width;
}

void
clock_label_fit (GtkWidget *label, double max_size, double width)
{
  g_autoptr(PangoLayout) layout = gtk_widget_create_pango_layout (label, gtk_label_get_text (GTK_LABEL (label)));
  g_autoptr(PangoAttrList) attributes = NULL;
  int scale = neo_device_scale (label);
  double size, full = text_width (layout, max_size);

  if (width <= 0 || full <= width)
    {
      gtk_label_set_attributes (GTK_LABEL (label), NULL);
      return;
    }
  /* the width grows with the size: start where it would just fit, then down a pixel at a time while it does not */
  size = floor (max_size * width / full * scale) / scale;
  while (size > 1.0 / scale && text_width (layout, size) > width)
    size -= 1.0 / scale;
  attributes = pango_attr_list_new ();
  pango_attr_list_insert (attributes, pango_attr_size_new_absolute (size * PANGO_SCALE));
  gtk_label_set_attributes (GTK_LABEL (label), attributes);
}
