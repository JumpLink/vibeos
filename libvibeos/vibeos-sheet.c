/* VibeosSheet: see vibeos-sheet.h.
 *
 * The sheet is one widget over the whole window. It draws the dim, then the sheet's fill, then its pages, which are
 * its children. Two of libspongy's springs move it, the closed form Android's SpringForce uses and libadwaita's
 * springs use too: the rise, 0 (under the window's bottom) to 1 (in place), and the slide of the page moving in or
 * out, 0 (in place) to 1 (past the sheet's right edge). Both run from one tick callback on the frame clock.
 *
 * The pages are laid out where they rest; the rise only moves the picture (a frame's translation in the snapshot),
 * so a frame of it lays nothing out. The slide moves the page it slides, as allocated. */
#include "vibeos-sheet.h"
#include "vibeos-pressable.h"
#include "vibeos-style.h"
#include "vibeos-top-bar.h"

#include <math.h>
#include <spongy.h>

#define GAP              30.0    /* libadwaita's space over a sheet as tall as it can be: 30 px on a window up to */
#define GAP_TALL         120.0   /* 720 px tall, growing with the window's height to 120 px at 1440 */
#define GAP_FROM_H       720.0
#define GAP_TO_H         1440.0
#define MAX_WIDTH        640     /* on a wider window the sheet is centred, this wide */
#define CORNER           28      /* the top corners, as large as a dialog's */
#define RISE_STIFFNESS   400.0   /* libadwaita's bottom sheet spring */
#define RISE_DAMPING     0.8
#define SLIDE_STIFFNESS  1000.0  /* libadwaita's navigation spring */
#define SLIDE_DAMPING    1.0
#define MIN_VISIBLE      0.001   /* of the sheet's height or width: the springs stop this close */
/* libadwaita's dialog (AdwDialog): a bottom sheet on a window up to 450 px wide or 360 px tall, a card floating in the
 * middle of a larger one (a tablet's), as wide as its content (360 px, AdwAboutDialog's width, unless the sheet says
 * otherwise) and as tall as it needs, at least 30 px in from the window's sides and 20 px from its top and bottom,
 * more on a larger window (120 px at 1440). It opens growing from 80 % of its size and fading in, on a spring that
 * goes a little past (damping 0.62, stiffness 500), and closes back, without going past */
#define SHEET_MAX_W      450
#define SHEET_MAX_H      360
#define FLOAT_WIDTH      360
#define FLOAT_SIDE       30.0
#define FLOAT_SIDE_BIG   120.0
#define FLOAT_ENDS       20.0
#define FLOAT_ENDS_BIG   120.0
#define FLOAT_FROM       720.0
#define FLOAT_TO         1440.0
#define FLOAT_MIN_SCALE  0.8
#define FLOAT_STIFFNESS  500.0
#define FLOAT_DAMPING    0.62
/* The dim over the window: libadwaita's for a dark sheet, deep enough to set a dark sheet apart from a dark window;
 * Material's scrim for a light one, which also has to show over the Weather's dark sky */
#define DIM_DARK         0.5
#define DIM_LIGHT        0.32
#define PAGE_DIM         0.2     /* over the page a pushed page covers, as it covers it */
#define CLOSE_RADIUS     16      /* the close button's circle, smaller than an icon button's (20), with a smaller */
#define CLOSE_ICON       20      /* X: as light as libadwaita's close button on its sheets, in the same place */

struct _VibeosSheet {
  GtkWidget     parent;
  GPtrArray    *pages;          /* GtkWidget: the pages, first to top; children of the sheet */
  GtkWidget    *leaving;        /* the page sliding out after back, until it is out */
  GtkWidget    *focus_before;   /* weak: what had the window's focus before the sheet came */
  graphene_rect_t rest;         /* where the sheet is in place, as allocated last */
  SpongySpring  rise;           /* the sheet: 0 under the window's bottom .. 1 in place */
  SpongySpring  slide;          /* the page sliding: 0 in place .. 1 past the right edge */
  double        rise_to, slide_to;
  gboolean      rise_start, slide_start;   /* a run to start on the next frame */
  guint         tick;
  gboolean      full_height;
  gboolean      closing;
  gboolean      floating;       /* a card in the middle of the window, not a bottom sheet */
  int           content_width;  /* the card's width; 0: FLOAT_WIDTH */
};

