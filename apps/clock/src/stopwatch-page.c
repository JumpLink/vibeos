/* StopwatchPage: see stopwatch-page.h. Without laps the ring is centred over the buttons; with laps it sits at the
 * top of the tab, the laps under it. On a tablet held sideways the laps are beside it instead, at the end, and the
 * ring is centred in the room they leave. */
#include "stopwatch-page.h"
#include "clock-draw.h"
#include "clock-tab.h"
#include "elapsed.h"
#include "stopwatch-client.h"

#include <glib/gi18n.h>
#include <locale.h>
#include <math.h>
#include <spongy-gtk.h>

#define GUIDELINE       0.1     /* the ring between vertical guidelines at 10 % and 90 % of the width */
#define MAX_CIRCLE      320
#define TABLET_CIRCLE   240     /* on a tablet, upright or sideways */
#define SIDE_GUTTER     (4.0 / 104)     /* sideways: the gutter at each side, of the width */
#define SIDE_BOTTOM     8       /* sideways: under the laps (the buttons are in their column, not under them) */
#define STROKE          12      /* the ring's width, and twice the dot's radius */
#define FAB_ROW         120     /* the buttons' row: the tab's bottom padding, the list's, and its fade */
#define TIME_SIZE       72      /* at most: it shrinks to fit */
#define TIME_OUTER      30      /* before the time and after the hundredths */
#define TIME_INNER      3       /* after the time and before the hundredths */
#define ROW_HEIGHT      27      /* a lap: its line, with 4 over and under it */
#define RUNNING_PERIOD  25      /* ms between redraws while it runs */
#define PAUSED_PERIOD   500     /* … and while it is paused (the blink) */
#define TRANSITION_MS   300     /* each step of the laps' transition, one after the other */

#define TEN_MINUTES   (10 * 60 * 1000)
#define HOUR          (60 * 60 * 1000)
#define TEN_HOURS     (10 * HOUR)
#define HUNDRED_HOURS ((gint64) 100 * HOUR)
#define LRM_SPACE     "‎ "  /* the laps' separator: a space that keeps the digits left to right */

struct _StopwatchPage
{
  GtkWidget parent;
  VibeosSnackbar *snackbar;
  StopwatchClient *client;

  GtkWidget *main_text, *hundredths;            /* the time, centred in the ring (place_time) */
  GtkWidget *scroller, *laps;                   /* the laps, a row each, the one running first */
  graphene_rect_t ring;                         /* the time's box (stopwatch_time_wrapper) */

  guint update;                 /* the next update of the time */
  guint refit;                  /* the time fitted again to a new ring size */
  double fitted;                /* the ring size it was fitted to */
  gint64 shown_time;            /* the time last written */
  int lap_length, total_length; /* how long the laps' columns were written */
  guint rows;
  gboolean pressed;             /* the ring held down: the time accented */
  guint inhibit;                /* the screen kept on while it runs */

  /* the laps coming and going: the ring moving between the middle and
   * the top, the list fading */
  gboolean laps_shown;
  VibeosAnim *transition;
  double move, fade;            /* 0 in the middle .. 1 at the top; the list's opacity */
  double move_from, fade_from;
  double first_ms;              /* how long the first of the two steps takes */

  VibeosLayout *layout;
  gboolean tablet, sideways;    /* a tablet; held sideways: the laps beside the ring */
  graphene_rect_t laps_box;     /* where the laps are, for their fade */
  double laps_fade;             /* how tall the fade at their bottom is */
};

