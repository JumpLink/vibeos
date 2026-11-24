/* nb-pages: the pages a tab opens over its browser page: History, Favorites,
 * Downloads (the browser's own list) and Desktop shortcuts. Each is a toolbar over a list, in a
 * page pushed on the tab's navigator; the up arrow (named "Back" for the shell's gesture) closes it. */
#include "nb.h"
#include "nb-widgets.h"

#include <math.h>
#include <vibeos.h>
#include <spongy-gtk.h>
#include <string.h>

/* behind a row being swiped away: the design's fixed red, the same in either mode and whatever the accent */
#define COLOR_DELETE           "#F44336"
#define DELETE_MARGIN          12          /* the delete icon, in from the row's end */

/* ==== the frame shared by every page ============================================================================= */

typedef struct {
  NbTab     *t;
  GtkWidget *page, *toolbar, *scroll, *list, *empty, *shadow;
} Frame;

static void
on_up (Frame *f)
{
  nb_tab_pop_page (f->t);
}

/* the toolbar lifts once the list has scrolled */
static void
on_scrolled (GtkAdjustment *adj, Frame *f)
{
  gboolean lifted = gtk_adjustment_get_value (adj) > 0.5;
  if (lifted)
    gtk_widget_add_css_class (f->toolbar, "lifted");
  else
    gtk_widget_remove_css_class (f->toolbar, "lifted");
  gtk_widget_set_visible (f->shadow, lifted);
}

/* a page: the toolbar, then `list` in a scroll view (with the scroll bar when `indicator`), and `empty` in its place
 * while there is nothing */
static Frame *
frame_new (NbTab *t, const char *title, const char *css, GtkWidget *actions, GtkWidget *list, GtkWidget *empty,
           gboolean indicator)
{
  Frame *f = g_new0 (Frame, 1);

  f->t = t;
  f->page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (f->page, "nb-page");
  gtk_widget_add_css_class (f->page, css);
  g_object_set_data_full (G_OBJECT (f->page), "nb-frame", f, g_free);
  f->toolbar = nb_toolbar_new (title, 0, G_CALLBACK (on_up), f, actions);
  gtk_box_append (GTK_BOX (f->page), f->toolbar);

  GtkWidget *body = gtk_overlay_new ();
  gtk_widget_set_vexpand (body, TRUE);
  f->list = list;
  f->scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (f->scroll), list);
  gtk_overlay_set_child (GTK_OVERLAY (body), f->scroll);
  GtkAdjustment *adj = spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (f->scroll));
  g_signal_connect (adj, "value-changed", G_CALLBACK (on_scrolled), f);
  if (indicator)
    gtk_overlay_add_overlay (GTK_OVERLAY (body), nb_indicator_new (adj));
  if (empty) {
    f->empty = empty;
    gtk_widget_set_visible (empty, FALSE);
    gtk_overlay_add_overlay (GTK_OVERLAY (body), empty);
  }
  /* the lifted toolbar's shadow falls on the list's top edge */
  f->shadow = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (f->shadow, "nb-toolbar-shadow");
  gtk_widget_set_valign (f->shadow, GTK_ALIGN_START);
  gtk_widget_set_can_target (f->shadow, FALSE);
  gtk_widget_set_visible (f->shadow, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (body), f->shadow);
  gtk_box_append (GTK_BOX (f->page), body);
  return f;
}

static void
frame_set_empty (Frame *f, gboolean empty)
{
  gtk_widget_set_visible (f->scroll, !empty);
  if (f->empty)
    gtk_widget_set_visible (f->empty, empty);
}

/* an empty page: the image over the text, centred */
static GtkWidget *
empty_layout (GtkWidget *image, const char *text)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *label = nb_label (text, 14, "nb-empty-text", FALSE);

  gtk_widget_add_css_class (box, "nb-empty");
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  gtk_widget_set_halign (image, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), image);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_justify (GTK_LABEL (label), GTK_JUSTIFY_CENTER);
  gtk_label_set_xalign (GTK_LABEL (label), 0.5);
  gtk_box_append (GTK_BOX (box), label);
  gtk_widget_set_can_target (box, FALSE);
  return box;
}

/* ==== History ===================================================================================================== */

typedef struct _History History;

/* a row the finger can swipe away to the left: red opens behind it with the delete icon at its end; let go past
 * half the width (or flung left faster than 120 px/s) it leaves, else it settles back; once gone the gap closes
 * over 250 ms, still red */

#define NB_TYPE_SWIPE_ROW (nb_swipe_row_get_type ())
G_DECLARE_FINAL_TYPE (NbSwipeRow, nb_swipe_row, NB, SWIPE_ROW, GtkWidget)

#define SWIPE_SLOP        8          /* px the finger moves before the row follows it */
#define SWIPE_ESCAPE      120        /* px/s: the slowest fling that takes the row away */
#define SWIPE_MS          250        /* the row going out or settling back */
#define MOVE_MS           250        /* the gap closing */

typedef enum { ANIM_NONE, ANIM_SETTLE, ANIM_OUT, ANIM_COLLAPSE } SwipeAnim;

struct _NbSwipeRow {
  GtkWidget  parent;
  GtkWidget *child;
  History   *history;
  char      *url, *title;
  double     time;
  double     dx, from_dx, shrink;   /* shrink: 1 = full height, 0 = closed */
  gboolean   dragging, gone;
  SwipeAnim  kind;
  VibeosAnim   *anim;
  double     start_x, last_x, vx;
  gint64     last_us;
};

G_DEFINE_FINAL_TYPE (NbSwipeRow, nb_swipe_row, GTK_TYPE_WIDGET)

struct _History {
  Frame     *f;
  NbTab     *t;
  GPtrArray *pending;        /* NbEntry copies not built into rows yet */
  guint      next, build_id;
  NbSwipeRow *last_deleted;
};

