/* TermView: see term-view.h */
#include "term-view.h"
#include "term-toolbar.h"

#include <glib/gi18n.h>
#include <math.h>
#include <spongy.h>
#include <string.h>
#include <vibeos.h>

#define SCROLLBACK_LINES  10000     /* GNOME Console's */
#define DEFAULT_FONT      "Monospace 11"
#define TOUCH_SLOP        8         /* px (dp): Android's */
#define LONG_PRESS_MS     500       /* the shell's TAP_MAX_MS: a lift after a long press is never the shell's tap */
#define MIN_FLING         50        /* px/s: Android's minimum and maximum fling velocities */
#define MAX_FLING         8000
#define HANDLE_RADIUS     11        /* a selection handle: a circle with its corner at the text */
#define HANDLE_TOUCH      24        /* the reach of a finger on it, from the circle's centre */
#define HIGHLIGHT_ALPHA   0.4f      /* the selected text's highlight: the accent at Android's textColorHighlight */

/* VTE's word characters for a double click (its default exceptions), with ':' for URLs */
#define WORD_CHARS        "-#%&+,./=?@\\_~:"

/* the 16 colours: the GNOME palette, as GNOME Console has them; the foreground and background are the theme's */
static const char *const palette_hex[16] = {
  "#241f31", "#c01c28", "#2ec27e", "#f5c211", "#1e78e4", "#9841bb", "#0ab9dc", "#c0bfbc",
  "#5e5c64", "#ed333b", "#57e389", "#f8e45c", "#51a1ff", "#c061cb", "#4fd2fd", "#f6f5f4",
};

/* a place between two cells: before column `col` (0 to the column count) of the absolute row `row` */
typedef struct {
  long row, col;
} Spot;

typedef enum {
  TOUCH_IDLE,
  TOUCH_PENDING,          /* one finger down, still: a tap or a long press to come */
  TOUCH_MOVED,            /* one finger past the slop */
  TOUCH_SELECT,           /* after a long press: the finger extends the selection */
  TOUCH_HANDLE,           /* a selection handle is being moved */
  TOUCH_SCROLL,           /* two fingers */
  TOUCH_DONE,             /* what the touch was for is over: the rest of it goes nowhere */
} TouchState;

typedef struct {
  GdkEventSequence *sequence;
  double x, y;
} Finger;

struct _TermView {
  VteTerminal     parent;
  TermSession    *session;
  long            rows, columns;          /* the size last given to the session */

  /* touch */
  TouchState      state;
  Finger          fingers[2];             /* the fingers followed: the first, and a second for scrolling */
  int             n_fingers;
  int             n_down;                 /* every finger down on the view */
  double          down_x, down_y;
  gboolean        no_tap;                 /* the touch stopped a fling: its lift is no tap */
  guint           long_press;

  /* scrolling */
  double          scroll_x, scroll_y;     /* the fingers' centre */
  double          wheel_rest;             /* px of travel not yet a wheel step */
  SpongyVelocity  velocity;
  SpongyFling     fling;
  float           fling_last;
  guint           fling_tick;

  /* the selection: [start, end), start before end */
  gboolean        selected;
  Spot            start, end;
  Spot            word_start, word_end;   /* the long-pressed word, which a drag extends from */
  int             handle;                 /* 1: the start's, 2: the end's, while moved */
  double          grab_dx, grab_dy;       /* from the finger to the handle's point */
  long            offset;                 /* VTE's adjustment value + offset = its scroll position, in rows */
  gboolean        offset_known;
  TermToolbar    *toolbar;
};

G_DEFINE_FINAL_TYPE (TermView, term_view, VTE_TYPE_TERMINAL)

static void show_toolbar (TermView *self);
static void hide_toolbar (TermView *self);

/* ---- geometry ------------------------------------------------------------------------------------------------- */

static GtkAdjustment *
vadjustment (TermView *self)
{
  return gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (self));
}

static double
cell_width (TermView *self)
{
  return MAX (1, vte_terminal_get_char_width (VTE_TERMINAL (self)));
}

static double
cell_height (TermView *self)
{
  return MAX (1, vte_terminal_get_char_height (VTE_TERMINAL (self)));
}

/* the absolute row at the top of the view, with the fraction a smooth scroll leaves (VTE's scroll_delta) */
static double
scroll_position (TermView *self)
{
  return gtk_adjustment_get_value (vadjustment (self)) + self->offset;
}

