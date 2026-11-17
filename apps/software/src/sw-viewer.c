/* SwViewer: see sw-viewer.h.
 *
 * Over the whole window, in its root overlay: a black backdrop, the pictures side by side in a horizontal
 * SpongyScrollView that pages one picture at a time, a dot per picture under them, a back arrow at the top left and a
 * close button at the top right (each on a dark circle, so that it shows over a light picture too).
 *
 * A page is an SwViewerPage: the store's 540 px picture and, over it, the full-size one when the store has one,
 * which fades in once it has loaded. The page draws both zoomed and moved as the viewer tells it.
 *
 * Its motion, all drawn as transforms of what is laid out once (nothing is laid out again while it moves):
 *
 *   opening    the picture grows from the thumbnail tapped to the screen, its corners straightening, while the
 *              backdrop darkens: 300 ms
 *   dragging   up or down (at 1x only), the picture follows the finger and shrinks a little, and the backdrop fades
 *              with the distance; let go past an eighth of the screen, or flung, it closes, otherwise it springs back
 *   zooming    two fingers pinch the picture larger or smaller about the point between them, and move it as they
 *              move; let go smaller than the screen or larger than MAX_ZOOM, it springs back. A double tap zooms in
 *              to DOUBLE_TAP_ZOOM about the point tapped, or back out to 1x. Zoomed in, one finger moves the
 *              picture, which stretches past its edges and springs back when let go, and a fling carries on. Zoomed
 *              in, the pages do not turn and the picture is not dragged away; a page turned to (from the keyboard)
 *              is at 1x again.
 *   closing    from wherever the picture is (zoomed in too), it shrinks into its thumbnail (the strip scrolled first
 *              to show it, if it was paged away from) while the backdrop fades: 250 ms. A thumbnail that is not on
 *              the screen any more: the picture just fades, shrinking a little. The close button closes it this way.
 *
 * A single tap hides the arrow, the close button and the dots, or shows them again.
 *
 * While it is open and still, the window under it is not drawn at all (it is hidden by the backdrop anyway): only the
 * picture is, frame after frame, while it pages or zooms. */
#include "sw-viewer.h"
#include "sw-image.h"
#include "sw-json.h"

#include <math.h>
#include <spongy-gtk.h>
#include <vibeos.h>

#define OPEN_MS       300
#define CLOSE_MS      250
#define BACK_MS       220
#define FADE_SPAN     0.4     /* of the height: dragged this far, the backdrop is as faded as it gets */
#define FADE_MOST     0.85
#define SHRINK_MOST   0.1     /* the picture's size given up as it is dragged */
#define CLOSE_SPAN    0.125   /* of the height: let go further than this, it closes */
#define CLOSE_FLING   800     /* px/s away from the middle: flung faster, it closes */
#define THUMB_RADIUS  12
#define DOT           6
#define DOT_GAP       8
#define MAX_ZOOM      4.0
#define DOUBLE_TAP_ZOOM 2.5
#define ZOOM_MS       250     /* a double tap's zoom, a pinch let go springing back */
#define PAN_SLOP      4       /* px a zoomed-in picture is moved before it follows the finger */
#define MIN_FLING     150     /* px/s: let go slower than this, a zoomed-in picture just stops */
#define CONTROLS_MS   150
#define CROSSFADE_MS  200     /* the full-size picture coming in over the small one */

/* ---- a page: the picture, small and full-size, zoomed -------------------------------------------------------- */

#define SW_TYPE_VIEWER_PAGE (sw_viewer_page_get_type ())
G_DECLARE_FINAL_TYPE (SwViewerPage, sw_viewer_page, SW, VIEWER_PAGE, GtkWidget)

struct _SwViewerPage {
  GtkWidget   parent_instance;
  GtkWidget  *small, *large;     /* the 540 px picture; the full-size one over it (NULL: the store has none) */
  VibeosAnim *crossfade;
  double      large_shown;       /* how far the full-size picture has faded in, 0 to 1 */
  double      zoom, x, y;        /* drawn `zoom` times as large, then moved by x, y */
};

G_DEFINE_FINAL_TYPE (SwViewerPage, sw_viewer_page, GTK_TYPE_WIDGET)

static void
sw_viewer_page_dispose (GObject *object)
{
  SwViewerPage *self = SW_VIEWER_PAGE (object);

  g_clear_pointer (&self->crossfade, vibeos_anim_free);
  g_clear_pointer (&self->small, gtk_widget_unparent);
  g_clear_pointer (&self->large, gtk_widget_unparent);
  G_OBJECT_CLASS (sw_viewer_page_parent_class)->dispose (object);
}

static void
sw_viewer_page_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  SwViewerPage *self = SW_VIEWER_PAGE (widget);
  gboolean moved = self->zoom != 1 || self->x != 0 || self->y != 0;

  if (moved)
    {
      gtk_snapshot_save (snapshot);
      gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (self->x, self->y));
      gtk_snapshot_scale (snapshot, self->zoom, self->zoom);
    }
  /* the small picture until the large one covers it */
  if (self->large_shown < 1)
    gtk_widget_snapshot_child (widget, self->small, snapshot);
  if (self->large && self->large_shown > 0)
    {
      if (self->large_shown < 1)
        gtk_snapshot_push_opacity (snapshot, self->large_shown);
      gtk_widget_snapshot_child (widget, self->large, snapshot);
      if (self->large_shown < 1)
        gtk_snapshot_pop (snapshot);
    }
  if (moved)
    gtk_snapshot_restore (snapshot);
}

static void
sw_viewer_page_class_init (SwViewerPageClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = sw_viewer_page_dispose;
  GTK_WIDGET_CLASS (klass)->snapshot = sw_viewer_page_snapshot;
  gtk_widget_class_set_layout_manager_type (GTK_WIDGET_CLASS (klass), GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "sw-viewer-page");
}

static void
sw_viewer_page_init (SwViewerPage *self)
{
  self->zoom = 1;
}

