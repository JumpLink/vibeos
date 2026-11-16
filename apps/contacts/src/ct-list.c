/* The contact list (ct-list.h).
 *
 * Three widgets: CtList, the one the screens hold, has the scroll view (libspongy: the drag, the fling, the
 * stretch) and draws the fast scroller over it; inside the scroll view is CtListContent, a GtkScrollable as tall as
 * the view, which draws the rows the adjustment says are showing, and the pinned letter over them. The fast
 * scroller's drag is CtList's, in the capture phase, so that it gets the touch before the scroll view does. */
#include "ct-list.h"
#include "ct-avatar.h"
#include "ct-ui.h"

#include <math.h>
#include <vibeos.h>
#include <spongy-gtk.h>

#define ROW             CT_ROW_H_DP
#define COLUMN          CT_ROW_HEADER_COL_DP
#define PHOTO           CT_ROW_PHOTO_DP
#define CHECKBOX        32.0                    /* the selection checkbox, with its margin */
#define NAME_STYLE      CT_TEXT (CT_ROW_NAME_SP, CT_WEIGHT_REGULAR)
#define SNIPPET_STYLE   CT_TEXT (CT_ROW_SECONDARY_SP, CT_WEIGHT_REGULAR)
#define HEADER_STYLE    CT_TEXT (CT_SECTION_SP, CT_WEIGHT_BOLD)
#define PREVIEW_STYLE   CT_TEXT (CT_FS_PREVIEW_TEXT_SP, CT_WEIGHT_REGULAR)
#define N_PRESSES       3                       /* rows whose ripple can be on screen at once */
#define LOAD_STAGGER    (CT_LIST_LOAD_MS * CT_LIST_LOAD_DELAY_FRAC)

typedef struct {
  int row;                      /* −1: free */
  VibeosRipple ripple;
} Press;

/* a row's text, kept while the row shows (shaping every name on every frame of a fling is wasted work) */
typedef struct {
  PangoLayout *name, *snippet;
  double width;
} RowText;

struct _CtList {
  GtkWidget parent;
  GtkWidget *scroll, *content;
  CtRows *rows;
  gboolean search;
  double top_padding;
  GHashTable *selected;         /* uid set, or NULL outside selection mode */
  GHashTable *texts;            /* row index -> RowText */
  GHashTable *headers;          /* section label -> PangoLayout */

  /* the press */
  Press presses[N_PRESSES];
  int down_row;
  double down_x, down_y;
  gboolean long_pressed;
  guint tap_timeout, long_press_timeout;

  /* the rows' first appearance */
  VibeosAnim *load_anim;
  double load_ms;               /* ms into it; < 0 when done */

  /* the fast scroller and the search's scroll bar: shown while scrolling, then faded */
  VibeosAnim *bar_anim;
  double bar_alpha;
  guint bar_hide;
  gboolean dragging;
  PangoLayout *preview;
  char *preview_label;
};

G_DEFINE_FINAL_TYPE (CtList, ct_list, GTK_TYPE_WIDGET)

enum { ROW_ACTIVATED, ROW_LONG_PRESSED, N_SIGNALS };
static guint signals[N_SIGNALS];

static GtkAdjustment *
vadjustment (CtList *self)
{
  return spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (self->scroll));
}

static guint
n_rows (CtList *self)
{
  return self->rows ? self->rows->rows->len : 0;
}

static double
content_height (CtList *self)
{
  return self->top_padding + n_rows (self) * ROW;
}

/* the row at @y of the content (the view's coordinates plus the scroll offset), or −1 */
static int
row_at (CtList *self, double y)
{
  double index = floor ((y - self->top_padding) / ROW);

  return index >= 0 && index < n_rows (self) ? (int) index : -1;
}

/* ===== the content ============================================================================================ */

#define CT_TYPE_LIST_CONTENT (ct_list_content_get_type ())
G_DECLARE_FINAL_TYPE (CtListContent, ct_list_content, CT, LIST_CONTENT, GtkWidget)

struct _CtListContent {
  GtkWidget parent;
  CtList *list;
  GtkAdjustment *vadjustment, *hadjustment;
};

enum { PROP_0, PROP_HADJUSTMENT, PROP_VADJUSTMENT, PROP_HSCROLL_POLICY, PROP_VSCROLL_POLICY };

G_DEFINE_FINAL_TYPE_WITH_CODE (CtListContent, ct_list_content, GTK_TYPE_WIDGET,
                               G_IMPLEMENT_INTERFACE (GTK_TYPE_SCROLLABLE, NULL))

static double
offset (CtList *self)
{
  CtListContent *content = CT_LIST_CONTENT (self->content);

  return content->vadjustment ? gtk_adjustment_get_value (content->vadjustment) : 0;
}