/* the first and last rows VTE keeps */
static long
first_row (TermView *self)
{
  return self->offset;
}

static long
last_row (TermView *self)
{
  return self->offset + (long) gtk_adjustment_get_upper (vadjustment (self)) - 1;
}

/* the view's y of the top of an absolute row, as VTE draws it (row_to_pixel) */
static double
row_y (TermView *self, long row)
{
  double ch = cell_height (self);
  return row * ch - round (scroll_position (self) * ch);
}

static long
row_at (TermView *self, double y)
{
  double ch = cell_height (self);
  long row = (long) floor ((y + round (scroll_position (self) * ch)) / ch);
  return CLAMP (row, first_row (self), last_row (self));
}

/* the place between cells nearest to (x, y) */
static Spot
spot_at (TermView *self, double x, double y)
{
  long columns = vte_terminal_get_column_count (VTE_TERMINAL (self));
  long col = lround (x / cell_width (self));

  return (Spot) { row_at (self, y), CLAMP (col, 0, columns) };
}

static int
spot_cmp (Spot a, Spot b)
{
  if (a.row != b.row)
    return a.row < b.row ? -1 : 1;
  return a.col < b.col ? -1 : a.col > b.col;
}

/* Finds the offset between VTE's adjustment and the rows of its buffer (term-view.h). The cursor's absolute row is
 * known (vte_terminal_get_cursor_position) and the cursor is on the screen, the last `rows` rows, which end at the
 * adjustment's upper bound: so the offset is one of `rows` values. For each, the rows the view would show are asked
 * for, and the one whose text begins the text VTE shows is it. When rows repeat (a blank screen), more than one may
 * fit; those show the same text, so any of them selects and copies the same. FALSE when none fits. */
static gboolean
offset_fits (TermView *self, long offset, double value, long rows, const char *shown)
{
  long ch = (long) cell_height (self);
  /* the top row as VTE finds it (pixel_to_row (0)): through its scroll position in whole pixels */
  long top = lround ((value + offset) * ch) / ch;
  g_autofree char *text = NULL;

  if (offset < 0)
    return FALSE;
  text = vte_terminal_get_text_range_format (VTE_TERMINAL (self), VTE_FORMAT_TEXT, top, 0, top + rows - 1, 0, NULL);
  return text && g_str_has_prefix (shown, text);
}

static gboolean
update_offset (TermView *self)
{
  VteTerminal *terminal = VTE_TERMINAL (self);
  GtkAdjustment *adj = vadjustment (self);
  double value = gtk_adjustment_get_value (adj);
  long upper = (long) gtk_adjustment_get_upper (adj);
  long rows = vte_terminal_get_row_count (terminal);
  long cursor_row = 0;
  g_autofree char *shown = vte_terminal_get_text_format (terminal, VTE_FORMAT_TEXT);

  if (!shown)
    return FALSE;
  vte_terminal_get_cursor_position (terminal, NULL, &cursor_row);
  if (self->offset_known && offset_fits (self, self->offset, value, rows, shown))
    return TRUE;
  /* the cursor on the screen's last row first, then up */
  for (long screen_row = rows - 1; screen_row >= 0; screen_row--) {
    long offset = cursor_row - screen_row + rows - upper;
    if (offset_fits (self, offset, value, rows, shown)) {
      self->offset = offset;
      self->offset_known = TRUE;
      return TRUE;
    }
  }
  return FALSE;
}

/* ---- the selection -------------------------------------------------------------------------------------------- */

static char *
cell_text (TermView *self, long row, long col)
{
  return vte_terminal_get_text_range_format (VTE_TERMINAL (self), VTE_FORMAT_TEXT, row, col, row, col + 1, NULL);
}

static gunichar
cell_char (TermView *self, long row, long col)
{
  g_autofree char *text = cell_text (self, row, col);
  return text && *text ? g_utf8_get_char (text) : 0;
}

static gboolean
is_word_char (gunichar c)
{
  return c && (g_unichar_isalnum (c) || g_unichar_ismark (c) || (c < 128 && strchr (WORD_CHARS, (int) c)));
}

/* The word around the cell, along its row: [start, end). A wide character's second cell reads as empty, so a cell
 * that is empty after a wide one is taken for its half. The one cell when it is no word's. */