G_DEFINE_FINAL_TYPE (VibeosSheet, vibeos_sheet, GTK_TYPE_WIDGET)

enum { CLOSED, N_SIGNALS };
static guint signals[N_SIGNALS];

static GtkWidget *
page_at (VibeosSheet *self, int from_top)
{
  int i = (int) self->pages->len - 1 - from_top;

  return i >= 0 ? g_ptr_array_index (self->pages, i) : NULL;
}

/* the page that slides now: one going out, or the top one coming in; NULL when none moves */
static GtkWidget *
sliding_page (VibeosSheet *self)
{
  if (self->leaving)
    return self->leaving;
  if (self->pages->len > 1 && (self->slide.value > 0 || self->slide_start))
    return page_at (self, 0);
  return NULL;
}

/* the page under the sliding one, which shows to its left */
static GtkWidget *
page_under_slide (VibeosSheet *self)
{
  return page_at (self, self->leaving ? 0 : 1);
}

/* The top page takes the touches; the page under a sliding one shows, the rest are put away */
static void
sync_pages (VibeosSheet *self)
{
  GtkWidget *sliding = sliding_page (self), *under = sliding ? page_under_slide (self) : NULL;

  for (guint i = 0; i < self->pages->len; i++)
    {
      GtkWidget *page = g_ptr_array_index (self->pages, i);
      gboolean top = i + 1 == self->pages->len;

      gtk_widget_set_child_visible (page, top || page == under);
      gtk_widget_set_can_target (page, top);
    }
  if (self->leaving)
    gtk_widget_set_can_target (self->leaving, FALSE);
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

/* ---- layout and drawing ---------------------------------------------------------------------------------------- */

/* a card in the middle, or a bottom sheet: AdwDialog's breakpoint */
static gboolean
floats_in (int width, int height)
{
  return width > SHEET_MAX_W && height > SHEET_MAX_H;
}

static double
float_margin (double from, double to, int size)
{
  return from + (to - from) * MAX (0, (size - FLOAT_FROM) / (FLOAT_TO - FLOAT_FROM));
}

/* the floating card, in the middle */
static graphene_rect_t
float_rect (VibeosSheet *self, int width, int height)
{
  double side = float_margin (FLOAT_SIDE, FLOAT_SIDE_BIG, width), ends = float_margin (FLOAT_ENDS, FLOAT_ENDS_BIG, height);
  int w = MIN (self->content_width > 0 ? self->content_width : FLOAT_WIDTH, width - (int) round (2 * side));
  int max_h = MAX (height - (int) round (2 * ends), 0), h = max_h;

  w = MAX (w, 0);
  if (!self->full_height && self->pages->len > 0)
    {
      int min, nat;

      gtk_widget_measure (g_ptr_array_index (self->pages, 0), GTK_ORIENTATION_VERTICAL, w, &min, &nat, NULL, NULL);
      h = MIN (MAX (min, nat), max_h);
    }
  return GRAPHENE_RECT_INIT (round ((width - w) / 2.0), round ((height - h) / 2.0), w, h);
}

/* where the sheet is in place, in a window `width` x `height` */
static graphene_rect_t
rest_rect (VibeosSheet *self, int width, int height)
{
  if (self->floating)
    return float_rect (self, width, height);

  double gap = GAP + (GAP_TALL - GAP) * MAX (0, (height - GAP_FROM_H) / (GAP_TO_H - GAP_FROM_H));
  int w = MIN (width, MAX_WIDTH), max_h = MAX (height - (int) round (gap), 0), h = max_h;

  /* as tall as the first page needs: the pages pushed over it scroll in the same height, as libadwaita's do */
  if (!self->full_height && self->pages->len > 0)
    {
      int min, nat;

      gtk_widget_measure (g_ptr_array_index (self->pages, 0), GTK_ORIENTATION_VERTICAL, w, &min, &nat, NULL, NULL);
      h = MIN (MAX (min, nat), max_h);
    }
  return GRAPHENE_RECT_INIT (floor ((width - w) / 2.0), height - h, w, h);
}

static void
vibeos_sheet_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                      int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 0;     /* it takes what the overlay gives it: the whole window */
}