static void
configure (CtListContent *self)
{
  double height = gtk_widget_get_height (GTK_WIDGET (self));
  double upper, value;

  if (!self->vadjustment)
    return;
  upper = MAX (content_height (self->list), height);
  value = CLAMP (gtk_adjustment_get_value (self->vadjustment), 0, upper - height);
  gtk_adjustment_configure (self->vadjustment, value, 0, upper, ROW, height * 0.9, height);
}

static void
ct_list_content_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  CtListContent *self = CT_LIST_CONTENT (widget);

  /* a new width is new ellipsizing */
  if (self->list->texts)
    g_hash_table_remove_all (self->list->texts);
  configure (self);
}

static void
ct_list_content_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                         int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 0;
}

static void
row_text_free (gpointer data)
{
  RowText *text = data;

  g_clear_object (&text->name);
  g_clear_object (&text->snippet);
  g_free (text);
}

/* where a row's name starts and how wide it may be */
static void
text_bounds (CtList *self, double width, double *x, double *text_width)
{
  /* the section column (none in a search), a gap, the picture, a gap */
  double left = (self->search ? 0 : COLUMN) + CT_ROW_GAP_HEADER_PHOTO_DP + PHOTO + CT_ROW_GAP_PHOTO_TEXT_DP;
  double right = width - CT_ROW_PAD_RIGHT_DP;

  /* the checkbox and its gap come off the name's end */
  if (self->selected)
    right -= CHECKBOX + CT_ROW_GAP_PHOTO_TEXT_DP;
  *x = left;
  *text_width = MAX (right - left, 0);
}

static RowText *
row_text (CtList *self, GtkWidget *widget, guint index, double width)
{
  RowText *text = g_hash_table_lookup (self->texts, GUINT_TO_POINTER (index));
  CtRow *row = &g_array_index (self->rows->rows, CtRow, index);

  if (text && text->width == width)
    return text;
  text = g_new0 (RowText, 1);
  text->width = width;
  /* the match highlight is a bold span, which Bold text makes the same weight as the rest: the name is
   * drawn as it is */
  text->name = text_layout (widget, NAME_STYLE, row->name && *row->name ? row->name : " ", width);
  if (row->match.snippet)
    text->snippet = text_layout (widget, SNIPPET_STYLE, row->match.snippet, width);
  g_hash_table_replace (self->texts, GUINT_TO_POINTER (index), text);
  return text;
}

static PangoLayout *
header_layout (CtList *self, GtkWidget *widget, const char *label)
{
  PangoLayout *layout = g_hash_table_lookup (self->headers, label);

  if (!layout)
    {
      /* in capitals */
      g_autofree char *caps = g_utf8_strup (label, -1);

      layout = text_layout (widget, HEADER_STYLE, caps, 0);
      g_hash_table_insert (self->headers, (gpointer) label, layout);
    }
  return layout;
}

/* a section letter centred in the section column of a row-high box at @top: centred, then nudged up
 * (CT_ROW_TEXT_OFFSET_TOP_DP) */
static void
draw_header (CtList *self, GtkWidget *widget, GtkSnapshot *snapshot, const char *label, double top)
{
  PangoLayout *layout = header_layout (self, widget, label);
  PangoRectangle logical;
  GdkRGBA color = ct_rgba (CT_C_SECTION_HEADER);

  pango_layout_get_extents (layout, NULL, &logical);
  text_draw (snapshot, layout, HEADER_STYLE, (COLUMN - logical.width / (double) PANGO_SCALE) / 2,
             top + (ROW - text_box_height (layout, HEADER_STYLE)) / 2 + CT_ROW_TEXT_OFFSET_TOP_DP, &color);
}

static Press *
press_of (CtList *self, int row)
{
  for (int i = 0; i < N_PRESSES; i++)
    if (self->presses[i].row == row)
      return &self->presses[i];
  return NULL;
}