static void
word_at (TermView *self, long row, long col, Spot *start, Spot *end)
{
  long columns = vte_terminal_get_column_count (VTE_TERMINAL (self));
  long a, b;
  gunichar c = cell_char (self, row, col);

  if (!c && col > 0 && g_unichar_iswide (cell_char (self, row, col - 1))) {
    col--;
    c = cell_char (self, row, col);
  }
  a = col;
  b = col + (g_unichar_iswide (c) ? 2 : 1);
  if (is_word_char (c)) {
    while (a > 0) {
      gunichar left = cell_char (self, row, a - 1);
      if (is_word_char (left))
        a--;
      else if (!left && a > 1 && g_unichar_iswide (left = cell_char (self, row, a - 2)) && is_word_char (left))
        a -= 2;
      else
        break;
    }
    while (b < columns) {
      gunichar right = cell_char (self, row, b);
      if (!is_word_char (right))
        break;
      b += g_unichar_iswide (right) ? 2 : 1;
    }
  }
  *start = (Spot) { row, a };
  *end = (Spot) { row, MIN (b, columns) };
}

static void
set_selection (TermView *self, Spot start, Spot end)
{
  self->selected = TRUE;
  self->start = start;
  self->end = end;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

gboolean
term_view_end_selection (TermView *self)
{
  g_return_val_if_fail (TERM_IS_VIEW (self), FALSE);

  hide_toolbar (self);
  if (!self->selected)
    return FALSE;
  self->selected = FALSE;
  self->handle = 0;
  if (self->state == TOUCH_SELECT || self->state == TOUCH_HANDLE)
    self->state = TOUCH_DONE;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  return TRUE;
}

/* the point a handle stands at: the foot of the selection's start, or of its end */
static graphene_point_t
handle_point (TermView *self, int handle)
{
  Spot s = handle == 1 ? self->start : self->end;
  return GRAPHENE_POINT_INIT (s.col * cell_width (self), row_y (self, s.row) + cell_height (self));
}

/* the handle's circle: under the point, to its left for the start, to its right for the end; inside the view, so
 * a selection from the first column or to the last one keeps a handle to take */
static graphene_point_t
handle_centre (TermView *self, int handle)
{
  graphene_point_t p = handle_point (self, handle);
  double width = gtk_widget_get_width (GTK_WIDGET (self));
  double x = p.x + (handle == 1 ? -HANDLE_RADIUS : HANDLE_RADIUS);

  return GRAPHENE_POINT_INIT (CLAMP (x, HANDLE_RADIUS, MAX (HANDLE_RADIUS, width - HANDLE_RADIUS)),
                              p.y + HANDLE_RADIUS);
}

static int
handle_at (TermView *self, double x, double y)
{
  int found = 0;
  double best = HANDLE_TOUCH;

  if (!self->selected)
    return 0;
  for (int h = 1; h <= 2; h++) {
    graphene_point_t c = handle_centre (self, h);
    double d = hypot (x - c.x, y - c.y);
    if (d <= best) {
      best = d;
      found = h;
    }
  }
  return found;
}

/* the finger moved a handle: its end follows, the other stays, and past the other one the two swap */
static void
move_handle (TermView *self, double x, double y)
{
  graphene_point_t p = GRAPHENE_POINT_INIT (x + self->grab_dx, y + self->grab_dy);
  /* the point is at the row's foot: the row is the one above it */
  Spot s = spot_at (self, p.x, p.y - cell_height (self) / 2);
  Spot other = self->handle == 1 ? self->end : self->start;
  int cmp = spot_cmp (s, other);

  if (cmp == 0)
    return;   /* never empty */
  if (cmp < 0) {
    self->handle = 1;
    set_selection (self, s, other);
  } else {
    self->handle = 2;
    set_selection (self, other, s);
  }
}

/* after a long press: the word, and from it to the finger */
static void
extend_selection (TermView *self, double x, double y)
{
  Spot s = spot_at (self, x, y);

  if (spot_cmp (s, self->word_end) > 0)
    set_selection (self, self->word_start, s);
  else if (spot_cmp (s, self->word_start) < 0)
    set_selection (self, s, self->word_end);
  else
    set_selection (self, self->word_start, self->word_end);
}

static void
copy_selection (TermView *self)
{
  g_autofree char *text = NULL;

  if (!self->selected)
    return;
  text = vte_terminal_get_text_range_format (VTE_TERMINAL (self), VTE_FORMAT_TEXT, self->start.row, self->start.col,
                                             self->end.row, self->end.col, NULL);
  if (text && *text)
    gdk_clipboard_set_text (gtk_widget_get_clipboard (GTK_WIDGET (self)), text);
}

void
term_view_copy (TermView *self)
{
  g_return_if_fail (TERM_IS_VIEW (self));

  if (!self->selected) {
    /* (VTE copies an empty string when nothing is selected: the clipboard keeps what it had) */
    if (vte_terminal_get_has_selection (VTE_TERMINAL (self)))
      vte_terminal_copy_clipboard_format (VTE_TERMINAL (self), VTE_FORMAT_TEXT);
    return;
  }
  copy_selection (self);
  term_view_end_selection (self);
}

void
term_view_paste (TermView *self)
{
  g_return_if_fail (TERM_IS_VIEW (self));

  term_view_end_selection (self);
  /* VTE reads the clipboard and sends it as a paste (bracketed when the program asked), through ::commit */
  vte_terminal_paste_clipboard (VTE_TERMINAL (self));
}

void
term_view_select_all (TermView *self)
{
  g_return_if_fail (TERM_IS_VIEW (self));

  if (!update_offset (self))
    return;
  set_selection (self, (Spot) { first_row (self), 0 },
                 (Spot) { last_row (self), vte_terminal_get_column_count (VTE_TERMINAL (self)) });
  show_toolbar (self);
}

static void
on_copy (GtkWidget *button, TermView *self)
{
  term_view_copy (self);
}

static void
on_paste (GtkWidget *button, TermView *self)
{
  term_view_paste (self);
}

static void
on_select_all (GtkWidget *button, TermView *self)
{
  term_view_select_all (self);
}

/* the selection's part on the view, with the handles under it; FALSE when none of it is on the view */
static gboolean
selection_bounds (TermView *self, graphene_rect_t *rect)
{
  double ch = cell_height (self), cw = cell_width (self);
  double height = gtk_widget_get_height (GTK_WIDGET (self));
  double top = row_y (self, self->start.row), bottom = row_y (self, self->end.row) + ch + 2 * HANDLE_RADIUS;
  double left = 0, right = vte_terminal_get_column_count (VTE_TERMINAL (self)) * cw;

  if (bottom <= 0 || top >= height)
    return FALSE;
  if (self->start.row == self->end.row) {
    left = self->start.col * cw;
    right = self->end.col * cw;
  }
  top = MAX (top, 0);
  bottom = MIN (bottom, height);
  graphene_rect_init (rect, left, top, right - left, bottom - top);
  return TRUE;
}

static void
show_toolbar (TermView *self)
{
  graphene_rect_t rect;

  if (!self->selected || !vibeos_window_get_overlay (GTK_WIDGET (self)))
    return;
  if (!self->toolbar) {
    self->toolbar = g_object_ref_sink (term_toolbar_new ());
    g_signal_connect (term_toolbar_add (self->toolbar, _("Copy")), "clicked", G_CALLBACK (on_copy), self);
    g_signal_connect (term_toolbar_add (self->toolbar, _("Paste")), "clicked", G_CALLBACK (on_paste), self);
    g_signal_connect (term_toolbar_add (self->toolbar, _("Select all")), "clicked", G_CALLBACK (on_select_all), self);
  }
  if (selection_bounds (self, &rect))
    term_toolbar_show_at (self->toolbar, GTK_WIDGET (self), &rect);
  else
    term_toolbar_hide (self->toolbar);
}

static void
hide_toolbar (TermView *self)
{
  if (self->toolbar)
    term_toolbar_hide (self->toolbar);
}

/* ---- scrolling ------------------------------------------------------------------------------------------------ */

/* The fingers' travel, `px` toward the end of the text (the content going up), as a wheel's: TRUE while it moved
 * something, FALSE at an end of the history. */
static gboolean
scroll_by (TermView *self, double px)
{
  const TermModes *modes = self->session ? term_session_get_modes (self->session) : NULL;
  double ch = cell_height (self);

  if (!modes || term_modes_scroll (modes) == TERM_SCROLL_HISTORY) {
    GtkAdjustment *adj = vadjustment (self);
    double value = gtk_adjustment_get_value (adj);
    double max = gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj);
    double target = CLAMP (value + px / ch, gtk_adjustment_get_lower (adj), MAX (max, 0));

    if (fabs (target - value) < 1e-6)
      return FALSE;
    gtk_adjustment_set_value (adj, target);
    return TRUE;
  }

  /* a step per row of travel, at the cell under the fingers' centre */
  self->wheel_rest += px;
  while (fabs (self->wheel_rest) >= ch) {
    gboolean down = self->wheel_rest > 0;
    long columns = vte_terminal_get_column_count (VTE_TERMINAL (self));
    long rows = vte_terminal_get_row_count (VTE_TERMINAL (self));
    long col = CLAMP ((long) floor (self->scroll_x / cell_width (self)), 0, columns - 1);
    long row = CLAMP ((long) floor (self->scroll_y / ch), 0, rows - 1);
    char buf[32];
    gsize n = term_modes_wheel (modes, down, col, row, buf, sizeof buf);

    self->wheel_rest -= down ? ch : -ch;
    if (n)
      term_session_write (self->session, buf, n);
  }
  return TRUE;
}