static void
on_crossfade (VibeosAnim *anim, double value, gpointer data)
{
  SwViewerPage *self = data;

  self->large_shown = value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_large_loaded (SwImage *large, SwViewerPage *self)
{
  /* not to be had: the small picture stays */
  if (!sw_image_has_picture (large))
    return;
  if (gtk_widget_get_mapped (GTK_WIDGET (self)))
    vibeos_anim_start (self->crossfade, self->large_shown, 1, CROSSFADE_MS, 0, SPONGY_LINEAR);
  else
    on_crossfade (self->crossfade, 1, self);
}

/* the API's Screenshot `shot`, in a box of width × height: its `url` at once, its `full` one over it once loaded */
static GtkWidget *
sw_viewer_page_new (JsonObject *shot, int width, int height)
{
  SwViewerPage *self = g_object_new (SW_TYPE_VIEWER_PAGE, NULL);
  JsonObject *full = sw_json_object (shot, "full");
  const char *large = full ? sw_json_string (full, "url") : NULL, *caption = sw_json_string (shot, "caption");

  self->crossfade = vibeos_anim_new (GTK_WIDGET (self), on_crossfade, self);
  self->small = sw_image_new (width, height, 0);
  sw_image_set_contain (SW_IMAGE (self->small), TRUE);
  sw_image_set_url (SW_IMAGE (self->small), sw_json_string (shot, "url"));
  if (caption)
    gtk_accessible_update_property (GTK_ACCESSIBLE (self->small), GTK_ACCESSIBLE_PROPERTY_LABEL, caption, -1);
  gtk_widget_set_parent (self->small, GTK_WIDGET (self));
  if (large && *large)
    {
      self->large = sw_image_new (width, height, 0);
      sw_image_set_contain (SW_IMAGE (self->large), TRUE);
      sw_image_set_url (SW_IMAGE (self->large), large);
      /* the same picture again, for the screen reader */
      gtk_accessible_update_state (GTK_ACCESSIBLE (self->large), GTK_ACCESSIBLE_STATE_HIDDEN, TRUE, -1);
      /* in memory already (opened before): there at once */
      if (sw_image_has_picture (SW_IMAGE (self->large)))
        self->large_shown = 1;
      g_signal_connect (self->large, "loaded", G_CALLBACK (on_large_loaded), self);
      gtk_widget_set_parent (self->large, GTK_WIDGET (self));
    }
  return GTK_WIDGET (self);
}

static void
sw_viewer_page_set_size (SwViewerPage *self, int width, int height)
{
  sw_image_set_size (SW_IMAGE (self->small), width, height);
  if (self->large)
    sw_image_set_size (SW_IMAGE (self->large), width, height);
}

static void
page_place (SwViewerPage *self, double zoom, double x, double y)
{
  if (self->zoom == zoom && self->x == x && self->y == y)
    return;
  self->zoom = zoom;
  self->x = x;
  self->y = y;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* zoomed in or out, or moved: not as it is at 1x */
static gboolean
page_is_zoomed (SwViewerPage *self)
{
  return self && (fabs (self->zoom - 1) > 0.001 || fabs (self->x) > 0.5 || fabs (self->y) > 0.5);
}

/* ---- the viewer ------------------------------------------------------------------------------------------------ */

struct _SwViewer {
  GtkWidget       parent_instance;
  GtkWidget      *pager, *pages, *back, *close;
  GPtrArray      *thumbs;        /* GWeakRef: the thumbnails, in order */
  GArray         *aspects;       /* double: width / height of each picture */
  guint           index;         /* the page opened at */
  int             page_w, page_h;
  gboolean        placed;        /* scrolled to the page opened at */

  /* the vertical drag */
  GtkGesture     *drag;
  SpongyVelocity  velocity;
  double          start_x, start_y, dy;
  double          taken;         /* the slop crossed before it moved: not part of dy */
  gboolean        claimed;
  VibeosAnim     *back_anim;

  /* opening and closing */
  VibeosAnim     *open_anim;
  double          open;          /* 0: at the thumbnail, 1: at `full` */
  graphene_rect_t thumb, full;
  double          fade;          /* the drag's fading, kept while it closes */
  gboolean        has_thumb, closing;
  GtkWidget      *hidden;        /* the thumbnail hidden while a picture grows out of it or shrinks into it */
  GtkWidget      *below;         /* the window's content, not drawn while the viewer covers it */
  GtkWidget      *focus_before;

  /* zooming: a pinch, a double tap, a zoomed-in picture moved by one finger (the vertical drag's gesture) */
  GtkGesture     *pinch, *taps;
  SwViewerPage   *zoomed;        /* the page showing, while it is not at 1x (or on its way there) */
  gboolean        pinching, panning;
  double          pinch_zoom;    /* the zoom the pinch started from */
  double          pinch_qx, pinch_qy;  /* the point of the picture under the fingers' middle (at 1x, page px) */
  double          pinch_mx, pinch_my;  /* the fingers' middle, the latest */
  double          pan_x, pan_y, pan_dx, pan_dy;  /* the picture's place, and the drag, when it began to follow */
  VibeosAnim     *zoom_anim;
  double          from_zoom, from_x, from_y, to_zoom, to_x, to_y;

  /* the arrow, the close button and the dots: shown (1) or hidden (0) by a single tap */
  VibeosAnim     *controls_anim;
  double          controls;
  gboolean        controls_hidden;
  guint           tap_timer;
};

G_DEFINE_FINAL_TYPE (SwViewer, sw_viewer, GTK_TYPE_WIDGET)

static double
clamp01 (double v)
{
  return CLAMP (v, 0, 1);
}

static guint
n_pages (SwViewer *self)
{
  return self->aspects->len;
}

/* the page showing, the nearest when between two */
static guint
current (SwViewer *self)
{
  double offset = spongy_scroll_view_get_offset (SPONGY_SCROLL_VIEW (self->pager));
  int w = gtk_widget_get_width (GTK_WIDGET (self));

  return w > 0 ? MIN ((guint) MAX (0, lround (offset / w)), n_pages (self) - 1) : self->index;
}

/* where page `i`'s picture is when it shows: as large as it fits, centred */
static graphene_rect_t
fitted (SwViewer *self, guint i)
{
  double w = gtk_widget_get_width (GTK_WIDGET (self)), h = gtk_widget_get_height (GTK_WIDGET (self));
  double a = i < n_pages (self) ? g_array_index (self->aspects, double, i) : 0.5;
  double fw = w / h > a ? h * a : w, fh = w / h > a ? h : w / a;

  return GRAPHENE_RECT_INIT ((w - fw) / 2, (h - fh) / 2, fw, fh);
}

static GtkWidget *
thumb_at (SwViewer *self, guint i)
{
  return i < self->thumbs->len ? g_weak_ref_get (g_ptr_array_index (self->thumbs, i)) : NULL;
}

static SwViewerPage *
page_at (SwViewer *self, guint i)
{
  GtkWidget *page = gtk_widget_get_first_child (self->pages);

  for (; page && i > 0; i--)
    page = gtk_widget_get_next_sibling (page);
  return page ? SW_VIEWER_PAGE (page) : NULL;
}

/* where page i's picture may be, zoomed `zoom` times: x between x_lo and x_hi, y between y_lo and y_hi. Along an
 * axis it does not fill, both are the place that centres it. */
static void
page_bounds (SwViewer *self, guint i, double zoom, double *x_lo, double *x_hi, double *y_lo, double *y_hi)
{
  double w = gtk_widget_get_width (GTK_WIDGET (self)), h = gtk_widget_get_height (GTK_WIDGET (self));
  graphene_rect_t f = fitted (self, i);
  double fw = f.size.width * zoom, fh = f.size.height * zoom;

  if (fw <= w)
    *x_lo = *x_hi = (w - fw) / 2 - f.origin.x * zoom;
  else
    {
      *x_lo = w - fw - f.origin.x * zoom;
      *x_hi = -f.origin.x * zoom;
    }
  if (fh <= h)
    *y_lo = *y_hi = (h - fh) / 2 - f.origin.y * zoom;
  else
    {
      *y_lo = h - fh - f.origin.y * zoom;
      *y_hi = -f.origin.y * zoom;
    }
}

/* page i's picture zoomed from `zoom` at x, y to `to_zoom` about the point (px, py) of the screen, which stays where
 * it is as far as the picture's edges allow: its place then */
static void
zoom_about (SwViewer *self, guint i, double zoom, double x, double y, double px, double py, double to_zoom,
            double *to_x, double *to_y)
{
  double qx = (px - x) / zoom, qy = (py - y) / zoom, xl, xh, yl, yh;

  page_bounds (self, i, to_zoom, &xl, &xh, &yl, &yh);
  *to_x = CLAMP (px - qx * to_zoom, xl, xh);
  *to_y = CLAMP (py - qy * to_zoom, yl, yh);
}

/* a place past [lo, hi] given less and less, the way a list's end stretches; `size` the screen's along the axis */
static double
stretch (double v, double lo, double hi, double size)
{
  if (v < lo)
    return lo - size * spongy_edge_damp ((lo - v) / size);
  if (v > hi)
    return hi + size * spongy_edge_damp ((v - hi) / size);
  return v;
}

static void
on_zoom_anim (VibeosAnim *anim, double t, gpointer data)
{
  SwViewer *self = data;
  SwViewerPage *page = self->zoomed;

  if (!page)
    return;
  page_place (page, self->from_zoom + (self->to_zoom - self->from_zoom) * t, self->from_x + (self->to_x - self->from_x) * t,
              self->from_y + (self->to_y - self->from_y) * t);
  /* back at 1x: exactly */
  if (!vibeos_anim_is_running (anim) && !page_is_zoomed (page))
    {
      page_place (page, 1, 0, 0);
      self->zoomed = NULL;
    }
}

/* the page showing to `zoom` at x, y over `ms` */
static void
zoom_to (SwViewer *self, SwViewerPage *page, double zoom, double x, double y, guint ms, SpongyCurve curve)
{
  vibeos_anim_stop (self->zoom_anim);
  self->zoomed = page;
  self->from_zoom = page->zoom;
  self->from_x = page->x;
  self->from_y = page->y;
  self->to_zoom = zoom;
  self->to_x = x;
  self->to_y = y;
  if (zoom == page->zoom && x == page->x && y == page->y)
    on_zoom_anim (self->zoom_anim, 1, self);
  else
    vibeos_anim_start (self->zoom_anim, 0, 1, ms, 0, curve);
}

/* the zoomed page back to 1x at once (another page shows, the viewer closes) */
static void
unzoom (SwViewer *self)
{
  vibeos_anim_stop (self->zoom_anim);
  if (self->zoomed)
    page_place (self->zoomed, 1, 0, 0);
  self->zoomed = NULL;
}

/* the page showing within its bounds and between 1x and MAX_ZOOM, about the point (px, py) */
static void
settle (SwViewer *self, double px, double py)
{
  SwViewerPage *page = self->zoomed;
  double z, x, y;

  if (!page)
    return;
  z = CLAMP (page->zoom, 1, MAX_ZOOM);
  zoom_about (self, current (self), page->zoom, page->x, page->y, px, py, z, &x, &y);
  zoom_to (self, page, z, x, y, ZOOM_MS, SPONGY_FAST_OUT_SLOW_IN);
}

/* the window's content under it: drawn or not */
static void
cover_below (SwViewer *self, gboolean covered)
{
  if (self->below)
    gtk_widget_set_opacity (self->below, covered ? 0 : 1);
}

static void
hide_thumb (SwViewer *self, GtkWidget *thumb)
{
  if (self->hidden)
    gtk_widget_set_opacity (self->hidden, 1);
  g_clear_weak_pointer (&self->hidden);
  if (thumb)
    {
      g_set_weak_pointer (&self->hidden, thumb);
      gtk_widget_set_opacity (thumb, 0);
    }
}

/* ---- drawing --------------------------------------------------------------------------------------------------- */

static void
sw_viewer_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  SwViewer *self = SW_VIEWER (widget);
  double w = gtk_widget_get_width (widget), h = gtk_widget_get_height (widget);
  graphene_rect_t all = GRAPHENE_RECT_INIT (0, 0, w, h);
  gboolean moving = self->open < 1 || self->closing;
  double shown, backdrop;
  GdkRGBA black = { 0, 0, 0, 1 };

  if (w <= 0 || h <= 0)
    return;
  if (moving)
    {
      /* between the thumbnail and the screen: the picture's box, its corners rounding as it gets there */
      graphene_rect_t f = fitted (self, current (self)), r;
      double k, radius = THUMB_RADIUS * (1 - self->open);
      GskRoundedRect clip;

      graphene_rect_interpolate (&self->thumb, &self->full, self->open, &r);
      /* scaled to cover the box (a thumbnail may crop its picture) */
      k = MAX (r.size.width / f.size.width, r.size.height / f.size.height);
      backdrop = self->open * (1 - FADE_MOST * self->fade);
      shown = self->open * (1 - self->fade);
      black.alpha = backdrop;
      gtk_snapshot_append_color (snapshot, &black, &all);
      gsk_rounded_rect_init_from_rect (&clip, &r, radius);
      gtk_snapshot_push_rounded_clip (snapshot, &clip);
      gtk_snapshot_save (snapshot);
      gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (r.origin.x + r.size.width / 2,
                                                              r.origin.y + r.size.height / 2));
      gtk_snapshot_scale (snapshot, k, k);
      gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (-(f.origin.x + f.size.width / 2),
                                                              -(f.origin.y + f.size.height / 2)));
      gtk_widget_snapshot_child (widget, self->pager, snapshot);
      gtk_snapshot_restore (snapshot);
      gtk_snapshot_pop (snapshot);
    }
  else
    {
      /* open: dragged up or down by dy, a little smaller the further it goes */
      double p = clamp01 (fabs (self->dy) / (h * FADE_SPAN)), s = 1 - SHRINK_MOST * p;

      backdrop = 1 - FADE_MOST * p;
      shown = 1 - p;
      black.alpha = backdrop;
      gtk_snapshot_append_color (snapshot, &black, &all);
      gtk_snapshot_save (snapshot);
      gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (w / 2, h / 2 + self->dy));
      gtk_snapshot_scale (snapshot, s, s);
      gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (-w / 2, -h / 2));
      gtk_snapshot_push_clip (snapshot, &all);
      gtk_widget_snapshot_child (widget, self->pager, snapshot);
      gtk_snapshot_pop (snapshot);
      gtk_snapshot_restore (snapshot);
    }

  /* the dots, the arrow and the close button come and go with the backdrop (and a tap): each faded by itself, as one
   * group they would be a picture of the whole screen's size (from the top corners to the dots) every frame */
  if (shown * self->controls > 0.01)
    {
      guint n = n_pages (self);
      double offset = spongy_scroll_view_get_offset (SPONGY_SCROLL_VIEW (self->pager)), at = offset / w;
      double row = n * DOT + (n - 1) * DOT_GAP, x0 = (w - row) / 2, y = h - 24 - DOT;

      shown = clamp01 (shown) * self->controls;
      for (guint i = 0; n > 1 && i < n; i++)
        {
          /* the dot of the page showing lights up, sliding over as the pages slide */
          double lit = clamp01 (1 - fabs (at - i));
          GdkRGBA dot = { 1, 1, 1, (0.4 + 0.6 * lit) * shown };
          GskRoundedRect circle;

          gsk_rounded_rect_init_from_rect (&circle, &GRAPHENE_RECT_INIT (x0 + i * (DOT + DOT_GAP), y, DOT, DOT),
                                           DOT / 2.0);
          gtk_snapshot_push_rounded_clip (snapshot, &circle);
          gtk_snapshot_append_color (snapshot, &dot, &circle.bounds);
          gtk_snapshot_pop (snapshot);
        }
      gtk_snapshot_push_opacity (snapshot, shown);
      gtk_widget_snapshot_child (widget, self->back, snapshot);
      gtk_snapshot_pop (snapshot);
      gtk_snapshot_push_opacity (snapshot, shown);
      gtk_widget_snapshot_child (widget, self->close, snapshot);
      gtk_snapshot_pop (snapshot);
    }
}