static void
draw_row (CtList *self, GtkWidget *widget, GtkSnapshot *snapshot, guint index, double top, double width)
{
  CtRow *row = &g_array_index (self->rows->rows, CtRow, index);
  double left = (self->search ? 0 : COLUMN) + CT_ROW_GAP_HEADER_PHOTO_DP;
  double x, text_width, text_height, text_top;
  GdkRGBA name_color = ct_rgba (CT_C_LIST_NAME), snippet_color = ct_rgba (CT_C_LIST_SECONDARY);
  Press *press = press_of (self, (int) index);
  RowText *text;
  GdkTexture *photo;

  if (press && vibeos_ripple_is_active (&press->ripple))
    {
      graphene_rect_t bounds = GRAPHENE_RECT_INIT (0, (float) top, (float) width, (float) ROW);
      GdkRGBA ripple = ct_rgba (CT_RIPPLE_LIST_SRC);

      gtk_snapshot_push_clip (snapshot, &bounds);
      gtk_snapshot_save (snapshot);
      gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (0, (float) top));
      vibeos_ripple_snapshot (&press->ripple, snapshot, &GRAPHENE_RECT_INIT (0, 0, (float) width, (float) ROW), &ripple);
      gtk_snapshot_restore (snapshot);
      gtk_snapshot_pop (snapshot);
    }

  /* the section's first row shows its letter, or the star for the favourites */
  if (!self->search && row->first && row->section >= 0)
    {
      const CtSection *section = &g_array_index (self->rows->sections, CtSection, row->section);

      if (section->rank == CT_BUCKET_FAVOURITES)
        {
          GdkRGBA star = ct_rgba (CT_C_SECTION_HEADER);

          vibeos_snapshot_icon (snapshot, widget, "ct-star-symbolic", (COLUMN - 24) / 2, top + (ROW - 24) / 2, 24, &star);
        }
      else
        draw_header (self, widget, snapshot, section->label, top);
    }

  /* the picture: centred, then nudged up (CT_ROW_PHOTO_OFFSET_TOP_DP) */
  photo = row->entry->photo ? ct_avatar_photo (widget, row->entry->uid, row->entry->photo) : NULL;
  ct_avatar_snapshot (snapshot, widget, left + PHOTO / 2, top + ROW / 2 + CT_ROW_PHOTO_OFFSET_TOP_DP, PHOTO,
                      row->entry->uid, row->name, photo);

  /* the name, and the line that matched under it, centred together, nudged up */
  text_bounds (self, width, &x, &text_width);
  text = row_text (self, widget, index, text_width);
  text_height = text_box_height (text->name, NAME_STYLE);
  if (text->snippet)
    text_height += text_box_height (text->snippet, SNIPPET_STYLE);
  text_top = top + (ROW - text_height) / 2 + CT_ROW_TEXT_OFFSET_TOP_DP;
  text_draw (snapshot, text->name, NAME_STYLE, x, text_top, &name_color);
  if (text->snippet)
    text_draw (snapshot, text->snippet, SNIPPET_STYLE, x, text_top + text_box_height (text->name, NAME_STYLE),
               &snippet_color);

  /* the checkbox: vertically centred, 20 device px in from the end padding */
  if (self->selected)
    ct_snapshot_checkbox (snapshot, width - CT_ROW_PAD_RIGHT_DP - CT_ROW_CHECKBOX_GAP_PX / 3.0 - CHECKBOX,
                          top + (ROW - CHECKBOX) / 2,
                          g_hash_table_contains (self->selected, row->entry->uid), CT_C_SECTION_HEADER,
                          CT_C_LIST_SECONDARY);
}

/* the letter of the section the top row is in, pinned over the column with
 * the list's background behind it; when the section's last row goes up past it, the letter goes up with it and
 * fades. The favourites' star is never pinned. */
static void
draw_pinned_header (CtList *self, GtkWidget *widget, GtkSnapshot *snapshot, double scrolled)
{
  int index = row_at (self, MAX (scrolled, self->top_padding));
  const CtSection *section = index >= 0 ? ct_rows_section_at (self->rows, (guint) index) : NULL;
  double y = 0, alpha = 1;
  const CtSection *next;
  GdkRGBA background = ct_rgba (CT_C_WINDOW_BG);

  if (!section || section->rank == CT_BUCKET_FAVOURITES)
    return;
  next = (guint) index + 1 < n_rows (self) ? ct_rows_section_at (self->rows, (guint) index + 1) : NULL;
  if (next && next != section)
    {
      double bottom = self->top_padding + (index + 1) * ROW - scrolled;

      if (bottom < ROW)
        {
          double portion = bottom - ROW;

          alpha = (ROW + portion) / ROW;
          y = portion;
        }
    }
  gtk_snapshot_push_opacity (snapshot, CLAMP (alpha, 0, 1));
  gtk_snapshot_append_color (snapshot, &background, &GRAPHENE_RECT_INIT (0, (float) y, (float) COLUMN, (float) ROW));
  draw_header (self, widget, snapshot, section->label, y);
  gtk_snapshot_pop (snapshot);
}

static void
ct_list_content_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CtList *self = CT_LIST_CONTENT (widget)->list;
  double width = gtk_widget_get_width (widget), height = gtk_widget_get_height (widget);
  double scrolled = offset (self);
  int first, last;

  if (!n_rows (self))
    return;
  first = MAX (0, (int) floor ((scrolled - self->top_padding) / ROW));
  last = MIN ((int) n_rows (self) - 1, (int) floor ((scrolled + height - self->top_padding) / ROW));
  /* the texts of rows that went off screen go */
  {
    GHashTableIter iter;
    gpointer key;

    g_hash_table_iter_init (&iter, self->texts);
    while (g_hash_table_iter_next (&iter, &key, NULL))
      if ((int) GPOINTER_TO_UINT (key) < first || (int) GPOINTER_TO_UINT (key) > last)
        g_hash_table_iter_remove (&iter);
  }
  for (int i = first; i <= last; i++)
    {
      double top = self->top_padding + i * ROW - scrolled;
      gboolean animating = self->load_ms >= 0;

      if (animating)
        {
          /* row i (of those on screen) starts 19 ms after the one above it: alpha 0 -> 1, 5 % of a row up */
          double t = CLAMP ((self->load_ms - (i - first) * LOAD_STAGGER) / CT_LIST_LOAD_MS, 0, 1);
          double eased = spongy_curve (SPONGY_LINEAR_OUT_SLOW_IN, t);

          gtk_snapshot_push_opacity (snapshot, eased);
          top += (1 - eased) * ROW * CT_LIST_LOAD_TRANSLATE_FRAC;
        }
      draw_row (self, widget, snapshot, (guint) i, top, width);
      if (animating)
        gtk_snapshot_pop (snapshot);
    }
  if (!self->search)
    draw_pinned_header (self, widget, snapshot, scrolled);
}