static gboolean
stop_fling (TermView *self)
{
  if (!self->fling_tick)
    return FALSE;
  gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->fling_tick);
  self->fling_tick = 0;
  show_toolbar (self);
  return TRUE;
}

static gboolean
on_fling_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  TermView *self = data;
  double now = gdk_frame_clock_get_frame_time (clock) / 1000.0;
  gboolean running = spongy_fling_update (&self->fling, now);
  float travel = self->fling.position - self->fling_last;

  self->fling_last = self->fling.position;
  if (scroll_by (self, travel) && running)
    return G_SOURCE_CONTINUE;
  self->fling_tick = 0;
  show_toolbar (self);
  return G_SOURCE_REMOVE;
}

static void
fingers_centre (TermView *self, double *x, double *y)
{
  *x = (self->fingers[0].x + self->fingers[1].x) / 2;
  *y = (self->fingers[0].y + self->fingers[1].y) / 2;
}

static void
scroll_begin (TermView *self, guint32 time)
{
  self->state = TOUCH_SCROLL;
  hide_toolbar (self);
  fingers_centre (self, &self->scroll_x, &self->scroll_y);
  self->wheel_rest = 0;
  spongy_velocity_clear (&self->velocity);
  spongy_velocity_add (&self->velocity, time, self->scroll_x, self->scroll_y);
}