static void stopwatch_page_tab_init (ClockTabInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (StopwatchPage, stopwatch_page, GTK_TYPE_WIDGET,
                               G_IMPLEMENT_INTERFACE (CLOCK_TYPE_TAB, stopwatch_page_tab_init))

static const Stopwatch *
stopwatch (StopwatchPage *self)
{
  return stopwatch_client_get_stopwatch (self->client);
}

/* --- the time */

/* "1:02:03", "2:03" or "3" */
static char *
time_string (gint64 time)
{
  int hours = time / HOUR, minutes = time / 60000 % 60, seconds = time / 1000 % 60;

  if (hours)
    return g_strdup_printf ("%d:%02d:%02d", hours, minutes, seconds);
  if (minutes)
    return g_strdup_printf ("%d:%02d", minutes, seconds);
  return g_strdup_printf ("%d", seconds);
}

/* the time shrinks to fit beside the hundredths in the ring ("1:00:00" does not, at TIME_SIZE) */
static void
fit_main_text (StopwatchPage *self)
{
  int hundredths;

  gtk_widget_measure (self->hundredths, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &hundredths, NULL, NULL);
  clock_label_fit (self->main_text, TIME_SIZE, self->ring.size.width - hundredths - 2 * (TIME_OUTER + TIME_INNER));
}

static gboolean
on_refit (gpointer data)
{
  StopwatchPage *self = data;

  self->refit = 0;
  fit_main_text (self);
  return G_SOURCE_REMOVE;
}

/* the hundredths every time they change, the rest every second */
static void
show_time (StopwatchPage *self, gint64 total)
{
  g_autofree char *hundredths = NULL;

  if (self->shown_time / 10 == total / 10)
    return;
  hundredths = g_strdup_printf ("%02d", (int) (total % 1000 / 10));
  gtk_label_set_text (GTK_LABEL (self->hundredths), hundredths);
  if (self->shown_time / 1000 != total / 1000)
    {
      g_autofree char *text = time_string (total);

      gtk_label_set_text (GTK_LABEL (self->main_text), text);
      fit_main_text (self);
    }
  self->shown_time = total;
}

/* --- the laps */

/* hours and minutes as wide as `max` needs, then seconds and hundredths */
static char *
format_time (gint64 max, gint64 time, const char *separator)
{
  GString *text = g_string_new (NULL);
  gint64 hours = 0;
  int minutes = 0, seconds = 0, hundredths = 0;

  if (time > 0)
    {
      hours = time / HOUR;
      minutes = time % HOUR / 60000;
      seconds = time % 60000 / 1000;
      hundredths = time % 1000 / 10;
    }
  if (max < TEN_MINUTES)
    g_string_append_printf (text, "%d", minutes);
  else if (max < HOUR)
    g_string_append_printf (text, "%02d", minutes);
  else if (max < TEN_HOURS)
    g_string_append_printf (text, "%d%s%02d", (int) hours, separator, minutes);
  else if (max < HUNDRED_HOURS)
    g_string_append_printf (text, "%02d%s%02d", (int) hours, separator, minutes);
  else
    g_string_append_printf (text, "%03d%s%02d", (int) hours, separator, minutes);
  g_string_append_printf (text, "%s%02d%s%02d", separator, seconds, localeconv ()->decimal_point, hundredths);
  return g_string_free (text, FALSE);
}

/* the longest lap sets the lap column's format, the time on the stopwatch the total's */
static char *
format_lap_time (StopwatchPage *self, gint64 lap_time, gint64 now_total)
{
  gint64 longest = MAX (stopwatch_longest_lap_time (stopwatch (self), now_total), lap_time);

  return format_time (longest, lap_time, LRM_SPACE);
}

static char *
format_total (StopwatchPage *self, gint64 total, gint64 now_total)
{
  return format_time (MAX (now_total, total), total, LRM_SPACE);
}

static GtkWidget *
lap_label (const char *css_class, int margin_end)
{
  GtkWidget *label = gtk_label_new (NULL);

  gtk_widget_add_css_class (label, css_class);
  vibeos_label_set_font_padding_sp (label, 14);
  /* the row's padding of 4 on top of the font's own */
  gtk_widget_set_margin_top (label, gtk_widget_get_margin_top (label) + 4);
  gtk_widget_set_valign (label, GTK_ALIGN_START);
  gtk_widget_set_margin_end (label, margin_end);
  return label;
}

/* a lap: the number, the lap's own time, the total at its end, centred as a group */
static GtkWidget *
lap_row_new (void)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

  gtk_widget_set_halign (row, GTK_ALIGN_CENTER);
  gtk_widget_set_size_request (row, -1, ROW_HEIGHT);
  gtk_box_append (GTK_BOX (row), lap_label ("lap-number", 16));
  gtk_box_append (GTK_BOX (row), lap_label ("lap-time", 16));
  gtk_box_append (GTK_BOX (row), lap_label ("lap-time", 0));
  return row;
}

static void
set_row (GtkWidget *row, const char *number, const char *lap_time, const char *total)
{
  GtkWidget *label = gtk_widget_get_first_child (row);

  if (number)
    gtk_label_set_text (GTK_LABEL (label), number);
  label = gtk_widget_get_next_sibling (label);
  gtk_label_set_text (GTK_LABEL (label), lap_time);
  gtk_label_set_text (GTK_LABEL (gtk_widget_get_next_sibling (label)), total);
}

static void refresh_rows (StopwatchPage *self);

/* the top row follows the stopwatch; when that makes a column wider, every row is written again in the new
 * format */
static void
update_current_lap (StopwatchPage *self, gint64 total)
{
  const Stopwatch *sw = stopwatch (self);
  g_autofree char *lap_time = NULL, *total_text = NULL;
  int lap_length, total_length;

  if (!self->rows)
    return;
  lap_time = format_lap_time (self, stopwatch_current_lap_time (sw, total), total);
  total_text = format_total (self, total, total);
  lap_length = g_utf8_strlen (lap_time, -1);
  total_length = g_utf8_strlen (total_text, -1);
  if (lap_length != self->lap_length || total_length != self->total_length)
    {
      self->lap_length = lap_length;
      self->total_length = total_length;
      refresh_rows (self);
      return;
    }
  set_row (gtk_widget_get_first_child (self->laps), NULL, lap_time, total_text);
}