static void
place (VibeosSheet *self, GtkWidget *page, gboolean sliding)
{
  const graphene_rect_t *r = &self->rest;
  double x = r->origin.x + (sliding ? round (self->slide.value * r->size.width) : 0);

  if (gtk_widget_get_child_visible (page))
    gtk_widget_allocate (page, (int) r->size.width, (int) r->size.height, -1,
                         gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (x, r->origin.y)));
}

static void
vibeos_sheet_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  VibeosSheet *self = VIBEOS_SHEET (widget);
  GtkWidget *sliding = sliding_page (self);

  self->floating = floats_in (width, height);
  self->rest = rest_rect (self, width, height);
  for (guint i = 0; i < self->pages->len; i++)
    place (self, g_ptr_array_index (self->pages, i), g_ptr_array_index (self->pages, i) == sliding);
  if (self->leaving)
    place (self, self->leaving, TRUE);
}

/* the sheet's fill in `shape`, and its pages */
static void
snapshot_sheet (VibeosSheet *self, GtkSnapshot *snapshot, const GskRoundedRect *shape)
{
  GtkWidget *widget = GTK_WIDGET (self);
  const graphene_rect_t *r = &self->rest;
  GdkRGBA fill = vibeos_color (VIBEOS_SURFACE_CONTAINER);
  GtkWidget *sliding = sliding_page (self);

  gtk_snapshot_push_rounded_clip (snapshot, shape);
  gtk_snapshot_append_color (snapshot, &fill, &shape->bounds);
  if (sliding)
    {
      /* as libadwaita's navigation does: the page under shows only left of the sliding page's edge, dimmed the
       * more the more it is covered; the sliding page has nothing under it but the sheet's fill */
      double edge = r->origin.x + round (self->slide.value * r->size.width);
      GtkWidget *under = page_under_slide (self);
      graphene_rect_t left = GRAPHENE_RECT_INIT (r->origin.x, r->origin.y, edge - r->origin.x, r->size.height);

      if (under && left.size.width > 0)
        {
          GdkRGBA shade = vibeos_color_alpha (VIBEOS_SCRIM, PAGE_DIM * CLAMP (1 - self->slide.value, 0, 1));

          gtk_snapshot_push_clip (snapshot, &left);
          gtk_widget_snapshot_child (widget, under, snapshot);
          gtk_snapshot_append_color (snapshot, &shade, &left);
          gtk_snapshot_pop (snapshot);
        }
      gtk_snapshot_push_clip (snapshot, &GRAPHENE_RECT_INIT (edge, r->origin.y, r->size.width, r->size.height));
      gtk_widget_snapshot_child (widget, sliding, snapshot);
      gtk_snapshot_pop (snapshot);
    }
  else if (self->pages->len > 0)
    gtk_widget_snapshot_child (widget, page_at (self, 0), snapshot);
  gtk_snapshot_pop (snapshot);
}