static void
scroll_update (TermView *self, guint32 time)
{
  double x, y, dy;

  fingers_centre (self, &x, &y);
  dy = y - self->scroll_y;
  self->scroll_x = x;
  self->scroll_y = y;
  spongy_velocity_add (&self->velocity, time, x, y);
  /* the content follows the fingers: fingers going up take the view toward the end */
  scroll_by (self, -dy);
}

static void
scroll_end (TermView *self, guint32 time, gboolean lifted)
{
  float vx, vy;
  GdkFrameClock *clock = gtk_widget_get_frame_clock (GTK_WIDGET (self));

  spongy_velocity_get (&self->velocity, time, &vx, &vy);
  if (!lifted || !clock || fabsf (vy) < MIN_FLING) {
    show_toolbar (self);
    return;
  }
  vy = CLAMP (vy, -MAX_FLING, MAX_FLING);
  spongy_fling_start (&self->fling, 0, -vy, -INFINITY, INFINITY, gdk_frame_clock_get_frame_time (clock) / 1000.0);
  self->fling_last = 0;
  self->fling_tick = gtk_widget_add_tick_callback (GTK_WIDGET (self), on_fling_tick, self, NULL);
}

/* ---- touch ---------------------------------------------------------------------------------------------------- */

static void
cancel_long_press (TermView *self)
{
  g_clear_handle_id (&self->long_press, g_source_remove);
}

static gboolean
on_long_press (gpointer data)
{
  TermView *self = data;
  long columns = vte_terminal_get_column_count (VTE_TERMINAL (self));

  self->long_press = 0;
  if (self->state != TOUCH_PENDING)
    return G_SOURCE_REMOVE;
  if (!update_offset (self)) {
    self->state = TOUCH_DONE;
    return G_SOURCE_REMOVE;
  }
  hide_toolbar (self);
  word_at (self, row_at (self, self->down_y),
           CLAMP ((long) floor (self->down_x / cell_width (self)), 0, columns - 1),
           &self->word_start, &self->word_end);
  set_selection (self, self->word_start, self->word_end);
  self->state = TOUCH_SELECT;
  return G_SOURCE_REMOVE;
}