/* every row: "# 7" while there are fewer than ten, "# 07" from ten */
static void
refresh_rows (StopwatchPage *self)
{
  const Stopwatch *sw = stopwatch (self);
  gint64 now_total = stopwatch_total (sw, elapsed_now ());
  GtkWidget *row = gtk_widget_get_first_child (self->laps);

  for (guint position = 0; row; position++, row = gtk_widget_get_next_sibling (row))
    {
      guint number = position ? sw->laps->len - position + 1 : sw->laps->len + 1;
      gint64 lap_time = position ? stopwatch_lap_time (sw, number) : stopwatch_current_lap_time (sw, now_total);
      gint64 total = position ? g_array_index (sw->laps, gint64, number - 1) : now_total;
      g_autofree char *number_text = g_strdup_printf (self->rows < 10 ? _("# %d") : _("# %02d"), number);
      g_autofree char *lap_text = format_lap_time (self, lap_time, now_total);
      g_autofree char *total_text = format_total (self, total, now_total);

      set_row (row, number_text, lap_text, total_text);
    }
}

/* one row per lap and one for the lap running, built again when a lap is added */
static void
build_rows (StopwatchPage *self)
{
  guint laps = stopwatch (self)->laps->len;
  guint rows = laps ? laps + 1 : 0;
  GtkWidget *child;

  if (rows == self->rows)
    return;
  while ((child = gtk_widget_get_first_child (self->laps)))
    gtk_box_remove (GTK_BOX (self->laps), child);
  for (guint i = 0; i < rows; i++)
    gtk_box_append (GTK_BOX (self->laps), lap_row_new ());
  self->rows = rows;
  self->lap_length = self->total_length = 0;
  refresh_rows (self);
}

/* --- the laps coming and going */