static void
vibeos_sheet_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  VibeosSheet *self = VIBEOS_SHEET (widget);
  const graphene_rect_t *r = &self->rest;
  double width = gtk_widget_get_width (widget), height = gtk_widget_get_height (widget);
  double shown = self->rise.value, below = (1 - shown) * r->size.height;
  gboolean dark = vibeos_theme_get_dark (vibeos_theme_get_default ());
  GdkRGBA dim = vibeos_color_alpha (VIBEOS_SCRIM, (dark ? DIM_DARK : DIM_LIGHT) * CLAMP (shown, 0, 1));
  graphene_size_t corner = GRAPHENE_SIZE_INIT (CORNER, CORNER), square = GRAPHENE_SIZE_INIT (0, 0);
  GskRoundedRect shape;

  gtk_snapshot_append_color (snapshot, &dim, &GRAPHENE_RECT_INIT (0, 0, width, height));
  gtk_snapshot_save (snapshot);
  if (self->floating)
    {
      /* the card grows from 80 % about the window's middle as it fades in, as libadwaita's floating dialog */
      double scale = FLOAT_MIN_SCALE + (1 - FLOAT_MIN_SCALE) * shown;

      gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (width / 2, height / 2));
      gtk_snapshot_scale (snapshot, scale, scale);
      gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (-width / 2, -height / 2));
      gtk_snapshot_push_opacity (snapshot, CLAMP (shown, 0, 1));
      gsk_rounded_rect_init_from_rect (&shape, (graphene_rect_t *) r, CORNER);
      /* elevation level 3, as a Material dialog: a soft shadow round the card */
      gtk_snapshot_append_outset_shadow (snapshot, &shape, (GdkRGBA[]) { vibeos_color_alpha (VIBEOS_SHADOW, 0.3f) },
                                         0, 4, 0, 12);
      snapshot_sheet (self, snapshot, &shape);
      gtk_snapshot_pop (snapshot);
    }
  else
    {
      gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (0, below));
      /* the fill reaches down past the window's bottom as far as the sheet has risen past its place */
      gsk_rounded_rect_init (&shape, &GRAPHENE_RECT_INIT (r->origin.x, r->origin.y, r->size.width,
                                                          r->size.height + MAX (0, -below) + 1),
                             &corner, &corner, &square, &square);
      snapshot_sheet (self, snapshot, &shape);
    }
  gtk_snapshot_restore (snapshot);
}

/* ---- the motion ------------------------------------------------------------------------------------------------ */

/* out of the overlay once down: from an idle, since leaving it can drop the last reference, and with it the tick
 * callback running */
static gboolean
leave_overlay (gpointer data)
{
  VibeosSheet *self = data;
  GtkWidget *parent = gtk_widget_get_parent (GTK_WIDGET (self));
  GtkWidget *focus = self->focus_before;

  if (focus)
    g_object_ref (focus);
  if (GTK_IS_OVERLAY (parent))
    gtk_overlay_remove_overlay (GTK_OVERLAY (parent), GTK_WIDGET (self));
  /* the focus back where it was: on a phone nowhere, with a keyboard the button that opened the sheet */
  if (focus && gtk_widget_get_root (focus))
    gtk_widget_grab_focus (focus);
  g_clear_object (&focus);
  return G_SOURCE_REMOVE;
}

/* a slide done: a page that went out is gone, the one a page came in over is put away */
static void
slide_done (VibeosSheet *self)
{
  g_clear_pointer (&self->leaving, gtk_widget_unparent);
  sync_pages (self);
}

static gboolean
on_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  VibeosSheet *self = VIBEOS_SHEET (widget);
  double now = gdk_frame_clock_get_frame_time (clock) / 1000.0;
  gboolean moving = FALSE, was_sliding;

  /* each run starts on the frame after it was asked for, from where the spring is, at its speed */
  if (self->rise_start)
    spongy_spring_animate (&self->rise, self->rise.value, self->rise.velocity, self->rise_to, now);
  if (self->slide_start)
    spongy_spring_animate (&self->slide, self->slide.value, self->slide.velocity, self->slide_to, now);
  self->rise_start = self->slide_start = FALSE;
  was_sliding = self->slide.running;

  if (self->rise.running)
    moving |= spongy_spring_update (&self->rise, now);
  if (self->slide.running)
    {
      moving |= spongy_spring_update (&self->slide, now);
      gtk_widget_queue_allocate (widget);
    }
  gtk_widget_queue_draw (widget);

  /* down: gone, at once (it would only settle under the window from there) */
  if (self->closing && self->rise.value <= 0)
    {
      self->tick = 0;
      g_idle_add_full (G_PRIORITY_DEFAULT, leave_overlay, g_object_ref (self), g_object_unref);
      return G_SOURCE_REMOVE;
    }
  if (was_sliding && !self->slide.running)
    slide_done (self);
  if (!moving)
    {
      self->tick = 0;
      return G_SOURCE_REMOVE;
    }
  return G_SOURCE_CONTINUE;
}