static void
on_value_changed (GtkAdjustment *adjustment, CtListContent *self);

static void
set_adjustment (CtListContent *self, GtkAdjustment **slot, GtkAdjustment *adjustment)
{
  if (*slot == self->vadjustment && self->vadjustment)
    g_signal_handlers_disconnect_by_func (self->vadjustment, on_value_changed, self);
  g_clear_object (slot);
  if (adjustment)
    *slot = g_object_ref_sink (adjustment);
  if (slot == &self->vadjustment && self->vadjustment)
    {
      g_signal_connect (self->vadjustment, "value-changed", G_CALLBACK (on_value_changed), self);
      configure (self);
    }
}

static void
ct_list_content_set_property (GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
  CtListContent *self = CT_LIST_CONTENT (object);

  switch (id)
    {
    case PROP_VADJUSTMENT:
      set_adjustment (self, &self->vadjustment, g_value_get_object (value));
      break;
    case PROP_HADJUSTMENT:
      set_adjustment (self, &self->hadjustment, g_value_get_object (value));
      break;
    case PROP_HSCROLL_POLICY:
    case PROP_VSCROLL_POLICY:
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, pspec);
    }
}

static void
ct_list_content_get_property (GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  CtListContent *self = CT_LIST_CONTENT (object);

  switch (id)
    {
    case PROP_VADJUSTMENT:
      g_value_set_object (value, self->vadjustment);
      break;
    case PROP_HADJUSTMENT:
      g_value_set_object (value, self->hadjustment);
      break;
    case PROP_HSCROLL_POLICY:
    case PROP_VSCROLL_POLICY:
      g_value_set_enum (value, GTK_SCROLL_MINIMUM);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, pspec);
    }
}

static void
ct_list_content_dispose (GObject *object)
{
  CtListContent *self = CT_LIST_CONTENT (object);

  set_adjustment (self, &self->vadjustment, NULL);
  set_adjustment (self, &self->hadjustment, NULL);
  G_OBJECT_CLASS (ct_list_content_parent_class)->dispose (object);
}

static void
ct_list_content_class_init (CtListContentClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = ct_list_content_dispose;
  object_class->set_property = ct_list_content_set_property;
  object_class->get_property = ct_list_content_get_property;
  widget_class->measure = ct_list_content_measure;
  widget_class->size_allocate = ct_list_content_size_allocate;
  widget_class->snapshot = ct_list_content_snapshot;
  g_object_class_override_property (object_class, PROP_HADJUSTMENT, "hadjustment");
  g_object_class_override_property (object_class, PROP_VADJUSTMENT, "vadjustment");
  g_object_class_override_property (object_class, PROP_HSCROLL_POLICY, "hscroll-policy");
  g_object_class_override_property (object_class, PROP_VSCROLL_POLICY, "vscroll-policy");
  gtk_widget_class_set_css_name (widget_class, "ct-list-content");
}

static void
ct_list_content_init (CtListContent *self)
{
  gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);
}

/* ===== pressing a row (VibeosPressable's rules, for rows that are not widgets) ===================================== */

static void
clear_timeouts (CtList *self)
{
  g_clear_handle_id (&self->tap_timeout, g_source_remove);
  g_clear_handle_id (&self->long_press_timeout, g_source_remove);
}

/* the ripple of the row the finger is on starts: in a free slot, else in the oldest */
static void
start_ripple (CtList *self)
{
  Press *press = press_of (self, self->down_row);
  double top = self->top_padding + self->down_row * ROW - offset (self);

  for (int i = 0; !press && i < N_PRESSES; i++)
    if (!vibeos_ripple_is_active (&self->presses[i].ripple))
      press = &self->presses[i];
  if (!press)
    {
      press = &self->presses[0];
      vibeos_ripple_clear (&press->ripple);
    }
  press->row = self->down_row;
  vibeos_ripple_press (&press->ripple, self->down_x, self->down_y - top);
}

static gboolean
on_tap_timeout (gpointer data)
{
  CtList *self = data;

  self->tap_timeout = 0;
  start_ripple (self);
  return G_SOURCE_REMOVE;
}