static void
nb_swipe_row_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                      int *minimum_baseline, int *natural_baseline)
{
  NbSwipeRow *self = NB_SWIPE_ROW (widget);

  gtk_widget_measure (self->child, orientation, for_size, minimum, natural, NULL, NULL);
  if (orientation == GTK_ORIENTATION_VERTICAL) {
    *minimum = (int) ceil (*minimum * self->shrink);
    *natural = (int) ceil (*natural * self->shrink);
  }
}

static void
nb_swipe_row_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  NbSwipeRow *self = NB_SWIPE_ROW (widget);
  int h;

  gtk_widget_measure (self->child, GTK_ORIENTATION_VERTICAL, width, NULL, &h, NULL, NULL);
  gtk_widget_allocate (self->child, width, MAX (h, height), -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (self->dx, 0)));
}

static void
nb_swipe_row_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  NbSwipeRow *self = NB_SWIPE_ROW (widget);
  double w = gtk_widget_get_width (widget), h = gtk_widget_get_height (widget);
  /* behind a row being swiped away: the fixed red, the delete icon in black on it */
  GdkRGBA red;

  gdk_rgba_parse (&red, COLOR_DELETE);
  if (self->gone) {
    gtk_snapshot_append_color (snapshot, &red, &GRAPHENE_RECT_INIT (0, 0, w, h));
    return;
  }
  if (self->dx < 0) {
    GdkRGBA icon = vibeos_palette_stop (VIBEOS_PALETTE_NEUTRAL1, 1000);
    gtk_snapshot_append_color (snapshot, &red, &GRAPHENE_RECT_INIT (w + self->dx, 0, -self->dx, h));
    vibeos_snapshot_icon (snapshot, widget, "jelly-delete-action-symbolic", w - DELETE_MARGIN - 24, (h - 24) / 2, 24,
                       &icon);
  }
  gtk_widget_snapshot_child (widget, self->child, snapshot);
}

static void history_row_deleted (History *h, NbSwipeRow *row);

static void
on_swipe_anim (VibeosAnim *anim, double value, gpointer data)
{
  NbSwipeRow *self = data;
  double w = gtk_widget_get_width (GTK_WIDGET (self));

  switch (self->kind) {
  case ANIM_SETTLE:
    self->dx = self->from_dx * (1 - value);
    break;
  case ANIM_OUT:
    self->dx = self->from_dx + (-w - self->from_dx) * value;
    break;
  case ANIM_COLLAPSE:
    self->shrink = 1 - value;
    break;
  case ANIM_NONE:
    return;
  }
  gtk_widget_queue_allocate (GTK_WIDGET (self));
  gtk_widget_queue_draw (GTK_WIDGET (self));
  if (vibeos_anim_is_running (anim))
    return;
  SwipeAnim done = self->kind;
  self->kind = ANIM_NONE;
  if (done == ANIM_OUT) {
    /* swiped away: the entry goes, the gap closes */
    self->gone = TRUE;
    history_row_deleted (self->history, self);
    self->kind = ANIM_COLLAPSE;
    vibeos_anim_start (self->anim, 0, 1, MOVE_MS, 0, SPONGY_ACCELERATE_DECELERATE);
  } else if (done == ANIM_COLLAPSE) {
    gtk_widget_set_visible (GTK_WIDGET (self), FALSE);
  }
}

static void
swipe_track (NbSwipeRow *self, double x)
{
  gint64 now = g_get_monotonic_time ();
  double dt = (now - self->last_us) / 1e6;

  if (self->last_us && dt > 0.001)
    self->vx = 0.6 * self->vx + 0.4 * (x - self->last_x) / dt;
  self->last_x = x;
  self->last_us = now;
}

static void
on_drag_begin (GtkGestureDrag *drag, double x, double y, NbSwipeRow *self)
{
  if (self->gone || self->kind == ANIM_OUT || self->kind == ANIM_COLLAPSE) {
    gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_DENIED);
    return;
  }
  vibeos_anim_stop (self->anim);
  self->kind = ANIM_NONE;
  self->start_x = self->dx;
  self->dragging = FALSE;
  self->vx = 0;
  self->last_us = 0;
  swipe_track (self, x);
}

static void
on_drag_update (GtkGestureDrag *drag, double ox, double oy, NbSwipeRow *self)
{
  double x, y;

  gtk_gesture_drag_get_start_point (drag, &x, &y);
  swipe_track (self, x + ox);
  if (!self->dragging) {
    /* a horizontal move past the slop takes the touch from the list's scrolling and the row's tap */
    if (fabs (ox) < SWIPE_SLOP || fabs (ox) < fabs (oy))
      return;
    self->dragging = TRUE;
    gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_CLAIMED);
  }
  /* only to the left */
  self->dx = MIN (0, self->start_x + ox);
  gtk_widget_queue_allocate (GTK_WIDGET (self));
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_drag_end (GtkGestureDrag *drag, double ox, double oy, NbSwipeRow *self)
{
  if (!self->dragging)
    return;
  self->dragging = FALSE;
  double w = gtk_widget_get_width (GTK_WIDGET (self));
  gboolean out = self->dx < -w / 2 || (self->vx <= -SWIPE_ESCAPE && self->dx < 0);
  self->from_dx = self->dx;
  self->kind = out ? ANIM_OUT : ANIM_SETTLE;
  vibeos_anim_start (self->anim, 0, 1, SWIPE_MS, 0, SPONGY_FAST_OUT_SLOW_IN);
}

static void
on_drag_cancel (GtkGesture *gesture, GdkEventSequence *seq, NbSwipeRow *self)
{
  if (!self->dragging)
    return;
  self->dragging = FALSE;
  self->from_dx = self->dx;
  self->kind = ANIM_SETTLE;
  vibeos_anim_start (self->anim, 0, 1, SWIPE_MS, 0, SPONGY_FAST_OUT_SLOW_IN);
}