static void
sw_viewer_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat, int *min_base,
                   int *nat_base)
{
  /* it takes what the overlay gives it: the whole window */
  *min = *nat = 0;
}

static gboolean
resize_pages (gpointer data)
{
  SwViewer *self = data;

  for (GtkWidget *page = gtk_widget_get_first_child (self->pages); page; page = gtk_widget_get_next_sibling (page))
    sw_viewer_page_set_size (SW_VIEWER_PAGE (page), self->page_w, self->page_h);
  return G_SOURCE_REMOVE;
}

static void
sw_viewer_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  SwViewer *self = SW_VIEWER (widget);
  int bw, bh;

  gtk_widget_allocate (self->pager, width, height, -1, NULL);
  gtk_widget_measure (self->back, GTK_ORIENTATION_HORIZONTAL, -1, &bw, NULL, NULL, NULL);
  gtk_widget_measure (self->back, GTK_ORIENTATION_VERTICAL, bw, &bh, NULL, NULL, NULL);
  gtk_widget_allocate (self->back, bw, bh, -1, gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (4, 4)));
  gtk_widget_measure (self->close, GTK_ORIENTATION_HORIZONTAL, -1, &bw, NULL, NULL, NULL);
  gtk_widget_measure (self->close, GTK_ORIENTATION_VERTICAL, bw, &bh, NULL, NULL, NULL);
  gtk_widget_allocate (self->close, bw, bh, -1, gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (width - bw - 4, 4)));
  /* the screen turned: pages the new size (laid out on the next frame; the pager keeps its page) */
  if ((width != self->page_w || height != self->page_h) && width > 0 && height > 0)
    {
      /* a zoom made for the old size is no good */
      unzoom (self);
      self->page_w = width;
      self->page_h = height;
      g_idle_add_full (G_PRIORITY_HIGH_IDLE, resize_pages, g_object_ref (self), g_object_unref);
    }
}