/* a tap: what VTE's own click would have done, the focus and, for a program that tracks the mouse, the click */
static void
tap (TermView *self, double x, double y)
{
  const TermModes *modes = self->session ? term_session_get_modes (self->session) : NULL;

  gtk_widget_grab_focus (GTK_WIDGET (self));
  if (term_view_end_selection (self))
    return;
  if (modes && term_modes_tracks_mouse (modes)) {
    GtkAdjustment *adj = vadjustment (self);
    double back = gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj) - gtk_adjustment_get_value (adj);
    long columns = vte_terminal_get_column_count (VTE_TERMINAL (self));
    long rows = vte_terminal_get_row_count (VTE_TERMINAL (self));
    char buf[64];
    gsize n;

    /* none on the history, as VTE sends none there */
    if (back >= 0.5)
      return;
    n = term_modes_click (modes, CLAMP ((long) floor (x / cell_width (self)), 0, columns - 1),
                          CLAMP ((long) floor (y / cell_height (self)), 0, rows - 1), buf, sizeof buf);
    if (n)
      term_session_write (self->session, buf, n);
  }
}

static int
finger_index (TermView *self, GdkEventSequence *sequence)
{
  for (int i = 0; i < self->n_fingers; i++)
    if (self->fingers[i].sequence == sequence)
      return i;
  return -1;
}

static void
touch_begin (TermView *self, GdkEventSequence *sequence, double x, double y, guint32 time)
{
  if (self->n_down++ == 0) {
    self->no_tap = stop_fling (self);
    self->fingers[0] = (Finger) { sequence, x, y };
    self->n_fingers = 1;
    self->down_x = x;
    self->down_y = y;
    self->handle = handle_at (self, x, y);
    if (self->handle) {
      graphene_point_t p = handle_point (self, self->handle);
      self->grab_dx = p.x - x;
      self->grab_dy = p.y - y;
      self->state = TOUCH_HANDLE;
      hide_toolbar (self);
      return;
    }
    self->state = TOUCH_PENDING;
    self->long_press = g_timeout_add (LONG_PRESS_MS, on_long_press, self);
    return;
  }
  /* a second finger while the first was a tap to be, or moving: the two scroll */
  if (self->n_fingers == 1 && (self->state == TOUCH_PENDING || self->state == TOUCH_MOVED)) {
    cancel_long_press (self);
    self->fingers[1] = (Finger) { sequence, x, y };
    self->n_fingers = 2;
    scroll_begin (self, time);
  }
  /* any further finger is not followed */
}

static void
touch_update (TermView *self, GdkEventSequence *sequence, double x, double y, guint32 time)
{
  int i = finger_index (self, sequence);

  if (i < 0)
    return;
  self->fingers[i].x = x;
  self->fingers[i].y = y;
  switch (self->state) {
  case TOUCH_PENDING:
    if (hypot (x - self->down_x, y - self->down_y) > TOUCH_SLOP) {
      cancel_long_press (self);
      self->state = TOUCH_MOVED;
    }
    break;
  case TOUCH_SELECT:
    extend_selection (self, x, y);
    break;
  case TOUCH_HANDLE:
    move_handle (self, x, y);
    break;
  case TOUCH_SCROLL:
    scroll_update (self, time);
    break;
  default:
    break;
  }
}

static void
touch_end (TermView *self, GdkEventSequence *sequence, double x, double y, guint32 time, gboolean lifted)
{
  int i = finger_index (self, sequence);

  self->n_down = MAX (0, self->n_down - 1);
  if (i >= 0) {
    switch (self->state) {
    case TOUCH_PENDING:
      cancel_long_press (self);
      if (lifted && !self->no_tap)
        tap (self, x, y);
      break;
    case TOUCH_SELECT:
    case TOUCH_HANDLE:
      self->handle = 0;
      show_toolbar (self);
      break;
    case TOUCH_SCROLL:
      scroll_end (self, time, lifted);
      break;
    default:
      break;
    }
    self->state = TOUCH_DONE;
    if (i == 0 && self->n_fingers == 2)
      self->fingers[0] = self->fingers[1];
    self->n_fingers--;
  }
  if (self->n_down == 0) {
    cancel_long_press (self);
    self->state = TOUCH_IDLE;
    self->n_fingers = 0;
  }
}