static void
nb_swipe_row_dispose (GObject *object)
{
  NbSwipeRow *self = NB_SWIPE_ROW (object);

  g_clear_pointer (&self->anim, vibeos_anim_free);
  g_clear_pointer (&self->child, gtk_widget_unparent);
  G_OBJECT_CLASS (nb_swipe_row_parent_class)->dispose (object);
}

static void
nb_swipe_row_finalize (GObject *object)
{
  NbSwipeRow *self = NB_SWIPE_ROW (object);

  g_free (self->url);
  g_free (self->title);
  G_OBJECT_CLASS (nb_swipe_row_parent_class)->finalize (object);
}

static void
nb_swipe_row_class_init (NbSwipeRowClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = nb_swipe_row_dispose;
  G_OBJECT_CLASS (klass)->finalize = nb_swipe_row_finalize;
  widget_class->measure = nb_swipe_row_measure;
  widget_class->size_allocate = nb_swipe_row_size_allocate;
  widget_class->snapshot = nb_swipe_row_snapshot;
  gtk_widget_class_set_css_name (widget_class, "nb-swipe-row");
}

static void
nb_swipe_row_init (NbSwipeRow *self)
{
  self->shrink = 1;
  gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);
  self->anim = vibeos_anim_new (GTK_WIDGET (self), on_swipe_anim, self);
  GtkGesture *drag = gtk_gesture_drag_new ();
  gtk_gesture_single_set_touch_only (GTK_GESTURE_SINGLE (drag), FALSE);
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_drag_end), self);
  g_signal_connect (drag, "cancel", G_CALLBACK (on_drag_cancel), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));
}

/* "Saturday November 07 2026, 04:41" */
static char *
history_date (double time)
{
  g_autoptr (GDateTime) dt = g_date_time_new_from_unix_local ((gint64) time);
  return dt ? g_date_time_format (dt, "%A %B %d %Y, %H:%M") : g_strdup ("");
}

/* an empty title shows the URL's third '/' piece, the host */
static char *
history_title (const char *url, const char *title)
{
  if (title && *title)
    return g_strdup (title);
  g_auto (GStrv) parts = g_strsplit (url, "/", 4);
  return g_strdup (g_strv_length (parts) > 2 ? parts[2] : url);
}

static void
on_history_row (GtkWidget *pressable, NbSwipeRow *row)
{
  /* a row opens its page in a new tab */
  nb_open_tab (row->url, FALSE, NULL, NULL);
}

static GtkWidget *
history_row_new (History *h, const NbEntry *e)
{
  NbSwipeRow *row = g_object_new (NB_TYPE_SWIPE_ROW, NULL);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  g_autofree char *title = history_title (e->url, e->title);
  g_autofree char *date = history_date (e->time);

  row->history = h;
  row->url = g_strdup (e->url);
  row->title = g_strdup (e->title);
  row->time = e->time;
  gtk_widget_add_css_class (box, "nb-history-row");
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), nb_label (title, 18, "nb-history-title", TRUE));
  gtk_box_append (GTK_BOX (box), nb_label (date, 14, "nb-history-summary", TRUE));
  GtkWidget *pressable = vibeos_pressable_new (box);
  gtk_widget_add_css_class (pressable, "nb-history-pressable");
  g_signal_connect (pressable, "clicked", G_CALLBACK (on_history_row), row);
  row->child = pressable;
  gtk_widget_set_parent (pressable, GTK_WIDGET (row));
  return GTK_WIDGET (row);
}

static guint
visible_rows (History *h)
{
  guint n = 0;
  for (GtkWidget *c = gtk_widget_get_first_child (h->f->list); c; c = gtk_widget_get_next_sibling (c))
    n += gtk_widget_get_visible (c) && !NB_SWIPE_ROW (c)->gone;
  return n + (h->pending ? h->pending->len - h->next : 0);
}

/* the deleted entry the snackbar's Undo puts back. One at a time: a newer deletion (here or in another tab's
 * History) keeps the older for good, and an older snackbar's Undo then does nothing */
static struct {
  guint    serial;
  NbEntry *entry;
  guint    index;
  GWeakRef row;
} undo;

static void
on_undo (gpointer data)
{
  if (GPOINTER_TO_UINT (data) != undo.serial || !undo.entry)
    return;
  /* model.insert (entry): back where it was */
  nb_store_put_history (nb->store, MIN (undo.index, nb->store->history->len), g_steal_pointer (&undo.entry));
  g_autoptr (NbSwipeRow) row = g_weak_ref_get (&undo.row);
  if (!row || !row->history)
    return;
  vibeos_anim_stop (row->anim);
  row->kind = ANIM_NONE;
  row->gone = FALSE;
  row->dx = 0;
  row->shrink = 1;
  gtk_widget_set_visible (GTK_WIDGET (row), TRUE);
  gtk_widget_queue_resize (GTK_WIDGET (row));
  frame_set_empty (row->history->f, FALSE);
}

static void
history_row_deleted (History *h, NbSwipeRow *row)
{
  /* the entry goes, and the snackbar: "Entry deleted" with Undo, for a long time */
  for (guint i = 0; i < nb->store->history->len; i++) {
    NbEntry *e = nb->store->history->pdata[i];
    if (!g_strcmp0 (e->url, row->url)) {
      g_clear_pointer (&undo.entry, nb_entry_free);
      undo.entry = nb_store_take_history (nb->store, i);
      undo.index = i;
      g_weak_ref_set (&undo.row, row);
      undo.serial++;
      nb_tab_snackbar (h->t, "Entry deleted", "Undo", on_undo, GUINT_TO_POINTER (undo.serial));
      break;
    }
  }
  if (!visible_rows (h))
    frame_set_empty (h->f, TRUE);
}