static void
apply_transition (StopwatchPage *self)
{
  gtk_widget_set_opacity (self->scroller, self->fade);
  gtk_widget_set_child_visible (self->scroller, self->fade > 0);
  /* the old laps fade out as they were (the transition draws the list it captured); now they can go */
  if (!self->laps_shown && self->fade == 0)
    build_rows (self);
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

/* the transition, in order: what goes fades out, everything moves, what comes fades in, 300 ms each on the default
 * curve. Laps coming have nothing to fade out, so the ring rises and then they fade in; going, they fade out and
 * then the ring sinks back. */
static void
on_transition (VibeosAnim *anim, double value, gpointer data)
{
  StopwatchPage *self = data;
  double first = self->first_ms ? spongy_curve (SPONGY_ACCELERATE_DECELERATE, CLAMP (value / self->first_ms, 0, 1)) : 1;
  double second = spongy_curve (SPONGY_ACCELERATE_DECELERATE, CLAMP ((value - self->first_ms) / TRANSITION_MS, 0, 1));

  if (self->laps_shown)
    {
      self->move = self->move_from + (1 - self->move_from) * first;
      self->fade = self->fade_from + (1 - self->fade_from) * second;
    }
  else
    {
      self->fade = self->fade_from * (1 - first);
      self->move = self->move_from * (1 - second);
    }
  apply_transition (self);
}

/* the laps shown while there are any */
static void
show_or_hide_laps (StopwatchPage *self, gboolean shown)
{
  double total;

  if (shown == self->laps_shown)
    return;
  self->laps_shown = shown;
  self->move_from = self->move;
  self->fade_from = self->fade;
  self->first_ms = shown ? (self->move < 1 ? TRANSITION_MS : 0) : (self->fade > 0 ? TRANSITION_MS : 0);
  total = self->first_ms + TRANSITION_MS;
  vibeos_anim_start (self->transition, 0, total, total, 0, SPONGY_LINEAR);
}

/* --- the ring */

/* The plain ring; with laps, the first lap is the measure: the part of it the running lap has covered in the
 * accent, the rest plain, a white mark where the last lap ended, and a dot where this one is. */
static void
snapshot_ring (StopwatchPage *self, GtkSnapshot *snapshot)
{
  const Stopwatch *sw = stopwatch (self);
  int scale = vibeos_device_scale (GTK_WIDGET (self));
  /* the centre in whole device pixels */
  double half = floor (self->ring.size.width * scale / 2) / scale;
  double cx = self->ring.origin.x + half, cy = self->ring.origin.y + half;
  double radius = half - STROKE;
  GdkRGBA circle = vibeos_color (VIBEOS_SURFACE_CONTAINER_HIGHEST), completed = vibeos_color (VIBEOS_PRIMARY);
  GdkRGBA marker = vibeos_color (VIBEOS_WHITE);
  guint laps = sw->laps->len;
  gint64 total, first, last;
  double done, left;

  if (radius <= 0)
    return;
  if (laps == 0 || laps >= STOPWATCH_MAX_LAPS)
    {
      clock_snapshot_arcs (snapshot, scale, cx, cy, radius, STROKE, (ClockArc[]) { { 0, 360, circle } }, 1);
      return;
    }
  total = stopwatch_total (sw, elapsed_now ());
  first = MAX (g_array_index (sw->laps, gint64, 0), 1);
  last = g_array_index (sw->laps, gint64, laps - 1);
  done = (double) (total - last) / first;
  left = 1 - MIN (done, 1);
  clock_snapshot_arcs (snapshot, scale, cx, cy, radius, STROKE,
                       (ClockArc[]) {
                         { 270 + (1 - left) * 360, left * 360, circle },
                         { 270, done * 360, completed },
                         { 270 + (double) stopwatch_lap_time (sw, laps) / first * 360, 360 / (radius * G_PI), marker },
                       }, laps > 1 ? 3 : 2);
  clock_snapshot_dot (snapshot, cx + radius * cos ((270 + done * 360) * G_PI / 180),
                      cy + radius * sin ((270 + done * 360) * G_PI / 180), STROKE / 2.0, &completed);
}

/* --- updating */

static void
update_time (StopwatchPage *self)
{
  gint64 total = stopwatch_total (stopwatch (self), elapsed_now ());

  show_time (self, total);
  if (stopwatch (self)->state != STOPWATCH_RESET)
    update_current_lap (self, total);
}

/* the paused time blinks */
static void
set_time_opacity (StopwatchPage *self, double opacity)
{
  gtk_widget_set_opacity (self->main_text, opacity);
  gtk_widget_set_opacity (self->hundredths, opacity);
}

static gboolean
on_update (gpointer data)
{
  StopwatchPage *self = data;
  const Stopwatch *sw = stopwatch (self);
  gint64 start = elapsed_now ();
  /* paused, the time blinks: hidden for the first half of every second, pressed or not */
  gboolean blink = sw->state == STOPWATCH_PAUSED && start % 1000 < 500;

  self->update = 0;
  update_time (self);
  set_time_opacity (self, blink ? 0 : 1);
  /* the ring moves with the lap running, redrawn with the time, not every frame: a
   * frame costs this phone's GL driver more than the drawing in it */
  gtk_widget_queue_draw (GTK_WIDGET (self));
  if (sw->state != STOPWATCH_RESET)
    {
      int period = sw->state == STOPWATCH_PAUSED ? PAUSED_PERIOD : RUNNING_PERIOD;

      self->update = g_timeout_add (MAX (0, start + period - elapsed_now ()), on_update, self);
    }
  return G_SOURCE_REMOVE;
}

/* the screen stays on while it runs in front */
static void
adjust_wake_lock (StopwatchPage *self)
{
  GtkApplication *app = GTK_APPLICATION (g_application_get_default ());
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
  gboolean keep = stopwatch (self)->state == STOPWATCH_RUNNING && gtk_widget_get_mapped (GTK_WIDGET (self));

  if (keep && !self->inhibit && GTK_IS_WINDOW (root))
    self->inhibit = gtk_application_inhibit (app, GTK_WINDOW (root), GTK_APPLICATION_INHIBIT_IDLE,
                                             _("The stopwatch is running"));
  else if (!keep && self->inhibit)
    {
      gtk_application_uninhibit (app, self->inhibit);
      self->inhibit = 0;
    }
}

/* the wake lock, the time, the ring and the laps for the stopwatch's state */
static void
update_ui (StopwatchPage *self)
{
  const Stopwatch *sw = stopwatch (self);

  adjust_wake_lock (self);
  update_time (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_clear_handle_id (&self->update, g_source_remove);
  if (sw->state != STOPWATCH_RESET && gtk_widget_get_mapped (GTK_WIDGET (self)))
    on_update (self);
  else
    set_time_opacity (self, 1);
  show_or_hide_laps (self, sw->laps->len > 0);
}

static void
on_changed (StopwatchClient *client, StopwatchPage *self)
{
  guint rows = self->rows;

  /* rows going away stay while they fade out (apply_transition) */
  if (stopwatch (self)->laps->len || !self->laps_shown)
    build_rows (self);
  /* a new lap is scrolled to */
  if (self->rows > rows)
    spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (self->scroller), 0);
  update_ui (self);
  /* the FAB's icon changes at once, without a morph */
  clock_tab_update_buttons (CLOCK_TAB (self), CLOCK_ALL_IMMEDIATE);
}

/* --- a tap on the time */

/* only a touch inside the ring's circle is a tap on the time */
static void
on_pressed (GtkGestureClick *gesture, int n_press, double x, double y, StopwatchPage *self)
{
  double r = self->ring.size.width / 2;
  double dx = x - (self->ring.origin.x + r), dy = y - (self->ring.origin.y + r);

  if (dx * dx + dy * dy > r * r)
    {
      gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_DENIED);
      return;
    }
  self->pressed = TRUE;
  gtk_widget_add_css_class (self->main_text, "pressed");
  gtk_widget_add_css_class (self->hundredths, "pressed");
}

