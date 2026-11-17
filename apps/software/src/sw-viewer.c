/* SwViewer: see sw-viewer.h.
 *
 * Over the whole window, in its root overlay: a black backdrop, the pictures side by side in a horizontal
 * SpongyScrollView that pages one picture at a time, a dot per picture under them and a back arrow.
 *
 * Its motion, all drawn as transforms of what is laid out once (nothing is laid out again while it moves):
 *
 *   opening    the picture grows from the thumbnail tapped to the screen, its corners straightening, while the
 *              backdrop darkens: 300 ms
 *   dragging   up or down, the picture follows the finger and shrinks a little, and the backdrop fades with the
 *              distance; let go past an eighth of the screen, or flung, it closes, otherwise it springs back
 *   closing    from wherever the picture is, it shrinks into its thumbnail (the strip scrolled first to show it, if
 *              it was paged away from) while the backdrop fades: 250 ms. A thumbnail that is not on the screen any
 *              more: the picture just fades, shrinking a little.
 *
 * While it is open and still, the window under it is not drawn at all (it is hidden by the backdrop anyway): only the
 * picture is, frame after frame, while it pages. */
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

struct _SwViewer {
  GtkWidget       parent_instance;
  GtkWidget      *pager, *pages, *back;
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

  /* the dots and the arrow come and go with the backdrop */
  if (shown > 0.01)
    {
      guint n = n_pages (self);
      double offset = spongy_scroll_view_get_offset (SPONGY_SCROLL_VIEW (self->pager)), at = offset / w;
      double row = n * DOT + (n - 1) * DOT_GAP, x0 = (w - row) / 2, y = h - 24 - DOT;

      gtk_snapshot_push_opacity (snapshot, clamp01 (shown));
      for (guint i = 0; n > 1 && i < n; i++)
        {
          /* the dot of the page showing lights up, sliding over as the pages slide */
          double lit = clamp01 (1 - fabs (at - i));
          GdkRGBA dot = { 1, 1, 1, 0.4 + 0.6 * lit };
          GskRoundedRect circle;

          gsk_rounded_rect_init_from_rect (&circle, &GRAPHENE_RECT_INIT (x0 + i * (DOT + DOT_GAP), y, DOT, DOT),
                                           DOT / 2.0);
          gtk_snapshot_push_rounded_clip (snapshot, &circle);
          gtk_snapshot_append_color (snapshot, &dot, &circle.bounds);
          gtk_snapshot_pop (snapshot);
        }
      gtk_widget_snapshot_child (widget, self->back, snapshot);
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
    sw_image_set_size (SW_IMAGE (page), self->page_w, self->page_h);
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
  /* the screen turned: pages the new size (laid out on the next frame; the pager keeps its page) */
  if ((width != self->page_w || height != self->page_h) && width > 0 && height > 0)
    {
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
  cover_below (self, FALSE);
  /* from where the picture is now: on its page (between two pages: the nearer), where a drag left it */
  spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (self->pager), page * w);
  f = fitted (self, page);
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

static void
drag_begin (GtkGestureDrag *drag, double x, double y, SwViewer *self)
{
  if (self->closing || self->open < 1)
    {
      gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_DENIED);
      return;
    }
  self->claimed = FALSE;
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
  vibeos_anim_start (self->back_anim, self->dy, 0, BACK_MS, 0, SPONGY_FAST_OUT_SLOW_IN);
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
  g_clear_pointer (&self->open_anim, vibeos_anim_free);
  g_clear_pointer (&self->back_anim, vibeos_anim_free);
  g_clear_pointer (&self->pager, gtk_widget_unparent);
  g_clear_pointer (&self->back, gtk_widget_unparent);
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

  self->thumbs = g_ptr_array_new_with_free_func (weak_ref_free);
  self->aspects = g_array_new (FALSE, FALSE, sizeof (double));
  self->open_anim = vibeos_anim_new (widget, on_open_anim, self);
  self->back_anim = vibeos_anim_new (widget, on_back_anim, self);
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
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->back), GTK_ACCESSIBLE_PROPERTY_LABEL, "Back", -1);
  g_signal_connect (self->back, "clicked", G_CALLBACK (on_back_clicked), self);
  gtk_widget_set_parent (self->back, widget);

  /* before the pages' own drag: up or down is the viewer's, sideways theirs */
  self->drag = gtk_gesture_drag_new ();
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (self->drag), GTK_PHASE_CAPTURE);
  g_signal_connect (self->drag, "drag-begin", G_CALLBACK (drag_begin), self);
  g_signal_connect (self->drag, "drag-update", G_CALLBACK (drag_update), self);
  g_signal_connect (self->drag, "drag-end", G_CALLBACK (drag_end), self);
  g_signal_connect (self->drag, "cancel", G_CALLBACK (drag_cancel), self);
  gtk_widget_add_controller (widget, GTK_EVENT_CONTROLLER (self->drag));

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
      GtkWidget *page = sw_image_new (w, h, 0);
      GWeakRef *ref = g_new0 (GWeakRef, 1);
      const char *caption = sw_json_string (shot, "caption");

      g_array_append_val (self->aspects, aspect);
      sw_image_set_contain (SW_IMAGE (page), TRUE);
      sw_image_set_url (SW_IMAGE (page), sw_json_string (shot, "url"));
      if (caption)
        gtk_accessible_update_property (GTK_ACCESSIBLE (page), GTK_ACCESSIBLE_PROPERTY_LABEL, caption, -1);
      gtk_box_append (GTK_BOX (self->pages), page);
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