static gboolean
build_rows (gpointer data)
{
  History *h = data;
  /* the first screenful at once, the rest in batches so a long history opens at once too */
  guint batch = h->next == 0 ? 30 : 100;

  for (guint n = 0; n < batch && h->next < h->pending->len; n++, h->next++)
    gtk_box_append (GTK_BOX (h->f->list), history_row_new (h, h->pending->pdata[h->next]));
  if (h->next < h->pending->len)
    return G_SOURCE_CONTINUE;
  h->build_id = 0;
  return G_SOURCE_REMOVE;
}

static void
history_free (gpointer data)
{
  History *h = data;

  g_clear_handle_id (&h->build_id, g_source_remove);
  g_clear_pointer (&h->pending, g_ptr_array_unref);
  /* a row Undo finds must not reach back into a page that is gone */
  for (GtkWidget *c = gtk_widget_get_first_child (h->f->list); c; c = gtk_widget_get_next_sibling (c))
    NB_SWIPE_ROW (c)->history = NULL;
  g_free (h);
}

/* the deleting dialog's spinner: an indeterminate circle (a ring in the primary colour; the whole turns 0 → 720° in
 * 4444 ms, the arc's end runs 0 → 0.75 and its start follows, on the two curves below, and the offset 0 → 0.25,
 * every 1333 ms) */
static void
draw_spinner (GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data)
{
  static SpongyInterp start_curve, end_curve;
  static gboolean ready;
  if (!ready) {
    spongy_interp_path (&end_curve, "M 0 0 C 0.2 0 0.1 1 0.5 1 L 1 1");
    spongy_interp_path (&start_curve, "M 0 0 L 0.5 0 C 0.7 0 0.6 1 1 1");
    ready = TRUE;
  }
  double ms = g_get_monotonic_time () / 1000.0;
  double t = fmod (ms, 1333) / 1333;
  double turn = fmod (ms, 4444) / 4444 * 720;
  double start = 0.75 * spongy_interp_get (&start_curve, (float) t);
  double end = 0.75 * spongy_interp_get (&end_curve, (float) t);
  double offset = 0.25 * t;
  double scale = MIN (w, h) / 48.0;
  GdkRGBA c = vibeos_color (VIBEOS_PRIMARY);

  cairo_translate (cr, w / 2.0, h / 2.0);
  cairo_scale (cr, scale, scale);
  cairo_rotate (cr, turn * G_PI / 180);
  cairo_set_line_width (cr, 4);
  cairo_set_line_cap (cr, CAIRO_LINE_CAP_SQUARE);
  gdk_cairo_set_source_rgba (cr, &c);
  double a0 = -G_PI / 2 + (offset + start) * 2 * G_PI, a1 = -G_PI / 2 + (offset + end) * 2 * G_PI;
  if (a1 - a0 < 0.01)
    a1 = a0 + 0.01;
  cairo_arc (cr, 0, 0, 19, a0, a1);
  cairo_stroke (cr);
}

static gboolean
spinner_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  gtk_widget_queue_draw (widget);
  return G_SOURCE_CONTINUE;
}

static gboolean
close_deleting (gpointer data)
{
  nb_dialog_close (data);
  return G_SOURCE_REMOVE;
}

static void
on_clear_response (NbDialog *d, int response, History *h)
{
  if (response != NB_RESPONSE_OK)
    return;
  /* "Clear history" over a spinner and "Clearing history…", not cancelable, gone 200 ms after */
  NbDialog *busy = nb_dialog_new ("Clear history");
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (row, "nb-deleting");
  GtkWidget *spin = gtk_drawing_area_new ();
  gtk_drawing_area_set_content_width (GTK_DRAWING_AREA (spin), 48);
  gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (spin), 48);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (spin), draw_spinner, NULL, NULL);
  gtk_widget_add_tick_callback (spin, spinner_tick, NULL, NULL);
  GtkWidget *label = nb_label ("Clearing history…", 14, "nb-dialog-message", FALSE);
  gtk_label_set_xalign (GTK_LABEL (label), 0.5);
  /* the spinner at the start, then the message */
  GtkWidget *left = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_set_homogeneous (GTK_BOX (row), FALSE);
  gtk_widget_set_hexpand (left, TRUE);
  gtk_box_append (GTK_BOX (left), spin);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_box_append (GTK_BOX (row), left);
  gtk_box_append (GTK_BOX (row), label);
  nb_dialog_set_content (busy, row);
  nb_dialog_set_sticky (busy, NB_RESPONSE_CANCEL);
  nb_dialog_present (busy, h->t->win);

  nb_store_clear_history (nb->store);
  g_clear_handle_id (&h->build_id, g_source_remove);
  if (h->pending)
    h->next = h->pending->len;
  GtkWidget *c;
  while ((c = gtk_widget_get_first_child (h->f->list)))
    gtk_box_remove (GTK_BOX (h->f->list), c);
  frame_set_empty (h->f, TRUE);
  g_timeout_add_full (G_PRIORITY_DEFAULT, 200, close_deleting, g_object_ref (busy), g_object_unref);
}

static void
on_clear_history (GtkWidget *button, History *h)
{
  NbDialog *d = nb_dialog_new ("Clear history");
  nb_dialog_set_message (d, "You're going to delete all the history, this action cannot be undone. Do you want to "
                            "proceed?");
  nb_dialog_add_button (d, "Cancel", NB_RESPONSE_CANCEL);
  nb_dialog_add_button (d, "Clear", NB_RESPONSE_OK);
  g_signal_connect (d, "response", G_CALLBACK (on_clear_response), h);
  nb_dialog_present (d, h->t->win);
}

static void
entry_copy_free (gpointer data)
{
  NbEntry *e = data;
  g_free (e->url);
  g_free (e->title);
  g_free (e);
}