static void
unpress (StopwatchPage *self)
{
  self->pressed = FALSE;
  gtk_widget_remove_css_class (self->main_text, "pressed");
  gtk_widget_remove_css_class (self->hundredths, "pressed");
}

static void
toggle (StopwatchPage *self)
{
  if (stopwatch (self)->state == STOPWATCH_RUNNING)
    stopwatch_client_pause (self->client);
  else
    stopwatch_client_start (self->client);
}

static void
on_released (GtkGestureClick *gesture, int n_press, double x, double y, StopwatchPage *self)
{
  if (!self->pressed)
    return;
  unpress (self);
  toggle (self);
}

static void
on_stopped (GtkGesture *gesture, StopwatchPage *self)
{
  unpress (self);
}

/* --- sharing */

/* the times as text, on the clipboard: there is no share sheet */
static void
share (StopwatchPage *self)
{
  const Stopwatch *sw = stopwatch (self);
  gint64 total = stopwatch_total (sw, elapsed_now ());
  g_autofree char *time = format_time (total, total, ":");
  g_autofree char *separator = g_strconcat (localeconv ()->decimal_point, " ", NULL);
  g_autoptr(GString) text = g_string_new (NULL);

  g_string_append_printf (text, _("My time is %s"), time);
  g_string_append_c (text, '\n');
  if (sw->laps->len)
    {
      g_string_append (text, _("Lap times:"));
      g_string_append_c (text, '\n');
      for (guint number = 1; number <= sw->laps->len + 1; number++)
        {
          gint64 lap = number <= sw->laps->len ? stopwatch_lap_time (sw, number)
                                               : stopwatch_current_lap_time (sw, total);
          g_autofree char *lap_text = format_time (lap, lap, " ");

          g_string_append_printf (text, "%u%s%s\n", number, separator, lap_text);
        }
    }
  gdk_clipboard_set_text (gtk_widget_get_clipboard (GTK_WIDGET (self)), text->str);
  vibeos_snackbar_show (self->snackbar, _("Copied to clipboard"), NULL, NULL, NULL, VIBEOS_SNACKBAR_SHORT);
}

/* --- the buttons */

static void
stopwatch_page_get_buttons (ClockTab *tab, ClockButton *buttons)
{
  const Stopwatch *sw = stopwatch (STOPWATCH_PAGE (tab));

  if (sw->state == STOPWATCH_RUNNING)
    buttons[CLOCK_FAB] = (ClockButton) { "clock-play-pause-symbolic", _("Pause") };
  else
    buttons[CLOCK_FAB] = (ClockButton) { "clock-pause-play-symbolic", _("Start") };
  if (sw->state == STOPWATCH_RESET)
    return;
  buttons[CLOCK_LEFT] = (ClockButton) { "clock-delete-symbolic", _("Reset") };
  if (sw->state == STOPWATCH_PAUSED)
    buttons[CLOCK_RIGHT] = (ClockButton) { "clock-share-symbolic", _("Share") };
  else if (sw->laps->len < STOPWATCH_MAX_LAPS)
    buttons[CLOCK_RIGHT] = (ClockButton) { "clock-stopwatch-black-symbolic", _("Lap") };
}

static void
stopwatch_page_clicked (ClockTab *tab, ClockButtonPosition button)
{
  StopwatchPage *self = STOPWATCH_PAGE (tab);
  const Stopwatch *sw = stopwatch (self);

  if (button == CLOCK_FAB)
    toggle (self);
  else if (button == CLOCK_LEFT)
    stopwatch_client_reset (self->client);
  else if (sw->state == STOPWATCH_RUNNING)
    stopwatch_client_lap (self->client);
  else if (sw->state == STOPWATCH_PAUSED)
    share (self);
}

static void
stopwatch_page_tab_init (ClockTabInterface *iface)
{
  iface->get_buttons = stopwatch_page_get_buttons;
  iface->clicked = stopwatch_page_clicked;
}

/* --- GtkWidget */