static gboolean
on_long_press_timeout (gpointer data)
{
  CtList *self = data;

  self->long_press_timeout = 0;
  self->long_pressed = TRUE;
  if (self->tap_timeout)
    {
      g_clear_handle_id (&self->tap_timeout, g_source_remove);
      start_ripple (self);
    }
  g_signal_emit (self, signals[ROW_LONG_PRESSED], 0, (guint) self->down_row);
  return G_SOURCE_REMOVE;
}

static void
release_ripple (CtList *self)
{
  Press *press = press_of (self, self->down_row);

  if (press)
    vibeos_ripple_release (&press->ripple);
}

static void
on_pressed (GtkGestureClick *click, int n_press, double x, double y, CtList *self)
{
  int row = row_at (self, y + offset (self));

  clear_timeouts (self);
  self->down_row = row;
  if (row < 0)
    {
      gtk_gesture_set_state (GTK_GESTURE (click), GTK_EVENT_SEQUENCE_DENIED);
      return;
    }
  self->down_x = x;
  self->down_y = y;
  self->long_pressed = FALSE;
  self->tap_timeout = g_timeout_add (CT_TAP_TIMEOUT_MS, on_tap_timeout, self);
  self->long_press_timeout = g_timeout_add (CT_LONG_PRESS_MS, on_long_press_timeout, self);
}

static void
on_released (GtkGestureClick *click, int n_press, double x, double y, CtList *self)
{
  int row = self->down_row;

  if (row < 0)
    return;
  /* a tap quicker than the tap timeout still plays the whole ripple */
  if (self->tap_timeout)
    {
      g_clear_handle_id (&self->tap_timeout, g_source_remove);
      start_ripple (self);
    }
  g_clear_handle_id (&self->long_press_timeout, g_source_remove);
  release_ripple (self);
  self->down_row = -1;
  if (!self->long_pressed && (guint) row < n_rows (self))
    g_signal_emit (self, signals[ROW_ACTIVATED], 0, (guint) row);
}

/* the press is lost: the scroll view took the touch, or the finger moved away */
static void
on_lost (GtkGesture *gesture, gpointer unused, CtList *self)
{
  if (self->down_row < 0)
    return;
  clear_timeouts (self);
  release_ripple (self);
  self->down_row = -1;
}

static gboolean
moved_away (CtList *self, GtkGesture *gesture)
{
  GdkEventSequence *sequence = gtk_gesture_single_get_current_sequence (GTK_GESTURE_SINGLE (gesture));
  int distance = 5;
  double x, y;

  if (!gtk_gesture_get_point (gesture, sequence, &x, &y))
    return TRUE;
  g_object_get (gtk_widget_get_settings (GTK_WIDGET (self)), "gtk-double-click-distance", &distance, NULL);
  return hypot (x - self->down_x, y - self->down_y) > distance;
}

static void
on_stopped (GtkGestureClick *click, CtList *self)
{
  if (moved_away (self, GTK_GESTURE (click)))
    on_lost (GTK_GESTURE (click), NULL, self);
}

static void
on_update (GtkGesture *gesture, GdkEventSequence *sequence, CtList *self)
{
  if (self->down_row >= 0 && moved_away (self, gesture))
    on_lost (gesture, NULL, self);
}

/* ===== the fast scroller and the scroll bar ===================================================================== */

/* the fast scroller shows only for a list at least four screens long; a search has the plain scroll bar */
static gboolean
fast_scroller_enabled (CtList *self)
{
  double page = gtk_widget_get_height (GTK_WIDGET (self));

  return !self->search && page > 0 && content_height (self) >= CT_FS_MIN_PAGES * page;
}