/* starts what was asked for on the next frame; a sheet not on screen (no frames) is put where it goes at once */
static void
run (VibeosSheet *self)
{
  GtkWidget *widget = GTK_WIDGET (self);

  if (gtk_widget_get_mapped (widget))
    {
      if (!self->tick)
        self->tick = gtk_widget_add_tick_callback (widget, on_tick, NULL, NULL);
      gtk_widget_queue_draw (widget);
      return;
    }
  if (self->rise_start)
    self->rise.value = self->rise_to;
  if (self->slide_start)
    {
      self->slide.value = self->slide_to;
      self->slide_start = FALSE;
      slide_done (self);
    }
  self->rise.running = self->slide.running = FALSE;
  self->rise_start = FALSE;
  if (self->closing)
    g_idle_add_full (G_PRIORITY_DEFAULT, leave_overlay, g_object_ref (self), g_object_unref);
}

static void
rise_to (VibeosSheet *self, double to)
{
  self->rise_to = to;
  self->rise_start = TRUE;
  run (self);
}

static void
slide_to (VibeosSheet *self, double to)
{
  self->slide_to = to;
  self->slide_start = TRUE;
  sync_pages (self);
  run (self);
}

/* ---- the pages ------------------------------------------------------------------------------------------------- */

static void
on_bar_back (VibeosTopBar *bar, VibeosSheet *self)
{
  vibeos_sheet_back (self);
}

static void
on_close_clicked (VibeosPressable *button, VibeosSheet *self)
{
  vibeos_sheet_close (self);
}

/* a page: its bar (the back arrow when `back`, the title, the close button), then its content */
static GtkWidget *
page_new (VibeosSheet *self, const char *title, gboolean back, GtkWidget *content)
{
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *bar = vibeos_top_bar_new (title);
  GtkWidget *close;

  gtk_widget_add_css_class (page, "vibeos-sheet-page");
  vibeos_top_bar_set_compact (VIBEOS_TOP_BAR (bar), TRUE);
  vibeos_top_bar_set_back (VIBEOS_TOP_BAR (bar), back);
  g_signal_connect (bar, "back", G_CALLBACK (on_bar_back), self);
  /* the X in a tonal circle; its touch target is still the bar's usual 48 px */
  close = vibeos_top_bar_add_action (VIBEOS_TOP_BAR (bar), "window-close-symbolic");
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (close), VIBEOS_RIPPLE_CIRCLE, CLOSE_RADIUS);
  gtk_image_set_pixel_size (GTK_IMAGE (vibeos_pressable_get_child (VIBEOS_PRESSABLE (close))), CLOSE_ICON);
  /* the theme's colour as it is when it draws: taken once, a sheet open while an app turned dark kept the light
   * circle (and a light one, the dark circle) under an X in the new theme's colour */
  vibeos_pressable_set_fill_role (VIBEOS_PRESSABLE (close), VIBEOS_SURFACE_CONTAINER_HIGHEST, FALSE);
  gtk_accessible_update_property (GTK_ACCESSIBLE (close), GTK_ACCESSIBLE_PROPERTY_LABEL, "Close", -1);
  g_signal_connect (close, "clicked", G_CALLBACK (on_close_clicked), self);
  gtk_box_append (GTK_BOX (page), bar);
  if (content)
    {
      gtk_widget_set_vexpand (content, TRUE);
      gtk_box_append (GTK_BOX (page), content);
    }
  gtk_widget_set_parent (page, GTK_WIDGET (self));
  return page;
}