/* the ring's box is square, between the guidelines and at most MAX_CIRCLE. With no laps it is centred over the
 * buttons' row (the tab's FAB_ROW bottom padding); with laps it is at the top and the list fills the rest. */
/* The time and the hundredths laid out in the ring in whole device pixels: each text view as wide as its text
 * rounded up, plus its padding; the two side by side, centred, left to right whatever the language; on one
 * baseline at the larger ascent, the row centred by that ascent and the larger descent, each rounded to the nearest
 * device pixel. A GtkBox would align them in whole px, and GtkLabel cuts its baseline to a whole px: 1 px low and 2
 * device pixels to the left. */
static void
place_time (StopwatchPage *self, int scale)
{
  GtkWidget *labels[2] = { self->main_text, self->hundredths };
  double before[2] = { TIME_OUTER, TIME_INNER }, after[2] = { TIME_INNER, TIME_OUTER };
  double widths[2], baselines[2], row = 0, ascent = 0, descent = 0, size = self->ring.size.width, x, y;

  for (int i = 0; i < 2; i++)
    {
      PangoLayout *layout = gtk_label_get_layout (GTK_LABEL (labels[i]));
      PangoRectangle line;

      pango_layout_get_extents (layout, NULL, &line);
      baselines[i] = (double) pango_layout_get_baseline (layout) / PANGO_SCALE;
      widths[i] = ceil ((double) line.width / PANGO_SCALE * scale) / scale;
      ascent = MAX (ascent, round (baselines[i] * scale) / scale);
      descent = MAX (descent, round (((double) line.height / PANGO_SCALE - baselines[i]) * scale) / scale);
      row += before[i] + widths[i] + after[i];
    }
  x = floor (scale * (size - row) / 2 + 0.5) / scale;
  y = floor (scale * (size - ascent - descent) / 2 + 0.5) / scale + ascent;
  for (int i = 0; i < 2; i++)
    {
      graphene_point_t at = GRAPHENE_POINT_INIT (self->ring.origin.x + x + before[i],
                                                 self->ring.origin.y + y - baselines[i]);
      int width, height;

      gtk_widget_measure (labels[i], GTK_ORIENTATION_HORIZONTAL, -1, NULL, &width, NULL, NULL);
      gtk_widget_measure (labels[i], GTK_ORIENTATION_VERTICAL, width, NULL, &height, NULL, NULL);
      gtk_widget_allocate (labels[i], width, height, -1, gsk_transform_translate (NULL, &at));
      x += before[i] + widths[i] + after[i];
    }
}

/* a tablet held sideways: the ring and the laps side by side between gutters, the laps at the end as wide as their
 * rows or half the room, whichever is wider, and centred top to bottom; the ring centred in the rest, or in all of
 * it without laps, moving between the two as the laps come and go */
static void
allocate_sideways (StopwatchPage *self, int width, int height, int scale)
{
  int gutter = (int) round (SIDE_GUTTER * width), inner = MAX (width - 2 * gutter, 0);
  int laps_width = 0, laps_height = 0, room;
  double size, full_x, rest_x, x, y;

  if (self->rows)
    {
      gtk_widget_measure (self->laps, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &laps_width, NULL, NULL);
      laps_width = MIN (MAX (laps_width, inner / 2), inner);
      gtk_widget_measure (self->laps, GTK_ORIENTATION_VERTICAL, laps_width, NULL, &laps_height, NULL, NULL);
    }
  room = inner - laps_width;
  size = MAX (MIN (MIN (inner, TABLET_CIRCLE), height - 2 * SIDE_BOTTOM), 0);
  full_x = gutter + (inner - size) / 2;
  rest_x = gutter + MAX (room - size, 0) / 2.0;
  x = floor (scale * (full_x + (rest_x - full_x) * self->move) + 0.5) / scale;
  y = floor (scale * (height - size) / 2 + 0.5) / scale;
  self->ring = GRAPHENE_RECT_INIT (x, y, size, size);
  if (size != self->fitted && !self->refit)
    self->refit = g_idle_add (on_refit, self);
  self->fitted = size;
  place_time (self, scale);
  if (gtk_widget_get_child_visible (self->scroller))
    {
      int laps_y = MAX ((height - laps_height) / 2, 0), laps_h = MIN (laps_height, height);

      self->laps_box = GRAPHENE_RECT_INIT (gutter + room, laps_y, laps_width, laps_h);
      /* nothing covers them here: they fade only where they go on past the bottom */
      self->laps_fade = laps_height > height ? MIN (FAB_ROW, laps_h) : 0;
      gtk_widget_size_allocate (self->scroller, &(GtkAllocation) { gutter + room, laps_y, laps_width, laps_h }, -1);
    }
}