/* ---- opening and closing --------------------------------------------------------------------------------------- */

/* out of the overlay once closed: from an idle, since leaving it can drop the last reference while its animation's
 * frame callback runs */
static gboolean
leave_overlay (gpointer data)
{
  SwViewer *self = data;
  GtkWidget *parent = gtk_widget_get_parent (GTK_WIDGET (self));
  GtkWidget *focus = self->focus_before;

  hide_thumb (self, NULL);
  cover_below (self, FALSE);
  if (focus)
    g_object_ref (focus);
  if (GTK_IS_OVERLAY (parent))
    gtk_overlay_remove_overlay (GTK_OVERLAY (parent), GTK_WIDGET (self));
  /* the focus back where it was: on a phone nowhere, with a keyboard the screenshot that opened it */
  if (focus && gtk_widget_get_root (focus))
    gtk_widget_grab_focus (focus);
  g_clear_object (&focus);
  return G_SOURCE_REMOVE;
}

static void
on_open_anim (VibeosAnim *anim, double value, gpointer data)
{
  SwViewer *self = data;

  self->open = value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  if (vibeos_anim_is_running (anim))
    return;
  if (self->closing)
    {
      g_idle_add_full (G_PRIORITY_DEFAULT, leave_overlay, g_object_ref (self), g_object_unref);
      return;
    }
  /* open and still: the thumbnail is under the backdrop, the window not drawn */
  hide_thumb (self, NULL);
  cover_below (self, TRUE);
}