static gboolean
on_event (GtkEventControllerLegacy *legacy, GdkEvent *event, TermView *self)
{
  GdkEventType type = gdk_event_get_event_type (event);
  GtkNative *native;
  double ex, ey, sx, sy;
  graphene_point_t p;

  if (type != GDK_TOUCH_BEGIN && type != GDK_TOUCH_UPDATE && type != GDK_TOUCH_END && type != GDK_TOUCH_CANCEL)
    return GDK_EVENT_PROPAGATE;         /* the mouse, the touchpad and the keys are VTE's */

  /* the event's surface coordinates, in the view's */
  native = gtk_widget_get_native (GTK_WIDGET (self));
  gdk_event_get_position (event, &ex, &ey);
  gtk_native_get_surface_transform (native, &sx, &sy);
  if (!gtk_widget_compute_point (GTK_WIDGET (native), GTK_WIDGET (self), &GRAPHENE_POINT_INIT (ex - sx, ey - sy), &p))
    p = GRAPHENE_POINT_INIT (0, 0);

  switch ((int) type) {
  case GDK_TOUCH_BEGIN:
    touch_begin (self, gdk_event_get_event_sequence (event), p.x, p.y, gdk_event_get_time (event));
    break;
  case GDK_TOUCH_UPDATE:
    touch_update (self, gdk_event_get_event_sequence (event), p.x, p.y, gdk_event_get_time (event));
    break;
  default:
    touch_end (self, gdk_event_get_event_sequence (event), p.x, p.y, gdk_event_get_time (event),
               type == GDK_TOUCH_END);
    break;
  }
  return GDK_EVENT_STOP;
}

/* ---- the terminal --------------------------------------------------------------------------------------------- */

static void
apply_colors (TermView *self)
{
  GdkRGBA palette[16];
  GdkRGBA fg = vibeos_color (VIBEOS_ON_SURFACE), bg = vibeos_color (VIBEOS_SURFACE);

  for (int i = 0; i < 16; i++)
    gdk_rgba_parse (&palette[i], palette_hex[i]);
  vte_terminal_set_colors (VTE_TERMINAL (self), &fg, &bg, palette, G_N_ELEMENTS (palette));
}

static void
on_theme_changed (VibeosTheme *theme, TermView *self)
{
  apply_colors (self);
}

/* GNOME's monospace font, as GNOME Console uses it */
static void
apply_font (TermView *self)
{
  GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
  g_autoptr (GSettingsSchema) schema = source ? g_settings_schema_source_lookup (source, "org.gnome.desktop.interface",
                                                                                 TRUE) : NULL;
  g_autofree char *name = NULL;
  PangoFontDescription *font;

  if (schema && g_settings_schema_has_key (schema, "monospace-font-name")) {
    g_autoptr (GSettings) settings = g_settings_new ("org.gnome.desktop.interface");
    name = g_settings_get_string (settings, "monospace-font-name");
  }
  font = pango_font_description_from_string (name && *name ? name : DEFAULT_FONT);
  vte_terminal_set_font (VTE_TERMINAL (self), font);
  pango_font_description_free (font);
}

static void
on_commit (VteTerminal *terminal, const char *text, guint size, TermView *self)
{
  /* typing (or a paste) is done with the selection */
  term_view_end_selection (self);
}

static void
on_contents_changed (VteTerminal *terminal, TermView *self)
{
  if (!self->selected)
    return;
  update_offset (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  if (self->toolbar && term_toolbar_is_shown (self->toolbar))
    show_toolbar (self);
}

static void
on_scrolled (GtkAdjustment *adj, TermView *self)
{
  if (!self->selected)
    return;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  if (self->toolbar && term_toolbar_is_shown (self->toolbar))
    show_toolbar (self);
}

static void
term_view_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  TermView *self = TERM_VIEW (widget);
  long rows, columns;

  GTK_WIDGET_CLASS (term_view_parent_class)->size_allocate (widget, width, height, baseline);
  /* VTE sizes its grid to the allocation; the program learns it from the pty */
  rows = vte_terminal_get_row_count (VTE_TERMINAL (self));
  columns = vte_terminal_get_column_count (VTE_TERMINAL (self));
  if (rows == self->rows && columns == self->columns)
    return;
  self->rows = rows;
  self->columns = columns;
  if (self->session)
    term_session_set_size (self->session, rows, columns);
  /* VTE rewraps its lines for the new width: the selection's rows are no longer where they were */
  term_view_end_selection (self);
  self->offset_known = FALSE;
}