static void
on_bar_anim (VibeosAnim *anim, double value, gpointer data)
{
  CtList *self = data;

  self->bar_alpha = value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static gboolean
on_bar_hide (gpointer data)
{
  CtList *self = data;

  self->bar_hide = 0;
  if (self->dragging)
    return G_SOURCE_REMOVE;
  vibeos_anim_start (self->bar_anim, self->bar_alpha, 0, fast_scroller_enabled (self) ? CT_FS_FADE_OUT_MS
                                                                                   : CT_SCROLLBAR_FADE_MS,
                  0, SPONGY_LINEAR);
  return G_SOURCE_REMOVE;
}

/* the list moved: the bar shows, and fades once it has been still a while (1500 ms the fast scroller, 400 ms the
 * scroll bar) */
static void
show_bar (CtList *self)
{
  gboolean fast = fast_scroller_enabled (self);

  /* a list too short for the fast scroller has no bar at all: only a search has the plain one */
  if (!fast && !self->search)
    return;
  if (self->bar_alpha < 1 && !vibeos_anim_is_running (self->bar_anim))
    vibeos_anim_start (self->bar_anim, self->bar_alpha, 1, fast ? CT_FS_FADE_IN_MS : 0, 0, SPONGY_LINEAR);
  else if (vibeos_anim_is_running (self->bar_anim) && vibeos_anim_get_to (self->bar_anim) < 1)
    vibeos_anim_start (self->bar_anim, self->bar_alpha, 1, fast ? CT_FS_FADE_IN_MS : 0, 0, SPONGY_LINEAR);
  g_clear_handle_id (&self->bar_hide, g_source_remove);
  self->bar_hide = g_timeout_add (fast ? CT_FS_HIDE_DELAY_MS : CT_SCROLLBAR_DELAY_MS, on_bar_hide, self);
}

static void
on_value_changed (GtkAdjustment *adjustment, CtListContent *content)
{
  CtList *self = content->list;

  /* the finger's press goes when the list moves under it */
  if (self->down_row >= 0 && !self->long_pressed)
    {
      clear_timeouts (self);
      release_ripple (self);
      self->down_row = -1;
    }
  show_bar (self);
  gtk_widget_queue_draw (GTK_WIDGET (content));
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* the thumb's top: as far down the track as the list is scrolled */
static double
thumb_top (CtList *self, double thumb)
{
  GtkAdjustment *adj = vadjustment (self);
  double track = gtk_widget_get_height (GTK_WIDGET (self));
  double range = gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj);
  double proportion = range > 0 ? CLAMP (gtk_adjustment_get_value (adj) / range, 0, 1) : 0;

  return proportion * (track - thumb);
}

/* the fast scroller's letter: a teardrop in the accent colour, its square corner towards the thumb, a little in
 * from it, its foot level with the thumb's top */
static void
draw_preview (CtList *self, GtkSnapshot *snapshot, double width, double thumb)
{
  PangoRectangle logical;
  double w, h, right, bottom, corner = CT_FS_PREVIEW_CORNER_DP;
  graphene_rect_t box;
  graphene_size_t round = GRAPHENE_SIZE_INIT (corner, corner), square = GRAPHENE_SIZE_INIT (0, 0);
  GskRoundedRect shape;
  GdkRGBA fill = ct_rgba (CT_C_FASTSCROLL_THUMB_PRESSED), white = ct_rgba (CT_C_WHITE);

  if (!self->preview)
    return;
  pango_layout_get_extents (self->preview, NULL, &logical);
  w = MAX (logical.width / (double) PANGO_SCALE + 2 * CT_FS_PREVIEW_PAD_H_DP, CT_FS_PREVIEW_MIN_W_DP);
  h = MAX (text_box_height (self->preview, PREVIEW_STYLE), CT_FS_PREVIEW_MIN_H_DP);
  right = width - CT_FS_TRACK_W_DP - CT_FS_PREVIEW_INSET_DP;
  bottom = MAX (thumb_top (self, CT_FS_THUMB_H_DP), h);
  box = GRAPHENE_RECT_INIT ((float) (right - w), (float) (bottom - h), (float) w, (float) h);
  gsk_rounded_rect_init (&shape, &box, &round, &round, &square, &round);
  gtk_snapshot_push_rounded_clip (snapshot, &shape);
  gtk_snapshot_append_color (snapshot, &fill, &box);
  gtk_snapshot_pop (snapshot);
  text_draw (snapshot, self->preview, PREVIEW_STYLE, box.origin.x + (w - logical.width / (double) PANGO_SCALE) / 2,
             box.origin.y + (h - text_box_height (self->preview, PREVIEW_STYLE)) / 2, &white);
}

static void
draw_bars (CtList *self, GtkSnapshot *snapshot)
{
  double width = gtk_widget_get_width (GTK_WIDGET (self)), height = gtk_widget_get_height (GTK_WIDGET (self));
  GtkAdjustment *adj = vadjustment (self);

  if (self->bar_alpha <= 0)
    return;
  gtk_snapshot_push_opacity (snapshot, self->bar_alpha);
  if (fast_scroller_enabled (self))
    {
      /* the track the full height at the end, the thumb on it */
      GdkRGBA track = ct_rgba (CT_C_FASTSCROLL_TRACK);
      GdkRGBA thumb = ct_rgba (self->dragging ? CT_C_FASTSCROLL_THUMB_PRESSED : CT_C_FASTSCROLL_THUMB);
      double top = thumb_top (self, CT_FS_THUMB_H_DP);

      gtk_snapshot_append_color (snapshot, &track,
                                 &GRAPHENE_RECT_INIT ((float) (width - CT_FS_TRACK_W_DP), 0, CT_FS_TRACK_W_DP,
                                                      (float) height));
      gtk_snapshot_append_color (snapshot, &thumb,
                                 &GRAPHENE_RECT_INIT ((float) (width - CT_FS_THUMB_W_DP), (float) top,
                                                      CT_FS_THUMB_W_DP, CT_FS_THUMB_H_DP));
      if (self->dragging)
        draw_preview (self, snapshot, width, CT_FS_THUMB_H_DP);
    }
  else if (gtk_adjustment_get_upper (adj) > gtk_adjustment_get_page_size (adj) + 1)
    {
      /* the plain scroll bar: thin, as long as the view is of the list */
      GdkRGBA color = ct_rgba (CT_C_SCROLLBAR);
      double length = MAX (height * gtk_adjustment_get_page_size (adj) / gtk_adjustment_get_upper (adj),
                           2 * CT_SCROLLBAR_W_DP);
      GskRoundedRect shape;
      graphene_rect_t bar = GRAPHENE_RECT_INIT ((float) (width - CT_SCROLLBAR_W_DP), (float) thumb_top (self, length),
                                                CT_SCROLLBAR_W_DP, (float) length);

      gsk_rounded_rect_init_from_rect (&shape, &bar, CT_SCROLLBAR_W_DP / 2);
      gtk_snapshot_push_rounded_clip (snapshot, &shape);
      gtk_snapshot_append_color (snapshot, &color, &bar);
      gtk_snapshot_pop (snapshot);
    }
  gtk_snapshot_pop (snapshot);
}

/* the list scrolls to put the row under @y of the track at the top, and the preview shows its section */
static void
drag_to (CtList *self, double y)
{
  GtkAdjustment *adj = vadjustment (self);
  double height = gtk_widget_get_height (GTK_WIDGET (self));
  double range = gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj);
  double proportion = CLAMP ((y - CT_FS_THUMB_H_DP / 2) / MAX (height - CT_FS_THUMB_H_DP, 1), 0, 1);
  const CtSection *section;
  int row;

  spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (self->scroll), proportion * range);
  row = row_at (self, MAX (offset (self), self->top_padding));
  section = row >= 0 ? ct_rows_section_at (self->rows, (guint) row) : NULL;
  /* the favourites' section has no label, so no preview */
  if (!section || !*section->label)
    g_clear_object (&self->preview);
  else if (g_strcmp0 (section->label, self->preview_label) != 0 || !self->preview)
    {
      g_clear_object (&self->preview);
      self->preview = text_layout (GTK_WIDGET (self), PREVIEW_STYLE, section->label, 0);
    }
  g_free (self->preview_label);
  self->preview_label = section ? g_strdup (section->label) : NULL;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_drag_begin (GtkGestureDrag *drag, double x, double y, CtList *self)
{
  double width = gtk_widget_get_width (GTK_WIDGET (self));
  double top = thumb_top (self, CT_FS_THUMB_H_DP);

  /* only on the thumb while it shows, with the touch slop around it */
  if (!fast_scroller_enabled (self) || self->bar_alpha <= 0 || x < width - CT_FS_THUMB_W_DP - CT_TOUCH_SLOP_DP * 2
      || y < top - CT_TOUCH_SLOP_DP || y > top + CT_FS_THUMB_H_DP + CT_TOUCH_SLOP_DP)
    {
      gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_DENIED);
      return;
    }
  gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_CLAIMED);
  self->dragging = TRUE;
  g_clear_handle_id (&self->bar_hide, g_source_remove);
  drag_to (self, y);
}