static void
close_viewer (SwViewer *self)
{
  guint page = current (self);
  GtkWidget *thumb = thumb_at (self, page), *strip;
  double h = gtk_widget_get_height (GTK_WIDGET (self)), w = gtk_widget_get_width (GTK_WIDGET (self));
  double p = clamp01 (fabs (self->dy) / (h * FADE_SPAN)), s = 1 - SHRINK_MOST * p;
  graphene_rect_t f, bounds;

  if (self->closing)
    {
      g_clear_object (&thumb);
      return;
    }
  self->closing = TRUE;
  vibeos_anim_stop (self->back_anim);
  gtk_event_controller_reset (GTK_EVENT_CONTROLLER (self->drag));
  gtk_event_controller_reset (GTK_EVENT_CONTROLLER (self->pinch));
  g_clear_handle_id (&self->tap_timer, g_source_remove);
  self->pinching = self->panning = FALSE;
  cover_below (self, FALSE);
  /* from where the picture is now: on its page (between two pages: the nearer), where a drag left it, or zoomed in
   * (the page itself back at 1x, the closing drawn from there) */
  spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (self->pager), page * w);
  f = fitted (self, page);
  if (self->zoomed)
    {
      SwViewerPage *z = self->zoomed;

      self->full = GRAPHENE_RECT_INIT (f.origin.x * z->zoom + z->x, f.origin.y * z->zoom + z->y, f.size.width * z->zoom,
                                       f.size.height * z->zoom);
      unzoom (self);
    }
  else
    self->full = GRAPHENE_RECT_INIT (w / 2 + (f.origin.x - w / 2) * s, h / 2 + self->dy + (f.origin.y - h / 2) * s,
                                     f.size.width * s, f.size.height * s);
  self->fade = p;
  self->dy = 0;
  /* into the thumbnail of the picture showing, its strip scrolled to show it all */
  strip = thumb ? gtk_widget_get_ancestor (thumb, SPONGY_TYPE_SCROLL_VIEW) : NULL;
  if (strip && gtk_widget_compute_bounds (thumb, strip, &bounds))
    {
      double sw = gtk_widget_get_width (strip), before = spongy_scroll_view_get_offset (SPONGY_SCROLL_VIEW (strip));
      double inset = 24, delta = 0;

      if (bounds.origin.x < inset)
        delta = bounds.origin.x - inset;
      else if (bounds.origin.x + bounds.size.width > sw - inset)
        delta = bounds.origin.x + bounds.size.width - (sw - inset);
      if (delta != 0)
        spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (strip), before + delta);
      delta = spongy_scroll_view_get_offset (SPONGY_SCROLL_VIEW (strip)) - before;
      self->has_thumb = gtk_widget_get_mapped (thumb) && gtk_widget_compute_bounds (thumb, GTK_WIDGET (self), &bounds);
      bounds.origin.x -= delta;
      /* on the screen at all */
      self->has_thumb = self->has_thumb && bounds.origin.y + bounds.size.height > 0 && bounds.origin.y < h;
    }
  else
    self->has_thumb = FALSE;
  if (self->has_thumb)
    {
      self->thumb = bounds;
      hide_thumb (self, thumb);
    }
  else
    {
      /* nowhere to go: it shrinks where it is while it fades */
      graphene_rect_t r = self->full;

      self->thumb = GRAPHENE_RECT_INIT (r.origin.x + r.size.width * 0.1, r.origin.y + r.size.height * 0.1,
                                        r.size.width * 0.8, r.size.height * 0.8);
    }
  vibeos_anim_start (self->open_anim, self->open, 0, CLOSE_MS, 0, SPONGY_FAST_OUT_SLOW_IN);
  g_clear_object (&thumb);
}

/* ---- input ----------------------------------------------------------------------------------------------------- */

static void
on_back_anim (VibeosAnim *anim, double value, gpointer data)
{
  SwViewer *self = data;

  self->dy = value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  /* back in place: the window under it hidden again, its thumbnail back */
  if (!vibeos_anim_is_running (anim) && !self->closing)
    {
      hide_thumb (self, NULL);
      cover_below (self, TRUE);
    }
}

static double
event_ms (GtkGesture *gesture)
{
  return gtk_event_controller_get_current_event_time (GTK_EVENT_CONTROLLER (gesture));
}

/* a zoomed-in picture let go: a fling carries it on (to its edge at most), else it stays; out of its bounds, or
 * zoomed past the ends, it springs back */
static void
pan_end (SwViewer *self, double vx, double vy)
{
  SwViewerPage *page = self->zoomed;
  guint i = current (self);
  double w = gtk_widget_get_width (GTK_WIDGET (self)), h = gtk_widget_get_height (GTK_WIDGET (self));
  double xl, xh, yl, yh, speed = hypot (vx, vy), distance, ms;

  if (!page)
    return;
  page_bounds (self, i, page->zoom, &xl, &xh, &yl, &yh);
  if (speed < MIN_FLING || page->zoom < 1 || page->zoom > MAX_ZOOM || page->x < xl - 0.5 || page->x > xh + 0.5
      || page->y < yl - 0.5 || page->y > yh + 0.5)
    {
      settle (self, w / 2, h / 2);
      return;
    }
  /* as far as the lists' fling goes, slowing down from the finger's speed (a cubic ease starts 3 times as fast as
   * its average) */
  distance = spongy_fling_distance (speed);
  ms = CLAMP (3000 * distance / speed, 120, 1500);
  zoom_to (self, page, page->zoom, CLAMP (page->x + vx / speed * distance, xl, xh),
           CLAMP (page->y + vy / speed * distance, yl, yh), (guint) ms, SPONGY_DECELERATE_CUBIC);
}

static void
drag_begin (GtkGestureDrag *drag, double x, double y, SwViewer *self)
{
  if (self->closing || self->open < 1 || self->pinching)
    {
      gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_DENIED);
      return;
    }
  self->claimed = FALSE;
  /* zoomed in: the finger moves the picture, in any direction */
  self->panning = self->zoomed && self->zoomed == page_at (self, current (self));
  self->start_x = x;
  self->start_y = y;
  spongy_velocity_clear (&self->velocity);
  spongy_velocity_add (&self->velocity, event_ms (GTK_GESTURE (drag)), x, y);
}