void
vibeos_sheet_set_content (VibeosSheet *self, GtkWidget *content)
{
  GtkWidget *page, *old;

  g_return_if_fail (VIBEOS_IS_SHEET (self));
  /* the first page's children: its bar, then its content */
  page = g_ptr_array_index (self->pages, 0);
  old = gtk_widget_get_next_sibling (gtk_widget_get_first_child (page));
  if (old)
    gtk_box_remove (GTK_BOX (page), old);
  if (content)
    {
      gtk_widget_set_vexpand (content, TRUE);
      gtk_box_append (GTK_BOX (page), content);
    }
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

void
vibeos_sheet_set_full_height (VibeosSheet *self, gboolean full_height)
{
  g_return_if_fail (VIBEOS_IS_SHEET (self));
  self->full_height = !!full_height;
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

void
vibeos_sheet_set_content_width (VibeosSheet *self, int width)
{
  g_return_if_fail (VIBEOS_IS_SHEET (self));
  self->content_width = width;
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

void
vibeos_sheet_push (VibeosSheet *self, const char *title, GtkWidget *content)
{
  g_return_if_fail (VIBEOS_IS_SHEET (self));
  if (self->closing)
    return;
  /* a page still sliding out is out */
  if (self->leaving)
    {
      self->slide.running = FALSE;
      g_clear_pointer (&self->leaving, gtk_widget_unparent);
    }
  g_ptr_array_add (self->pages, page_new (self, title, TRUE, content));
  /* it comes in from past the right edge */
  self->slide.value = 1;
  self->slide.velocity = 0;
  slide_to (self, 0);
  /* the keys stay the sheet's (Escape and Alt+Left go back), not the covered page's */
  gtk_widget_grab_focus (GTK_WIDGET (self));
}

gboolean
vibeos_sheet_back (VibeosSheet *self)
{
  g_return_val_if_fail (VIBEOS_IS_SHEET (self), FALSE);
  if (self->closing || self->pages->len < 2)
    return FALSE;
  if (self->leaving)
    {
      /* a second back while one page slides out: that one is out, the next goes from where it is */
      g_clear_pointer (&self->leaving, gtk_widget_unparent);
      self->slide.value = 0;
      self->slide.velocity = 0;
    }
  /* from where it is: a page still sliding in turns round at its speed */
  self->leaving = g_ptr_array_steal_index (self->pages, self->pages->len - 1);
  slide_to (self, 1);
  gtk_widget_grab_focus (GTK_WIDGET (self));
  return TRUE;
}

void
vibeos_sheet_present (VibeosSheet *self, GtkWidget *parent)
{
  GtkOverlay *overlay = vibeos_window_get_overlay (parent);
  GtkRoot *root = gtk_widget_get_root (parent);

  g_return_if_fail (VIBEOS_IS_SHEET (self));
  g_return_if_fail (overlay != NULL);
  g_set_weak_pointer (&self->focus_before, root ? gtk_root_get_focus (root) : NULL);
  gtk_overlay_add_overlay (overlay, GTK_WIDGET (self));
  /* the dim covers the whole window and takes every touch outside the sheet */
  gtk_overlay_set_measure_overlay (overlay, GTK_WIDGET (self), FALSE);
  gtk_overlay_set_clip_overlay (overlay, GTK_WIDGET (self), TRUE);
  /* the keys come to it, so that Escape and Alt+Left are its own and do not close the window */
  gtk_widget_grab_focus (GTK_WIDGET (self));
  self->floating = floats_in (gtk_widget_get_width (GTK_WIDGET (overlay)), gtk_widget_get_height (GTK_WIDGET (overlay)));
  self->rise.stiffness = self->floating ? FLOAT_STIFFNESS : RISE_STIFFNESS;
  self->rise.damping = self->floating ? FLOAT_DAMPING : RISE_DAMPING;
  self->rise.value = 0;
  self->rise.velocity = 0;
  rise_to (self, 1);
}

void
vibeos_sheet_close (VibeosSheet *self)
{
  g_return_if_fail (VIBEOS_IS_SHEET (self));
  if (self->closing)
    return;
  self->closing = TRUE;
  g_object_ref (self);
  g_signal_emit (self, signals[CLOSED], 0);
  /* touches go to the window again as it goes down; it stops at the bottom rather than bounce under it */
  gtk_widget_set_can_target (GTK_WIDGET (self), FALSE);
  self->rise.min = 0;
  rise_to (self, 0);
  g_object_unref (self);
}

/* ---- input ----------------------------------------------------------------------------------------------------- */

static void
on_pressed (GtkGestureClick *click, int n_press, double x, double y, VibeosSheet *self)
{
  graphene_rect_t shown = self->rest;

  if (!self->floating)
    shown.origin.y += (1 - self->rise.value) * self->rest.size.height;
  if (graphene_rect_contains_point (&shown, &GRAPHENE_POINT_INIT (x, y)))
    return;
  /* a touch on the dim closes the sheet, and does nothing else */
  gtk_gesture_set_state (GTK_GESTURE (click), GTK_EVENT_SEQUENCE_CLAIMED);
  vibeos_sheet_close (self);
}

static gboolean
on_key (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, VibeosSheet *self)
{
  gboolean alt_left = (keyval == GDK_KEY_Left || keyval == GDK_KEY_KP_Left) && (state & GDK_ALT_MASK);

  if (keyval != GDK_KEY_Escape && keyval != GDK_KEY_Back && !alt_left)
    return FALSE;
  if (!vibeos_sheet_back (self))
    vibeos_sheet_close (self);
  return TRUE;
}

/* ---- GObject --------------------------------------------------------------------------------------------------- */

static void
vibeos_sheet_dispose (GObject *object)
{
  VibeosSheet *self = VIBEOS_SHEET (object);

  if (self->tick)
    gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->tick);
  self->tick = 0;
  g_clear_weak_pointer (&self->focus_before);
  g_clear_pointer (&self->leaving, gtk_widget_unparent);
  if (self->pages)
    {
      for (guint i = 0; i < self->pages->len; i++)
        gtk_widget_unparent (g_ptr_array_index (self->pages, i));
      g_clear_pointer (&self->pages, g_ptr_array_unref);
    }
  G_OBJECT_CLASS (vibeos_sheet_parent_class)->dispose (object);
}

static void
vibeos_sheet_class_init (VibeosSheetClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = vibeos_sheet_dispose;
  widget_class->measure = vibeos_sheet_measure;
  widget_class->size_allocate = vibeos_sheet_size_allocate;
  widget_class->snapshot = vibeos_sheet_snapshot;
  gtk_widget_class_set_css_name (widget_class, "vibeos-sheet");
  /* a dialog to the shell's back gesture too (a11y.js), which then sends Escape rather than leave the app */
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_DIALOG);

  signals[CLOSED] = g_signal_new ("closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                  NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
vibeos_sheet_init (VibeosSheet *self)
{
  GtkWidget *widget = GTK_WIDGET (self);
  GtkGesture *click = gtk_gesture_click_new ();
  GtkEventController *keys = gtk_event_controller_key_new ();

  self->pages = g_ptr_array_new ();
  spongy_spring_init (&self->rise, RISE_STIFFNESS, RISE_DAMPING, MIN_VISIBLE);
  spongy_spring_init (&self->slide, SLIDE_STIFFNESS, SLIDE_DAMPING, MIN_VISIBLE);
  gtk_widget_set_focusable (widget, TRUE);
  g_signal_connect (click, "pressed", G_CALLBACK (on_pressed), self);
  gtk_widget_add_controller (widget, GTK_EVENT_CONTROLLER (click));
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key), self);
  gtk_widget_add_controller (widget, keys);
}

GtkWidget *
vibeos_sheet_new (const char *title)
{
  VibeosSheet *self = g_object_new (VIBEOS_TYPE_SHEET, NULL);

  g_ptr_array_add (self->pages, page_new (self, title, FALSE, NULL));
  if (title)
    gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
  return GTK_WIDGET (self);
}