GtkWidget *
nb_history_page_new (NbTab *t)
{
  History *h = g_new0 (History, 1);
  GtkWidget *del = nb_action_button ("jelly-delete-symbolic", "Clear history");
  GtkWidget *list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *image = nb_icon ("jelly-history-symbolic", 104);

  h->t = t;
  gtk_widget_add_css_class (del, "nb-toolbar-action");
  gtk_widget_add_css_class (image, "nb-empty-image");
  gtk_widget_set_valign (list, GTK_ALIGN_START);
  h->f = frame_new (t, "History", "nb-history", del,
                    list, empty_layout (image, "There's nothing here, browse on the internet to see your history here"),
                    TRUE);
  g_signal_connect (del, "clicked", G_CALLBACK (on_clear_history), h);
  g_object_set_data_full (G_OBJECT (h->f->page), "nb-history", h, history_free);

  /* the rows are copies: another tab may change the store while this page shows it */
  h->pending = g_ptr_array_new_with_free_func (entry_copy_free);
  for (guint i = 0; i < nb->store->history->len; i++) {
    NbEntry *e = nb->store->history->pdata[i], *c = g_new0 (NbEntry, 1);
    c->url = g_strdup (e->url);
    c->title = g_strdup (e->title);
    c->time = e->time;
    g_ptr_array_add (h->pending, c);
  }
  frame_set_empty (h->f, h->pending->len == 0);
  if (build_rows (h))
    h->build_id = g_idle_add_full (G_PRIORITY_LOW, build_rows, h, NULL);
  return h->f->page;
}

/* ==== Favorites =================================================================================================== */

typedef struct {
  Frame     *f;
  NbTab     *t;
  GtkWidget *grid;
} Favorites;

typedef struct {
  Favorites *fav;
  char      *url, *title;
  GtkWidget *title_field, *url_field;
} FavEdit;

static void favorites_fill (Favorites *fav);

/* a resource SVG shown at a fixed size in px, rendered at the window's scale (a GtkPicture would show the texture
 * at its pixel size) */
#define NB_TYPE_SVG_VIEW (nb_svg_view_get_type ())
G_DECLARE_FINAL_TYPE (NbSvgView, nb_svg_view, NB, SVG_VIEW, GtkWidget)

struct _NbSvgView {
  GtkWidget   parent;
  char       *resource;
  int         w, h, scale;
  GdkTexture *texture;
};

G_DEFINE_FINAL_TYPE (NbSvgView, nb_svg_view, GTK_TYPE_WIDGET)

static void
nb_svg_view_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                     int *minimum_baseline, int *natural_baseline)
{
  NbSvgView *self = NB_SVG_VIEW (widget);
  *minimum = *natural = orientation == GTK_ORIENTATION_HORIZONTAL ? self->w : self->h;
}

static void
nb_svg_view_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  NbSvgView *self = NB_SVG_VIEW (widget);
  int scale = gtk_widget_get_scale_factor (widget);

  if (!self->texture || self->scale != scale) {
    g_clear_object (&self->texture);
    g_autoptr (GdkPixbuf) pixbuf = gdk_pixbuf_new_from_resource_at_scale (self->resource, self->w * scale,
                                                                          self->h * scale, TRUE, NULL);
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    self->texture = pixbuf ? gdk_texture_new_for_pixbuf (pixbuf) : NULL;
    G_GNUC_END_IGNORE_DEPRECATIONS
    self->scale = scale;
  }
  if (self->texture)
    gtk_snapshot_append_texture (snapshot, self->texture, &GRAPHENE_RECT_INIT (0, 0, self->w, self->h));
}

static void
nb_svg_view_finalize (GObject *object)
{
  NbSvgView *self = NB_SVG_VIEW (object);
  g_clear_object (&self->texture);
  g_free (self->resource);
  G_OBJECT_CLASS (nb_svg_view_parent_class)->finalize (object);
}

static void
nb_svg_view_class_init (NbSvgViewClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = nb_svg_view_finalize;
  GTK_WIDGET_CLASS (klass)->measure = nb_svg_view_measure;
  GTK_WIDGET_CLASS (klass)->snapshot = nb_svg_view_snapshot;
  gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "nb-svg-view");
}

static void
nb_svg_view_init (NbSvgView *self)
{
}

static GtkWidget *
nb_svg_view_new (const char *resource, int w, int h)
{
  NbSvgView *self = g_object_new (NB_TYPE_SVG_VIEW, NULL);
  self->resource = g_strdup (resource);
  self->w = w;
  self->h = h;
  return GTK_WIDGET (self);
}

/* relative luminance > 0.5 */
static gboolean
is_light (const GdkRGBA *c)
{
  double ch[3] = { c->red, c->green, c->blue };
  for (int i = 0; i < 3; i++)
    ch[i] = ch[i] < 0.04045 ? ch[i] / 12.92 : pow ((ch[i] + 0.055) / 1.055, 2.4);
  return 0.2126 * ch[0] + 0.7152 * ch[1] + 0.0722 * ch[2] > 0.5;
}

static void
fav_edit_free (gpointer data)
{
  FavEdit *e = data;
  g_free (e->url);
  g_free (e->title);
  g_free (e);
}

/* the URL field: "Insert a valid URL" while there is no "http" in it */
static void
on_fav_url_changed (GtkEditable *editable, FavEdit *e)
{
  const char *text = gtk_editable_get_text (editable);
  nb_field_set_error (e->url_field, strstr (text, "http") ? NULL : "Insert a valid URL");
}