static void
drag_update (GtkGestureDrag *drag, double dx, double dy, SwViewer *self)
{
  double slop = 8;

  if (self->closing)
    return;
  spongy_velocity_add (&self->velocity, event_ms (GTK_GESTURE (drag)), self->start_x + dx, self->start_y + dy);
  if (self->panning)
    {
      SwViewerPage *page = self->zoomed;
      double w = gtk_widget_get_width (GTK_WIDGET (self)), h = gtk_widget_get_height (GTK_WIDGET (self));
      double xl, xh, yl, yh;

      if (!page)
        return;
      if (!self->claimed)
        {
          /* claimed sooner than the pages' slop: a zoomed-in picture does not page */
          if (hypot (dx, dy) <= PAN_SLOP)
            return;
          self->claimed = TRUE;
          gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_CLAIMED);
          vibeos_anim_stop (self->zoom_anim);
          g_clear_handle_id (&self->tap_timer, g_source_remove);
          self->pan_x = page->x;
          self->pan_y = page->y;
          self->pan_dx = dx;
          self->pan_dy = dy;
        }
      page_bounds (self, current (self), page->zoom, &xl, &xh, &yl, &yh);
      page_place (page, page->zoom, stretch (self->pan_x + dx - self->pan_dx, xl, xh, w),
                  stretch (self->pan_y + dy - self->pan_dy, yl, yh, h));
      return;
    }
  if (!self->claimed)
    {
      /* sideways past the slop first: the pages' */
      if (fabs (dx) > slop && fabs (dx) >= fabs (dy))
        {
          gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_DENIED);
          return;
        }
      if (fabs (dy) <= slop)
        return;
      /* up or down first: the picture follows the finger from here, the window under it shows through */
      self->claimed = TRUE;
      gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_CLAIMED);
      vibeos_anim_stop (self->back_anim);
      /* the window shows through as the backdrop fades, without the thumbnail the picture came out of */
      {
        GtkWidget *thumb = thumb_at (self, current (self));

        hide_thumb (self, thumb);
        g_clear_object (&thumb);
      }
      cover_below (self, FALSE);
      self->taken = dy > 0 ? slop : -slop;
      spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (self->pager),
                                    current (self) * gtk_widget_get_width (GTK_WIDGET (self)));
    }
  self->dy = dy - self->taken;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
drag_end (GtkGestureDrag *drag, double dx, double dy, SwViewer *self)
{
  float vx, vy;
  double h = gtk_widget_get_height (GTK_WIDGET (self));

  if (!self->claimed || self->closing)
    return;
  self->claimed = FALSE;
  spongy_velocity_get (&self->velocity, event_ms (GTK_GESTURE (drag)), &vx, &vy);
  if (self->panning)
    {
      pan_end (self, vx, vy);
      return;
    }
  if (fabs (self->dy) > h * CLOSE_SPAN || (fabs (vy) > CLOSE_FLING && (vy > 0) == (self->dy > 0)))
    close_viewer (self);
  else
    vibeos_anim_start (self->back_anim, self->dy, 0, BACK_MS, 0, SPONGY_FAST_OUT_SLOW_IN);
}

static void
drag_cancel (GtkGesture *gesture, GdkEventSequence *sequence, SwViewer *self)
{
  if (!self->claimed || self->closing)
    return;
  self->claimed = FALSE;
  if (self->panning)
    {
      /* taken by a pinch, which settles it, or cancelled: back within bounds */
      if (!self->pinching)
        pan_end (self, 0, 0);
      return;
    }
  vibeos_anim_start (self->back_anim, self->dy, 0, BACK_MS, 0, SPONGY_FAST_OUT_SLOW_IN);
}

/* ---- zooming --------------------------------------------------------------------------------------------------- */

static void
pinch_begin (GtkGesture *gesture, GdkEventSequence *sequence, SwViewer *self)
{
  guint i = current (self);
  double w = gtk_widget_get_width (GTK_WIDGET (self));
  double offset = spongy_scroll_view_get_offset (SPONGY_SCROLL_VIEW (self->pager));
  SwViewerPage *page = page_at (self, i);
  double mx, my;

  /* the viewer open and still, a page at rest (not turning), the picture not being dragged away */
  if (self->closing || self->open < 1 || (self->claimed && !self->panning) || fabs (offset - i * w) > 1 || !page
      || !gtk_gesture_get_bounding_box_center (gesture, &mx, &my))
    {
      gtk_gesture_set_state (gesture, GTK_EVENT_SEQUENCE_DENIED);
      return;
    }
  /* first: claiming cancels a drag moving the zoomed picture, which must leave it to the pinch */
  self->pinching = TRUE;
  gtk_gesture_set_state (gesture, GTK_EVENT_SEQUENCE_CLAIMED);
  vibeos_anim_stop (self->zoom_anim);
  g_clear_handle_id (&self->tap_timer, g_source_remove);
  self->zoomed = page;
  self->pinch_zoom = page->zoom;
  self->pinch_qx = (mx - page->x) / page->zoom;
  self->pinch_qy = (my - page->y) / page->zoom;
  self->pinch_mx = mx;
  self->pinch_my = my;
}

static void
pinch_scale (GtkGestureZoom *gesture, double scale, SwViewer *self)
{
  double zoom = self->pinch_zoom * scale, mx, my;

  if (!self->pinching || !self->zoomed || !gtk_gesture_get_bounding_box_center (GTK_GESTURE (gesture), &mx, &my))
    return;
  /* past the ends it gives less and less */
  if (zoom > MAX_ZOOM)
    zoom = MAX_ZOOM * pow (zoom / MAX_ZOOM, 0.3);
  else if (zoom < 1)
    zoom = MAX (pow (zoom, 0.5), 0.5);
  self->pinch_mx = mx;
  self->pinch_my = my;
  /* the point of the picture that was between the fingers stays between them */
  page_place (self->zoomed, zoom, mx - self->pinch_qx * zoom, my - self->pinch_qy * zoom);
}

static void
pinch_end (GtkGesture *gesture, GdkEventSequence *sequence, SwViewer *self)
{
  if (!self->pinching)
    return;
  self->pinching = FALSE;
  settle (self, self->pinch_mx, self->pinch_my);
}

static void
set_controls (SwViewer *self, gboolean shown)
{
  gtk_widget_set_can_target (self->back, shown);
  gtk_widget_set_can_target (self->close, shown);
  vibeos_anim_start (self->controls_anim, self->controls, shown ? 1 : 0, CONTROLS_MS, 0, SPONGY_LINEAR);
}