static void
term_view_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  TermView *self = TERM_VIEW (widget);
  double cw, ch, height;
  long columns, first, last;
  GdkRGBA highlight, accent;

  GTK_WIDGET_CLASS (term_view_parent_class)->snapshot (widget, snapshot);
  if (!self->selected)
    return;

  cw = cell_width (self);
  ch = cell_height (self);
  height = gtk_widget_get_height (widget);
  columns = vte_terminal_get_column_count (VTE_TERMINAL (self));
  highlight = vibeos_color_alpha (VIBEOS_PRIMARY, HIGHLIGHT_ALPHA);
  accent = vibeos_color (VIBEOS_PRIMARY);
  first = MAX (self->start.row, row_at (self, 0));
  last = MIN (self->end.row, row_at (self, height));
  for (long row = first; row <= last; row++) {
    long a = row == self->start.row ? self->start.col : 0;
    long b = row == self->end.row ? self->end.col : columns;
    if (b > a)
      gtk_snapshot_append_color (snapshot, &highlight,
                                 &GRAPHENE_RECT_INIT (a * cw, row_y (self, row), (b - a) * cw, ch));
  }
  /* the handles: a circle each, with a square corner at the selection's end */
  for (int h = 1; h <= 2; h++) {
    graphene_point_t p = handle_point (self, h), c = handle_centre (self, h);
    g_autoptr (GskPathBuilder) builder = NULL;
    g_autoptr (GskPath) path = NULL;

    if (p.y < 0 || p.y > height)
      continue;
    builder = gsk_path_builder_new ();
    gsk_path_builder_add_circle (builder, &c, HANDLE_RADIUS);
    path = gsk_path_builder_to_path (builder);
    gtk_snapshot_append_fill (snapshot, path, GSK_FILL_RULE_WINDING, &accent);
    gtk_snapshot_append_color (snapshot, &accent,
                               &GRAPHENE_RECT_INIT (MIN (p.x, c.x), p.y, HANDLE_RADIUS, HANDLE_RADIUS));
  }
}

static void
term_view_dispose (GObject *object)
{
  TermView *self = TERM_VIEW (object);

  cancel_long_press (self);
  stop_fling (self);
  if (self->toolbar) {
    GtkWidget *overlay = gtk_widget_get_parent (GTK_WIDGET (self->toolbar));
    if (overlay)
      gtk_overlay_remove_overlay (GTK_OVERLAY (overlay), GTK_WIDGET (self->toolbar));
    g_clear_object (&self->toolbar);
  }
  g_clear_object (&self->session);
  G_OBJECT_CLASS (term_view_parent_class)->dispose (object);
}

static void
term_view_constructed (GObject *object)
{
  TermView *self = TERM_VIEW (object);

  G_OBJECT_CLASS (term_view_parent_class)->constructed (object);
  /* the adjustment VTE has once its construct properties are set (GtkScrollable's vadjustment is one) */
  g_signal_connect_object (vadjustment (self), "value-changed", G_CALLBACK (on_scrolled), self, 0);
}

static void
term_view_class_init (TermViewClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->constructed = term_view_constructed;
  G_OBJECT_CLASS (klass)->dispose = term_view_dispose;
  widget_class->size_allocate = term_view_size_allocate;
  widget_class->snapshot = term_view_snapshot;
}

static void
term_view_init (TermView *self)
{
  VteTerminal *terminal = VTE_TERMINAL (self);
  GtkEventController *legacy = gtk_event_controller_legacy_new ();

  vte_terminal_set_scrollback_lines (terminal, SCROLLBACK_LINES);
  /* the grid from the top left corner, as the selection's arithmetic takes it */
  vte_terminal_set_xalign (terminal, VTE_ALIGN_START);
  vte_terminal_set_yalign (terminal, VTE_ALIGN_START);
  vte_terminal_set_mouse_autohide (terminal, TRUE);
  apply_colors (self);
  apply_font (self);
  g_signal_connect_object (vibeos_theme_get_default (), "changed", G_CALLBACK (on_theme_changed), self, 0);
  g_signal_connect (terminal, "commit", G_CALLBACK (on_commit), self);
  g_signal_connect (terminal, "contents-changed", G_CALLBACK (on_contents_changed), self);

  /* every touch, before VTE's own controllers see it */
  gtk_event_controller_set_propagation_phase (legacy, GTK_PHASE_CAPTURE);
  g_signal_connect (legacy, "event", G_CALLBACK (on_event), self);
  gtk_widget_add_controller (GTK_WIDGET (self), legacy);
}

TermView *
term_view_new (void)
{
  return g_object_new (TERM_TYPE_VIEW, NULL);
}

void
term_view_set_session (TermView *self, TermSession *session)
{
  g_return_if_fail (TERM_IS_VIEW (self));

  g_set_object (&self->session, session);
  if (session && self->rows > 0)
    term_session_set_size (session, self->rows, self->columns);
}