static void
on_fav_edit_response (NbDialog *d, int response, FavEdit *e)
{
  Favorites *fav = e->fav;

  if (response == NB_RESPONSE_OK) {
    const char *url = gtk_editable_get_text (GTK_EDITABLE (nb_field_get_entry (e->url_field)));
    const char *title = gtk_editable_get_text (GTK_EDITABLE (nb_field_get_entry (e->title_field)));
    /* Jelly saves whatever is there; a favourite that cannot open is kept from being made instead: the dialog
     * stays with its error */
    if (!strstr (url, "http")) {
      nb_field_set_error (e->url_field, "Insert a valid URL");
      gtk_widget_grab_focus (GTK_WIDGET (nb_field_get_entry (e->url_field)));
      return;
    }
    nb_store_edit_favorite (nb->store, e->url, url, title);
  } else if (response == NB_RESPONSE_NEUTRAL) {
    nb_store_remove_favorite (nb->store, e->url);
  }
  nb_dialog_close (d);
  if (response == NB_RESPONSE_OK || response == NB_RESPONSE_NEUTRAL)
    favorites_fill (fav);
}

static void
on_fav_long_press (GtkWidget *card, Favorites *fav)
{
  FavEdit *e = g_new0 (FavEdit, 1);
  NbDialog *d = nb_dialog_new ("Edit favorite");
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  e->fav = fav;
  e->url = g_strdup (g_object_get_data (G_OBJECT (card), "nb-url"));
  e->title = g_strdup (g_object_get_data (G_OBJECT (card), "nb-title"));
  /* two outlined fields, the title over the URL */
  gtk_widget_add_css_class (box, "nb-fav-edit");
  e->title_field = nb_field_new ("Title", FALSE);
  e->url_field = nb_field_new ("URL", FALSE);
  gtk_widget_add_css_class (e->title_field, "nb-fav-edit-title");
  gtk_editable_set_text (GTK_EDITABLE (nb_field_get_entry (e->title_field)), e->title ? e->title : "");
  gtk_editable_set_text (GTK_EDITABLE (nb_field_get_entry (e->url_field)), e->url);
  gtk_entry_set_input_purpose (nb_field_get_entry (e->url_field), GTK_INPUT_PURPOSE_URL);
  g_signal_connect (nb_field_get_entry (e->url_field), "changed", G_CALLBACK (on_fav_url_changed), e);
  gtk_box_append (GTK_BOX (box), e->title_field);
  gtk_box_append (GTK_BOX (box), e->url_field);
  nb_dialog_set_content (d, box);
  nb_dialog_add_button (d, "Delete", NB_RESPONSE_NEUTRAL);
  nb_dialog_add_button (d, "Cancel", NB_RESPONSE_CANCEL);
  nb_dialog_add_button (d, "Edit", NB_RESPONSE_OK);
  nb_dialog_set_sticky (d, NB_RESPONSE_OK);
  g_object_set_data_full (G_OBJECT (d), "nb-data", e, (GDestroyNotify) fav_edit_free);
g_signal_connect (d, "response", G_CALLBACK (on_fav_edit_response), e);
  nb_dialog_present (d, fav->t->win);
}

static void
on_fav_click (GtkWidget *card, Favorites *fav)
{
  nb_open_tab (g_object_get_data (G_OBJECT (card), "nb-url"), FALSE, NULL, NULL);
}

/* a raised card in the favourite's colour (its site's), the title centred in up to two lines, black or white for
 * contrast; a favourite without a colour of its own is in the theme's primary container */
static GtkWidget *
favorite_card (Favorites *fav, const NbEntry *e)
{
  GdkRGBA color;
  gboolean own = e->color && gdk_rgba_parse (&color, e->color);
  GtkWidget *label = nb_label (e->title && *e->title ? e->title : e->url, 18,
                               !own ? "nb-fav-theme-text" : is_light (&color) ? "nb-fav-dark-text"
                                                                               : "nb-fav-light-text", FALSE);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_lines (GTK_LABEL (label), 2);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_label_set_justify (GTK_LABEL (label), GTK_JUSTIFY_CENTER);
  gtk_label_set_xalign (GTK_LABEL (label), 0.5);
  gtk_widget_add_css_class (label, "nb-fav-title");
  gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
  GtkWidget *card = vibeos_pressable_new (label);
  gtk_widget_set_hexpand (card, TRUE);
  gtk_widget_add_css_class (card, "nb-fav-card");
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (card), VIBEOS_RIPPLE_BOUNDED, 2);
  if (own)
    vibeos_pressable_set_fill (VIBEOS_PRESSABLE (card), &color, FALSE);
  else
    vibeos_pressable_set_fill_role (VIBEOS_PRESSABLE (card), VIBEOS_PRIMARY_CONTAINER, FALSE);
  g_object_set_data_full (G_OBJECT (card), "nb-url", g_strdup (e->url), g_free);
  g_object_set_data_full (G_OBJECT (card), "nb-title", g_strdup (e->title), g_free);
  g_signal_connect (card, "clicked", G_CALLBACK (on_fav_click), fav);
  g_signal_connect (card, "long-pressed", G_CALLBACK (on_fav_long_press), fav);
  return card;
}

static void
favorites_fill (Favorites *fav)
{
  GtkWidget *c;
  while ((c = gtk_widget_get_first_child (fav->grid)))
    gtk_grid_remove (GTK_GRID (fav->grid), c);
  /* two columns, a row as tall as its taller card */
  guint n = nb->store->bookmarks->len;
  for (guint i = 0; i < n; i++)
    gtk_grid_attach (GTK_GRID (fav->grid), favorite_card (fav, nb->store->bookmarks->pdata[i]), i % 2, i / 2, 1, 1);
  /* a lone card is half the width too: an empty cell holds the second column (GtkGrid has no empty columns) */
  if (n % 2) {
    GtkWidget *cell = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand (cell, TRUE);
    gtk_grid_attach (GTK_GRID (fav->grid), cell, 1, n / 2, 1, 1);
  }
  frame_set_empty (fav->f, nb->store->bookmarks->len == 0);
}