static void
on_controls_anim (VibeosAnim *anim, double value, gpointer data)
{
  SwViewer *self = data;

  self->controls = value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static gboolean
on_single_tap (gpointer data)
{
  SwViewer *self = data;

  self->tap_timer = 0;
  self->controls_hidden = !self->controls_hidden;
  set_controls (self, !self->controls_hidden);
  return G_SOURCE_REMOVE;
}

/* the point is on the arrow or the close button */
static gboolean
on_control (SwViewer *self, double x, double y)
{
  GtkWidget *picked = gtk_widget_pick (GTK_WIDGET (self), x, y, GTK_PICK_DEFAULT);

  return picked && (picked == self->back || picked == self->close || gtk_widget_is_ancestor (picked, self->back)
                    || gtk_widget_is_ancestor (picked, self->close));
}

static void
taps_pressed (GtkGestureClick *gesture, int n_press, double x, double y, SwViewer *self)
{
  guint i = current (self);
  SwViewerPage *page = page_at (self, i);
  double tx, ty;

  if (n_press != 2)
    return;
  g_clear_handle_id (&self->tap_timer, g_source_remove);
  if (self->closing || self->open < 1 || self->pinching || !page || on_control (self, x, y))
    return;
  /* zoomed: back to 1x; at 1x: in, the point tapped staying under the finger as far as the edges allow */
  if (self->zoomed)
    zoom_to (self, page, 1, 0, 0, ZOOM_MS, SPONGY_FAST_OUT_SLOW_IN);
  else
    {
      zoom_about (self, i, page->zoom, page->x, page->y, x, y, DOUBLE_TAP_ZOOM, &tx, &ty);
      zoom_to (self, page, DOUBLE_TAP_ZOOM, tx, ty, ZOOM_MS, SPONGY_FAST_OUT_SLOW_IN);
    }
}

static void
taps_released (GtkGestureClick *gesture, int n_press, double x, double y, SwViewer *self)
{
  int ms = 400;

  if (n_press != 1 || self->closing || self->open < 1 || on_control (self, x, y))
    return;
  /* one tap: the controls go or come once no second tap has followed */
  g_object_get (gtk_widget_get_settings (GTK_WIDGET (self)), "gtk-double-click-time", &ms, NULL);
  g_clear_handle_id (&self->tap_timer, g_source_remove);
  self->tap_timer = g_timeout_add (ms, on_single_tap, self);
}

static void
go_to_page (SwViewer *self, int page)
{
  int w = gtk_widget_get_width (GTK_WIDGET (self));

  page = CLAMP (page, 0, (int) n_pages (self) - 1);
  spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (self->pager), page * w);
}

static gboolean
on_key (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, SwViewer *self)
{
  gboolean alt = (state & GDK_ALT_MASK) != 0, left = keyval == GDK_KEY_Left || keyval == GDK_KEY_KP_Left;

  if (keyval == GDK_KEY_Escape || keyval == GDK_KEY_Back || (left && alt))
    close_viewer (self);
  else if (left)
    go_to_page (self, (int) current (self) - 1);
  else if (keyval == GDK_KEY_Right || keyval == GDK_KEY_KP_Right)
    go_to_page (self, (int) current (self) + 1);
  else
    return FALSE;
  return TRUE;
}

static void
on_back_clicked (GtkWidget *button, SwViewer *self)
{
  close_viewer (self);
}

static void
on_close_clicked (GtkWidget *button, SwViewer *self)
{
  close_viewer (self);
}

/* the pages are laid out: on the one opened at */
static void
on_pager_changed (GtkAdjustment *adj, SwViewer *self)
{
  int w = gtk_widget_get_width (GTK_WIDGET (self));

  if (self->placed || w <= 0 || gtk_adjustment_get_upper (adj) < (self->index + 1) * (double) w)
    return;
  self->placed = TRUE;
  spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (self->pager), self->index * (double) w);
}

static void
on_pager_offset (GObject *pager, GParamSpec *pspec, SwViewer *self)
{
  /* another page: the one zoomed back at 1x */
  if (self->zoomed && self->zoomed != page_at (self, current (self)))
    unzoom (self);
  /* the dots */
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* ---- GObject --------------------------------------------------------------------------------------------------- */

static void
sw_viewer_dispose (GObject *object)
{
  SwViewer *self = SW_VIEWER (object);

  hide_thumb (self, NULL);
  cover_below (self, FALSE);
  g_clear_weak_pointer (&self->below);
  g_clear_weak_pointer (&self->focus_before);
  g_clear_handle_id (&self->tap_timer, g_source_remove);
  self->zoomed = NULL;
  g_clear_pointer (&self->open_anim, vibeos_anim_free);
  g_clear_pointer (&self->back_anim, vibeos_anim_free);
  g_clear_pointer (&self->zoom_anim, vibeos_anim_free);
  g_clear_pointer (&self->controls_anim, vibeos_anim_free);
  g_clear_pointer (&self->pager, gtk_widget_unparent);
  g_clear_pointer (&self->back, gtk_widget_unparent);
  g_clear_pointer (&self->close, gtk_widget_unparent);
  G_OBJECT_CLASS (sw_viewer_parent_class)->dispose (object);
}

static void
sw_viewer_finalize (GObject *object)
{
  SwViewer *self = SW_VIEWER (object);

  g_ptr_array_unref (self->thumbs);
  g_array_unref (self->aspects);
  G_OBJECT_CLASS (sw_viewer_parent_class)->finalize (object);
}

static void
weak_ref_free (gpointer data)
{
  g_weak_ref_clear (data);
  g_free (data);
}

static void
sw_viewer_class_init (SwViewerClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = sw_viewer_dispose;
  G_OBJECT_CLASS (klass)->finalize = sw_viewer_finalize;
  widget_class->snapshot = sw_viewer_snapshot;
  widget_class->measure = sw_viewer_measure;
  widget_class->size_allocate = sw_viewer_size_allocate;
  gtk_widget_class_set_css_name (widget_class, "sw-viewer");
  /* a dialog to the shell's back gesture, which then sends Escape: it closes the viewer, not the page */
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_DIALOG);
}