static void
on_drag_update (GtkGestureDrag *drag, double dx, double dy, CtList *self)
{
  double x, y;

  if (self->dragging && gtk_gesture_drag_get_start_point (drag, &x, &y))
    drag_to (self, y + dy);
}

static void
on_drag_end (GtkGestureDrag *drag, double dx, double dy, CtList *self)
{
  if (!self->dragging)
    return;
  self->dragging = FALSE;
  g_clear_object (&self->preview);
  show_bar (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* ===== CtList ================================================================================================= */

static void
ct_list_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                 int *minimum_baseline, int *natural_baseline)
{
  gtk_widget_measure (CT_LIST (widget)->scroll, orientation, for_size, minimum, natural, NULL, NULL);
}

static void
ct_list_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  gtk_widget_allocate (CT_LIST (widget)->scroll, width, height, -1, NULL);
}

static void
ct_list_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CtList *self = CT_LIST (widget);

  gtk_widget_snapshot_child (widget, self->scroll, snapshot);
  draw_bars (self, snapshot);
}

static void
on_load_anim (VibeosAnim *anim, double value, gpointer data)
{
  CtList *self = data;

  self->load_ms = value >= 1 ? -1 : value * (CT_LIST_LOAD_MS + 14 * LOAD_STAGGER);
  gtk_widget_queue_draw (self->content);
}

static void
ct_list_dispose (GObject *object)
{
  CtList *self = CT_LIST (object);

  clear_timeouts (self);
  g_clear_handle_id (&self->bar_hide, g_source_remove);
  for (int i = 0; i < N_PRESSES; i++)
    vibeos_ripple_clear (&self->presses[i].ripple);
  g_clear_pointer (&self->load_anim, vibeos_anim_free);
  g_clear_pointer (&self->bar_anim, vibeos_anim_free);
  g_clear_pointer (&self->scroll, gtk_widget_unparent);
  g_clear_pointer (&self->rows, ct_rows_free);
  g_clear_pointer (&self->texts, g_hash_table_unref);
  g_clear_pointer (&self->headers, g_hash_table_unref);
  g_clear_object (&self->preview);
  g_clear_pointer (&self->preview_label, g_free);
  G_OBJECT_CLASS (ct_list_parent_class)->dispose (object);
}