static void
stopwatch_page_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  StopwatchPage *self = STOPWATCH_PAGE (widget);
  int scale = vibeos_device_scale (widget);
  int left = (int) round (GUIDELINE * width), right = (int) round ((1 - GUIDELINE) * width);
  double size = MIN (MIN (right - left, self->tablet ? TABLET_CIRCLE : MAX_CIRCLE), height);
  double middle;

  if (self->sideways)
    {
      allocate_sideways (self, width, height, scale);
      return;
    }

  /* in whole device pixels, where GTK's own allocation would round to whole px
   * (up to 4 device pixels off): the ring centred over the button row, or at the top with laps */
  middle = MAX (floor (scale * (height - FAB_ROW - size) / 2 + 0.5) / scale, 0);
  self->ring = GRAPHENE_RECT_INIT (left + (right - left - size) / 2,
                                   floor (scale * middle * (1 - self->move) + 0.5) / scale, size, size);
  /* a new size changes what fits; the label cannot change in the middle of the layout */
  if (size != self->fitted && !self->refit)
    self->refit = g_idle_add (on_refit, self);
  self->fitted = size;
  place_time (self, scale);
  self->laps_box = GRAPHENE_RECT_INIT (0, size, width, MAX (height - size, 0));
  self->laps_fade = MIN (FAB_ROW, self->laps_box.size.height);
  if (gtk_widget_get_child_visible (self->scroller))
    gtk_widget_size_allocate (self->scroller, &(GtkAllocation) { 0, size, width, MAX (height - size, 0) }, -1);
}

static void
stopwatch_page_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                        int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 0;
}

/* the ring, the time, the laps, and over the laps' last FAB_ROW a fade into the background (0 % to 50 % over the
 * first quarter, to 100 % by nine tenths); sideways, only when the laps run on past the bottom */
static void
stopwatch_page_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  static const guint8 alphas[] = { 0x00, 0x1A, 0x33, 0x4D, 0x66, 0x80, 0x89, 0x93, 0x9D, 0xA7, 0xB1,
                                   0xBA, 0xC4, 0xCE, 0xD8, 0xE2, 0xEB, 0xF5, 0xFF, 0xFF, 0xFF };
  StopwatchPage *self = STOPWATCH_PAGE (widget);
  GskColorStop stops[G_N_ELEMENTS (alphas)];
  /* under the laps: their box's bottom, which is the tab's own but sideways */
  float x = self->laps_box.origin.x, width = self->laps_box.size.width;
  float height = self->laps_box.origin.y + self->laps_box.size.height;
  float fade = self->laps_fade;

  snapshot_ring (self, snapshot);
  gtk_widget_snapshot_child (widget, self->main_text, snapshot);
  gtk_widget_snapshot_child (widget, self->hundredths, snapshot);
  if (!gtk_widget_get_child_visible (self->scroller))
    return;
  gtk_widget_snapshot_child (widget, self->scroller, snapshot);
  if (fade <= 0)
    return;
  for (guint i = 0; i < G_N_ELEMENTS (alphas); i++)
    stops[i] = (GskColorStop) { (float) i / (G_N_ELEMENTS (alphas) - 1),
                                vibeos_color_alpha (VIBEOS_SURFACE, alphas[i] / 255.f * self->fade) };
  gtk_snapshot_append_linear_gradient (snapshot, &GRAPHENE_RECT_INIT (x, height - fade, width, fade),
                                       &GRAPHENE_POINT_INIT (x, height - fade), &GRAPHENE_POINT_INIT (x, height),
                                       stops, G_N_ELEMENTS (stops));
}

/* a tablet: the smaller ring; held sideways, the laps beside it, with no room kept under them for the buttons, which
 * are in their own column */