static void
sw_viewer_init (SwViewer *self)
{
  GtkWidget *widget = GTK_WIDGET (self);
  GtkEventController *keys = gtk_event_controller_key_new ();
  GtkWidget *arrow = gtk_image_new_from_icon_name ("sw-back-symbolic");
  GtkWidget *cross = gtk_image_new_from_icon_name ("sw-close-symbolic");
  GdkRGBA scrim = { 0, 0, 0, 0.45 };

  self->thumbs = g_ptr_array_new_with_free_func (weak_ref_free);
  self->aspects = g_array_new (FALSE, FALSE, sizeof (double));
  self->open_anim = vibeos_anim_new (widget, on_open_anim, self);
  self->back_anim = vibeos_anim_new (widget, on_back_anim, self);
  self->zoom_anim = vibeos_anim_new (widget, on_zoom_anim, self);
  self->controls_anim = vibeos_anim_new (widget, on_controls_anim, self);
  self->controls = 1;
  gtk_widget_set_focusable (widget, TRUE);
  gtk_widget_set_overflow (widget, GTK_OVERFLOW_HIDDEN);

  self->pager = g_object_new (SPONGY_TYPE_SCROLL_VIEW, "orientation", GTK_ORIENTATION_HORIZONTAL, NULL);
  spongy_scroll_view_set_snap (SPONGY_SCROLL_VIEW (self->pager), 0, TRUE);
  self->pages = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_set_homogeneous (GTK_BOX (self->pages), TRUE);
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (self->pager), self->pages);
  gtk_widget_set_parent (self->pager, widget);
  g_signal_connect (spongy_scroll_view_get_hadjustment (SPONGY_SCROLL_VIEW (self->pager)), "changed",
                    G_CALLBACK (on_pager_changed), self);
  g_signal_connect (self->pager, "notify::offset", G_CALLBACK (on_pager_offset), self);

  gtk_image_set_pixel_size (GTK_IMAGE (arrow), 24);
  self->back = vibeos_pressable_new (arrow);
  gtk_widget_add_css_class (self->back, "sw-viewer-back");
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (self->back), VIBEOS_RIPPLE_CIRCLE, 20);
  vibeos_pressable_set_fill (VIBEOS_PRESSABLE (self->back), &scrim, FALSE);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->back), GTK_ACCESSIBLE_PROPERTY_LABEL, "Back", -1);
  g_signal_connect (self->back, "clicked", G_CALLBACK (on_back_clicked), self);
  gtk_widget_set_parent (self->back, widget);

  gtk_image_set_pixel_size (GTK_IMAGE (cross), 24);
  self->close = vibeos_pressable_new (cross);
  gtk_widget_add_css_class (self->close, "sw-viewer-back");
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (self->close), VIBEOS_RIPPLE_CIRCLE, 20);
  vibeos_pressable_set_fill (VIBEOS_PRESSABLE (self->close), &scrim, FALSE);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->close), GTK_ACCESSIBLE_PROPERTY_LABEL, "Close", -1);
  g_signal_connect (self->close, "clicked", G_CALLBACK (on_close_clicked), self);
  gtk_widget_set_parent (self->close, widget);

  /* before the pages' own drag: up or down is the viewer's, sideways theirs */
  self->drag = gtk_gesture_drag_new ();
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (self->drag), GTK_PHASE_CAPTURE);
  g_signal_connect (self->drag, "drag-begin", G_CALLBACK (drag_begin), self);
  g_signal_connect (self->drag, "drag-update", G_CALLBACK (drag_update), self);
  g_signal_connect (self->drag, "drag-end", G_CALLBACK (drag_end), self);
  g_signal_connect (self->drag, "cancel", G_CALLBACK (drag_cancel), self);
  gtk_widget_add_controller (widget, GTK_EVENT_CONTROLLER (self->drag));

  /* two fingers: the zoom, before the pages and the drag (claiming the touches cancels both) */
  self->pinch = gtk_gesture_zoom_new ();
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (self->pinch), GTK_PHASE_CAPTURE);
  g_signal_connect (self->pinch, "begin", G_CALLBACK (pinch_begin), self);
  g_signal_connect (self->pinch, "scale-changed", G_CALLBACK (pinch_scale), self);
  g_signal_connect (self->pinch, "end", G_CALLBACK (pinch_end), self);
  gtk_widget_add_controller (widget, GTK_EVENT_CONTROLLER (self->pinch));

  /* taps anywhere but on the buttons: one shows or hides them, two zoom; it claims nothing, a tap still reaches what
   * is under it */
  self->taps = gtk_gesture_click_new ();
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (self->taps), GTK_PHASE_CAPTURE);
  g_signal_connect (self->taps, "pressed", G_CALLBACK (taps_pressed), self);
  g_signal_connect (self->taps, "released", G_CALLBACK (taps_released), self);
  gtk_widget_add_controller (widget, GTK_EVENT_CONTROLLER (self->taps));

  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key), self);
  gtk_widget_add_controller (widget, keys);
}

void
sw_viewer_present (GtkWidget *thumb, JsonArray *shots, guint index, GPtrArray *thumbs)
{
  GtkOverlay *overlay = vibeos_window_get_overlay (thumb);
  GtkRoot *root = gtk_widget_get_root (thumb);
  SwViewer *self;
  int w, h;

  g_return_if_fail (overlay != NULL);
  self = g_object_new (SW_TYPE_VIEWER, NULL);
  w = gtk_widget_get_width (GTK_WIDGET (overlay));
  h = gtk_widget_get_height (GTK_WIDGET (overlay));
  self->page_w = w;
  self->page_h = h;
  self->index = index;
  for (guint i = 0; i < json_array_get_length (shots); i++)
    {
      JsonObject *shot = sw_json_array_object (shots, i);
      double sw = sw_json_double (shot, "width"), sh = sw_json_double (shot, "height");
      double aspect = sw > 0 && sh > 0 ? sw / sh : 0.5;
      GWeakRef *ref = g_new0 (GWeakRef, 1);

      g_array_append_val (self->aspects, aspect);
      gtk_box_append (GTK_BOX (self->pages), sw_viewer_page_new (shot, w, h));
      g_weak_ref_init (ref, thumbs && i < thumbs->len ? g_ptr_array_index (thumbs, i) : NULL);
      g_ptr_array_add (self->thumbs, ref);
    }
  gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_LABEL, "Screenshots", -1);

  /* from the thumbnail, where it is now */
  self->has_thumb = gtk_widget_compute_bounds (thumb, GTK_WIDGET (overlay), &self->thumb);
  if (!self->has_thumb)
    self->thumb = GRAPHENE_RECT_INIT (w * 0.1, h * 0.1, w * 0.8, h * 0.8);
  g_set_weak_pointer (&self->below, gtk_overlay_get_child (overlay));
  g_set_weak_pointer (&self->focus_before, root ? gtk_root_get_focus (root) : NULL);
  gtk_overlay_add_overlay (overlay, GTK_WIDGET (self));
  gtk_overlay_set_measure_overlay (overlay, GTK_WIDGET (self), FALSE);
  gtk_overlay_set_clip_overlay (overlay, GTK_WIDGET (self), TRUE);
  gtk_widget_grab_focus (GTK_WIDGET (self));
  self->full = GRAPHENE_RECT_INIT (0, 0, 0, 0);
  {
    /* to the picture's place on the screen: fitted the way the page will be, before it is laid out */
    double a = g_array_index (self->aspects, double, MIN (index, n_pages (self) - 1));
    double fw = (double) w / h > a ? h * a : w, fh = (double) w / h > a ? h : w / a;

    self->full = GRAPHENE_RECT_INIT ((w - fw) / 2, (h - fh) / 2, fw, fh);
  }
  hide_thumb (self, self->has_thumb ? thumb : NULL);
  self->open = 0;
  vibeos_anim_start (self->open_anim, 0, 1, OPEN_MS, 0, SPONGY_FAST_OUT_SLOW_IN);
}