GtkWidget *
nb_favorites_page_new (NbTab *t)
{
  Favorites *fav = g_new0 (Favorites, 1);
  GtkWidget *image = nb_svg_view_new (NB_RESOURCE_ICONS "jelly-favorite-empty.svg", 192, 96);

  fav->t = t;
  fav->grid = gtk_grid_new ();
  gtk_grid_set_column_homogeneous (GTK_GRID (fav->grid), TRUE);
  gtk_widget_add_css_class (fav->grid, "nb-fav-grid");
  gtk_widget_set_valign (fav->grid, GTK_ALIGN_START);
  /* the empty page's picture, in its own colours */
  gtk_widget_add_css_class (image, "nb-fav-empty-image");
  fav->f = frame_new (t, "Favorites", "nb-favorites", NULL, fav->grid,
                      empty_layout (image, "There's nothing here, add a favorite link to see it here"), TRUE);
  g_object_set_data_full (G_OBJECT (fav->f->page), "nb-favorites", fav, g_free);
  favorites_fill (fav);
  return fav->f->page;
}

/* ==== Downloads: the browser's downloads, newest first ============================================================ */

typedef struct {
  Frame     *f;
  NbTab     *t;
  guint      refresh_id;
} Downloads;

/* SI units, two decimals under 100 */
static char *
format_size (guint64 bytes)
{
  static const char *const units[] = { "B", "kB", "MB", "GB", "TB" };
  double v = bytes;
  int u = 0;
  while (v > 900 && u < 4) {
    v /= 1000;
    u++;
  }
  if (u == 0)
    return g_strdup_printf ("%" G_GUINT64_FORMAT " B", bytes);
  return g_strdup_printf (v < 100 ? "%.2f %s" : "%.0f %s", v, units[u]);
}

/* the time today, "Nov 4" this year, else with the year */
static char *
format_day (gint64 time)
{
  g_autoptr (GDateTime) dt = g_date_time_new_from_unix_local (time);
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  if (!dt)
    return g_strdup ("");
  if (g_date_time_get_year (dt) == g_date_time_get_year (now) &&
      g_date_time_get_day_of_year (dt) == g_date_time_get_day_of_year (now))
    return g_date_time_format (dt, "%H:%M");
  if (g_date_time_get_year (dt) == g_date_time_get_year (now))
    return g_date_time_format (dt, "%b %-d");
  return g_date_time_format (dt, "%b %-d, %Y");
}

static void
on_download_row (GtkWidget *row, gpointer data)
{
  const char *path = g_object_get_data (G_OBJECT (row), "nb-path");
  NbTab *t = data;
  if (!path)
    return;
  g_autoptr (GFile) file = g_file_new_for_path (path);
  g_autoptr (GtkFileLauncher) launcher = gtk_file_launcher_new (file);
  gtk_file_launcher_launch (launcher, GTK_WINDOW (t->win), NULL, NULL, NULL);
}

/* a download's row: the type's glyph on its tile, then the name over "date, size, type" */
static GtkWidget *
download_row (Downloads *dl, NbDownload *e)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *icon_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  g_autofree char *type = e->mime ? g_content_type_from_mime_type (e->mime) : NULL;
  if (!type && e->mime)
    type = g_strdup (e->mime);
  g_autoptr (GIcon) gicon = type ? g_content_type_get_symbolic_icon (type) : NULL;
  GtkWidget *glyph = gicon ? gtk_image_new_from_gicon (gicon) : nb_icon ("jelly-file-download-symbolic", 24);
  gtk_image_set_pixel_size (GTK_IMAGE (glyph), 24);
  gtk_widget_add_css_class (glyph, "nb-download-glyph");
  gtk_widget_set_halign (glyph, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (glyph, TRUE);
  gtk_widget_add_css_class (icon_box, "nb-download-icon");
  gtk_widget_set_valign (icon_box, GTK_ALIGN_CENTER);
  /* the tile stays square: the glyph's expand (to centre it) must not widen it in the row */
  gtk_widget_set_hexpand (icon_box, FALSE);
  gtk_box_append (GTK_BOX (icon_box), glyph);
  gtk_box_append (GTK_BOX (box), icon_box);

  GtkWidget *text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (text, TRUE);
  gtk_box_append (GTK_BOX (text), nb_label (e->name ? e->name : "", 16, "nb-download-name", TRUE));
  g_autofree char *line2 = NULL;
  if (e->failed) {
    line2 = g_strdup ("Download failed");
  } else if (!e->done && e->d) {
    double p = webkit_download_get_estimated_progress (e->d);
    line2 = g_strdup_printf ("Downloading… %d%%", (int) round (p * 100));
  } else {
    g_autofree char *day = format_day (e->time);
    g_autofree char *size = format_size (e->size);
    g_autofree char *desc = type ? g_content_type_get_description (type) : g_strdup ("File");
    line2 = g_strdup_printf ("%s, %s, %s", day, size, desc);
  }
  gtk_box_append (GTK_BOX (text), nb_label (line2, 14, "nb-download-meta", TRUE));
  gtk_box_append (GTK_BOX (box), text);
  gtk_widget_add_css_class (box, "nb-download-row");

  GtkWidget *row = vibeos_pressable_new (box);
  gtk_widget_add_css_class (row, "nb-download-pressable");
  if (e->done && !e->failed && e->path)
    g_object_set_data_full (G_OBJECT (row), "nb-path", g_strdup (e->path), g_free);
  g_signal_connect (row, "clicked", G_CALLBACK (on_download_row), dl->t);
  return row;
}

static void
downloads_fill (Downloads *dl)
{
  GtkWidget *c;
  while ((c = gtk_widget_get_first_child (dl->f->list)))
    gtk_box_remove (GTK_BOX (dl->f->list), c);
  for (guint i = 0; i < nb->downloads->len; i++)
    gtk_box_append (GTK_BOX (dl->f->list), download_row (dl, nb->downloads->pdata[i]));
  frame_set_empty (dl->f, nb->downloads->len == 0);
}