static void
on_layout_changed (VibeosLayout *layout, StopwatchPage *self)
{
  self->tablet = vibeos_layout_is_tablet (layout);
  self->sideways = self->tablet && vibeos_layout_is_landscape (layout);
  gtk_widget_set_margin_bottom (self->laps, self->sideways ? SIDE_BOTTOM : FAB_ROW);
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

static void
stopwatch_page_root (GtkWidget *widget)
{
  StopwatchPage *self = STOPWATCH_PAGE (widget);

  GTK_WIDGET_CLASS (stopwatch_page_parent_class)->root (widget);
  self->layout = vibeos_layout_get (widget);
  if (self->layout)
    {
      g_object_ref (self->layout);
      g_signal_connect (self->layout, "changed", G_CALLBACK (on_layout_changed), self);
      on_layout_changed (self->layout, self);
    }
}

static void
stopwatch_page_unroot (GtkWidget *widget)
{
  StopwatchPage *self = STOPWATCH_PAGE (widget);

  if (self->layout)
    {
      g_signal_handlers_disconnect_by_data (self->layout, self);
      g_clear_object (&self->layout);
    }
  GTK_WIDGET_CLASS (stopwatch_page_parent_class)->unroot (widget);
}

static void
stopwatch_page_map (GtkWidget *widget)
{
  StopwatchPage *self = STOPWATCH_PAGE (widget);

  GTK_WIDGET_CLASS (stopwatch_page_parent_class)->map (widget);
  /* shown again: the laps may have changed while the tab was away */
  self->lap_length = self->total_length = 0;
  refresh_rows (self);
  update_ui (self);
}

static void
stopwatch_page_unmap (GtkWidget *widget)
{
  StopwatchPage *self = STOPWATCH_PAGE (widget);

  /* hidden: nothing updates while the tab is away */
  g_clear_handle_id (&self->update, g_source_remove);
  GTK_WIDGET_CLASS (stopwatch_page_parent_class)->unmap (widget);
  adjust_wake_lock (self);
}

static void
stopwatch_page_dispose (GObject *object)
{
  StopwatchPage *self = STOPWATCH_PAGE (object);

  g_clear_handle_id (&self->update, g_source_remove);
  g_clear_handle_id (&self->refit, g_source_remove);
  if (self->inhibit)
    gtk_application_uninhibit (GTK_APPLICATION (g_application_get_default ()), self->inhibit);
  self->inhibit = 0;
  if (self->client)
    g_signal_handlers_disconnect_by_data (self->client, self);
  g_clear_pointer (&self->transition, vibeos_anim_free);
  g_clear_pointer (&self->main_text, gtk_widget_unparent);
  g_clear_pointer (&self->hundredths, gtk_widget_unparent);
  g_clear_pointer (&self->scroller, gtk_widget_unparent);
  G_OBJECT_CLASS (stopwatch_page_parent_class)->dispose (object);
}

static void
stopwatch_page_class_init (StopwatchPageClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = stopwatch_page_dispose;
  widget_class->measure = stopwatch_page_measure;
  widget_class->size_allocate = stopwatch_page_size_allocate;
  widget_class->snapshot = stopwatch_page_snapshot;
  widget_class->map = stopwatch_page_map;
  widget_class->unmap = stopwatch_page_unmap;
  widget_class->root = stopwatch_page_root;
  widget_class->unroot = stopwatch_page_unroot;
  gtk_widget_class_set_css_name (widget_class, "stopwatch-page");
}

static void
stopwatch_page_init (StopwatchPage *self)
{
  GtkGesture *click;

  self->shown_time = G_MININT64;

  /* the time and the smaller hundredths, without font padding, so a label's own line is right; place_time puts
   * them on one baseline */
  self->main_text = gtk_label_new ("0");
  gtk_widget_add_css_class (self->main_text, "stopwatch-main");
  self->hundredths = gtk_label_new ("00");
  gtk_widget_add_css_class (self->hundredths, "stopwatch-hundredths");
  /* each with its text at its top left, where place_time puts it */
  gtk_label_set_xalign (GTK_LABEL (self->main_text), 0);
  gtk_label_set_yalign (GTK_LABEL (self->main_text), 0);
  gtk_label_set_xalign (GTK_LABEL (self->hundredths), 0);
  gtk_label_set_yalign (GTK_LABEL (self->hundredths), 0);
  gtk_widget_set_parent (self->main_text, GTK_WIDGET (self));
  gtk_widget_set_parent (self->hundredths, GTK_WIDGET (self));

  /* the laps, with room under the last for the FAB row */
  self->laps = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_bottom (self->laps, FAB_ROW);
  gtk_widget_set_valign (self->laps, GTK_ALIGN_START);
  self->scroller = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (self->scroller), self->laps);
  gtk_widget_set_parent (self->scroller, GTK_WIDGET (self));
  gtk_widget_set_child_visible (self->scroller, FALSE);
  self->transition = vibeos_anim_new (GTK_WIDGET (self), on_transition, self);

  click = gtk_gesture_click_new ();
  g_signal_connect (click, "pressed", G_CALLBACK (on_pressed), self);
  g_signal_connect (click, "released", G_CALLBACK (on_released), self);
  g_signal_connect (click, "stopped", G_CALLBACK (on_stopped), self);
  g_signal_connect (click, "cancel", G_CALLBACK (on_stopped), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
}

GtkWidget *
stopwatch_page_new (VibeosSnackbar *snackbar)
{
  StopwatchPage *self = g_object_new (STOPWATCH_TYPE_PAGE, NULL);

  self->snackbar = snackbar;
  self->client = stopwatch_client_get ();
  g_signal_connect (self->client, "changed", G_CALLBACK (on_changed), self);
  on_changed (self->client, self);
  return GTK_WIDGET (self);
}