static void
ct_list_class_init (CtListClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = ct_list_dispose;
  widget_class->measure = ct_list_measure;
  widget_class->size_allocate = ct_list_size_allocate;
  widget_class->snapshot = ct_list_snapshot;
  gtk_widget_class_set_css_name (widget_class, "ct-list");
  signals[ROW_ACTIVATED] = g_signal_new ("row-activated", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                         NULL, NULL, G_TYPE_NONE, 1, G_TYPE_UINT);
  signals[ROW_LONG_PRESSED] = g_signal_new ("row-long-pressed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                            NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_UINT);
}

static void
ct_list_init (CtList *self)
{
  GtkGesture *click = gtk_gesture_click_new ();
  GtkGesture *drag = gtk_gesture_drag_new ();

  self->top_padding = CT_LIST_PAD_TOP_DP;
  self->down_row = -1;
  self->load_ms = -1;
  self->texts = g_hash_table_new_full (NULL, NULL, NULL, row_text_free);
  self->headers = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_object_unref);
  self->content = g_object_new (CT_TYPE_LIST_CONTENT, NULL);
  CT_LIST_CONTENT (self->content)->list = self;
  /* the ripples are drawn with the rows, so it is the content they redraw */
  for (int i = 0; i < N_PRESSES; i++)
    {
      self->presses[i].row = -1;
      vibeos_ripple_init (&self->presses[i].ripple, self->content);
    }
  self->load_anim = vibeos_anim_new (GTK_WIDGET (self), on_load_anim, self);
  self->bar_anim = vibeos_anim_new (GTK_WIDGET (self), on_bar_anim, self);
  self->scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (self->scroll), self->content);
  gtk_widget_set_parent (self->scroll, GTK_WIDGET (self));

  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), GDK_BUTTON_PRIMARY);
  g_signal_connect (click, "pressed", G_CALLBACK (on_pressed), self);
  g_signal_connect (click, "released", G_CALLBACK (on_released), self);
  g_signal_connect (click, "stopped", G_CALLBACK (on_stopped), self);
  g_signal_connect (click, "update", G_CALLBACK (on_update), self);
  g_signal_connect (click, "cancel", G_CALLBACK (on_lost), self);
  gtk_widget_add_controller (self->content, GTK_EVENT_CONTROLLER (click));

  /* before the scroll view's own drag, which is in the capture phase too, one level down */
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (drag), GTK_PHASE_CAPTURE);
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_drag_end), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));
}

GtkWidget *
ct_list_new (void)
{
  return g_object_new (CT_TYPE_LIST, NULL);
}

void
ct_list_set_rows (CtList *self, CtRows *rows, gboolean to_top)
{
  g_clear_pointer (&self->rows, ct_rows_free);
  self->rows = rows;
  g_hash_table_remove_all (self->texts);
  /* a press on a row that has gone is no press */
  for (int i = 0; i < N_PRESSES; i++)
    self->presses[i].row = -1;
  self->down_row = -1;
  clear_timeouts (self);
  configure (CT_LIST_CONTENT (self->content));
  if (to_top)
    spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (self->scroll), 0);
  gtk_widget_queue_draw (self->content);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

CtRows *
ct_list_get_rows (CtList *self)
{
  return self->rows;
}

CtEntry *
ct_list_get_entry (CtList *self, guint index)
{
  return index < n_rows (self) ? g_array_index (self->rows->rows, CtRow, index).entry : NULL;
}

void
ct_list_set_search (CtList *self, gboolean search)
{
  if (self->search == search)
    return;
  self->search = search;
  self->bar_alpha = 0;
  vibeos_anim_stop (self->bar_anim);
  g_hash_table_remove_all (self->texts);
  gtk_widget_queue_draw (self->content);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
ct_list_set_top_padding (CtList *self, double padding)
{
  if (self->top_padding == padding)
    return;
  self->top_padding = padding;
  configure (CT_LIST_CONTENT (self->content));
  gtk_widget_queue_draw (self->content);
}

void
ct_list_set_selection (CtList *self, GHashTable *selected)
{
  self->selected = selected;
  g_hash_table_remove_all (self->texts);
  gtk_widget_queue_draw (self->content);
}

void
ct_list_update_selection (CtList *self)
{
  gtk_widget_queue_draw (self->content);
}

void
ct_list_animate_load (CtList *self)
{
  self->load_ms = 0;
  vibeos_anim_start (self->load_anim, 0, 1, (guint) (CT_LIST_LOAD_MS + 14 * LOAD_STAGGER), 0, SPONGY_LINEAR);
}

void
ct_list_scroll_to_top (CtList *self)
{
  spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (self->scroll), 0);
}