static gboolean
on_refresh_due (gpointer data)
{
  Downloads *dl = data;
  dl->refresh_id = 0;
  downloads_fill (dl);
  return G_SOURCE_REMOVE;
}

void
nb_downloads_page_refresh (GtkWidget *page)
{
  Downloads *dl = g_object_get_data (G_OBJECT (page), "nb-downloads");
  /* progress comes many times a second: the list is redrawn four times a second at most */
  if (dl && !dl->refresh_id)
    dl->refresh_id = g_timeout_add (250, on_refresh_due, dl);
}

static void
downloads_free (gpointer data)
{
  Downloads *dl = data;
  g_clear_handle_id (&dl->refresh_id, g_source_remove);
  if (dl->t->downloads_page == dl->f->page)
    dl->t->downloads_page = NULL;
  g_free (dl);
}

GtkWidget *
nb_downloads_page_new (NbTab *t)
{
  Downloads *dl = g_new0 (Downloads, 1);
  GtkWidget *list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *image = nb_icon ("jelly-download-symbolic", 104);

  dl->t = t;
  gtk_widget_set_valign (list, GTK_ALIGN_START);
  gtk_widget_add_css_class (list, "nb-download-list");
  gtk_widget_add_css_class (image, "nb-empty-image");
  dl->f = frame_new (t, "Downloads", "nb-downloads", NULL, list, empty_layout (image, "No items"), TRUE);
  g_object_set_data_full (G_OBJECT (dl->f->page), "nb-downloads", dl, downloads_free);
  nb_scan_downloads ();
  downloads_fill (dl);
  t->downloads_page = dl->f->page;
  return dl->f->page;
}

/* ==== Desktop shortcuts =========================================================================================== */

typedef struct {
  Frame      *f;
  NbTab      *t;
  GHashTable *selected;     /* shortcut ids */
  GPtrArray  *shortcuts;    /* NbShortcut */
} Shortcuts;

static void
shortcuts_free (gpointer data)
{
  Shortcuts *s = data;
  g_hash_table_unref (s->selected);
  g_ptr_array_unref (s->shortcuts);
  g_free (s);
}

static void
on_shortcut_row (GtkWidget *row, Shortcuts *s)
{
  /* a tap toggles the row */
  const char *id = g_object_get_data (G_OBJECT (row), "nb-id");
  NbMark *mark = g_object_get_data (G_OBJECT (row), "nb-mark");
  gboolean on = !g_hash_table_contains (s->selected, id);
  if (on)
    g_hash_table_add (s->selected, g_strdup (id));
  else
    g_hash_table_remove (s->selected, id);
  nb_mark_set_active (mark, on);
}

static void
on_shortcuts_save (GtkWidget *button, Shortcuts *s)
{
  /* save (): only ids that still exist, then finish () */
  g_autoptr (GPtrArray) ids = g_ptr_array_new ();
  for (guint i = 0; i < s->shortcuts->len; i++) {
    NbShortcut *sc = s->shortcuts->pdata[i];
    if (g_hash_table_contains (s->selected, sc->id))
      g_ptr_array_add (ids, sc->id);
  }
  g_ptr_array_add (ids, NULL);
  nb_store_set_strv (nb->store, "desktopShortcuts", (const char *const *) ids->pdata);
  nb_tab_pop_page (s->t);
}

/* a shortcut's row: the checkbox, then the name */
static GtkWidget *
shortcut_row (Shortcuts *s, NbShortcut *sc)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *mark = nb_mark_new (NB_MARK_CHECK);
  GtkWidget *name = nb_label (sc->name, 18, "nb-shortcut-name", TRUE);

  gtk_widget_add_css_class (box, "nb-shortcut-row");
  nb_mark_set_active (NB_MARK (mark), g_hash_table_contains (s->selected, sc->id));
  gtk_widget_set_valign (mark, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), mark);
  gtk_widget_set_hexpand (name, TRUE);
  gtk_widget_set_valign (name, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), name);
  GtkWidget *row = vibeos_pressable_new (box);
  g_object_set_data_full (G_OBJECT (row), "nb-id", g_strdup (sc->id), g_free);
  g_object_set_data (G_OBJECT (row), "nb-mark", mark);
  g_signal_connect (row, "clicked", G_CALLBACK (on_shortcut_row), s);
  return row;
}

static int
shortcut_cmp (gconstpointer a, gconstpointer b)
{
  const NbShortcut *x = *(NbShortcut **) a, *y = *(NbShortcut **) b;
  return g_utf8_collate (x->name, y->name);
}

GtkWidget *
nb_desktop_shortcuts_page_new (NbTab *t)
{
  Shortcuts *s = g_new0 (Shortcuts, 1);
  GtkWidget *save = nb_action_button ("jelly-check-symbolic", "Save");
  GtkWidget *list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  s->t = t;
  gtk_widget_add_css_class (save, "nb-toolbar-action");
  gtk_widget_set_valign (list, GTK_ALIGN_START);
  s->f = frame_new (t, "Desktop shortcuts", "nb-shortcuts", save, list, NULL, FALSE);
  g_object_set_data_full (G_OBJECT (s->f->page), "nb-shortcuts", s, shortcuts_free);
  g_signal_connect (save, "clicked", G_CALLBACK (on_shortcuts_save), s);
  s->shortcuts = nb_list_shortcuts ();
  g_ptr_array_sort (s->shortcuts, shortcut_cmp);
  s->selected = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_auto (GStrv) saved = nb_store_get_strv (nb->store, "desktopShortcuts");
  for (guint i = 0; saved && saved[i]; i++)
    g_hash_table_add (s->selected, g_strdup (saved[i]));
  for (guint i = 0; i < s->shortcuts->len; i++)
    gtk_box_append (GTK_BOX (list), shortcut_row (s, s->shortcuts->pdata[i]));
  return s->f->page;
}
