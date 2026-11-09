/* CalMonthView: Etar's Month view (month-view.h; spec/calendar.md §4). It draws itself in device pixels, as
 * MonthWeekEventsView draws its canvas. */
#include "month-view.h"
#include "cal-colors.h"
#include "cal-text.h"

#include <glib/gi18n.h>
#include <math.h>
#include <neo.h>
#include <spongy.h>

/* --- dimensions, dp (E/month/MonthWeekEventsView.java:76-108, L/full_month_header.xml, V/integers.xml) */
#define HEADER_ROW 16.0                 /* full_month_header: 16 dp tall … */
#define HEADER_MARGIN 10.0              /* … with 10 dp under it */
#define HEADER_TOP_PADDING 1.0          /* full_month_by_week's paddingTop */
#define DAY_NAME_SIZE 11.0              /* day_label_text_size */
#define DAY_NAME_END_MARGIN 6.0
#define WEEKS_SHOWN 6
#define MONTH_NUMBER_SIZE 20.0          /* text_size_month_number */
#define MONTH_NUMBER_TOP 3.0            /* mTopPaddingMonthNumber */
#define MONTH_NUMBER_SIDE 4.0           /* month_day_number_margin */
#define SEPARATOR_WIDTH 1.0             /* mDaySeparatorInnerWidth */
#define EVENT_TITLE_SIZE 10.0           /* text_size_event_title */
#define EXTRAS_SIZE 12.0                /* mTextSizeEvent: the "+N" */
#define EVENT_Y_OFFSET 2.0              /* mEventYOffsetPortrait */
#define EVENT_SQUARE_WIDTH 3.0
#define EVENT_SQUARE_HEIGHT 10.0
#define EVENT_LINE_PADDING 2.0
#define EVENT_RIGHT_PADDING 4.0
#define CLICKED_ALPHA (128 / 255.0)

/* MonthListView: below 1500 dp/s the list flings freely; above, it snaps to the start of a month, one month below
 * 2000 dp/s and one more per 500 dp/s above that, in 1000 ms */
#define MIN_VELOCITY_FOR_FLING 1500.0
#define MULTIPLE_MONTH_VELOCITY_THRESHOLD 2000.0
#define FLING_VELOCITY_DIVIDER 500.0
#define FLING_TIME 1000
#define TOUCH_SLOP 8.0

struct _CalMonthView {
  GtkWidget parent;
  int week_start;
  int base;                     /* a day that starts a week: week w starts on base + 7 w */
  double top_week;              /* the scroll: the week at the top of the list, fractional */
  int focus_month;              /* year · 12 + month − 1 */
  GPtrArray *events;
  int clicked_day;              /* the pressed day, drawn in the clicked colour; 0 for none */

  gboolean dragging;
  double down_y, start_top;
  SpongyVelocity velocity;
  SpongyFling fling;
  guint tick;
  NeoAnim *snap;
  double snap_from, snap_to;
};

G_DEFINE_FINAL_TYPE (CalMonthView, cal_month_view, GTK_TYPE_WIDGET)

enum { DAY_ACTIVATED, MOVED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* --- days and months */

static int
month_of (int day)
{
  GDate date;

  g_date_clear (&date, 1);
  g_date_set_julian (&date, day);
  return g_date_get_year (&date) * 12 + g_date_get_month (&date) - 1;
}

static int
month_start (int month)
{
  GDate date;

  g_date_clear (&date, 1);
  g_date_set_dmy (&date, 1, month % 12 + 1, month / 12);
  return g_date_get_julian (&date);
}

static int
week_of (CalMonthView *self, int day)
{
  int d = day - self->base;

  return d >= 0 ? d / 7 : -((-d + 6) / 7);
}

static int
week_first_day (CalMonthView *self, int week)
{
  return self->base + 7 * week;
}

/* --- geometry */

static int
scale_of (CalMonthView *self)
{
  return neo_device_scale (GTK_WIDGET (self));
}

/* the top of the list of weeks: the names, their margin and the 1 px line under them */
static double
list_top (CalMonthView *self)
{
  return HEADER_TOP_PADDING + HEADER_ROW + HEADER_MARGIN + 1.0 / scale_of (self);
}

/* each week is a sixth of the list, in whole device pixels (MonthByWeekAdapter: list height / 6) */
static double
row_height (CalMonthView *self)
{
  int scale = scale_of (self);
  double list = gtk_widget_get_height (GTK_WIDGET (self)) - list_top (self);

  return MAX (1.0, floor (list * scale / WEEKS_SHOWN) / scale);
}

/* the x where day column @i starts (computeDayLeftPosition: i · width / 7, in whole device pixels) */
static double
day_left (CalMonthView *self, int i)
{
  int scale = scale_of (self);

  return floor (i * gtk_widget_get_width (GTK_WIDGET (self)) * scale / 7.0) / scale;
}

/* the month in focus: the month of the middle of the week in the middle of the list */
static void
update_focus (CalMonthView *self)
{
  int week = (int) floor (self->top_week + WEEKS_SHOWN / 2.0);
  int month = month_of (week_first_day (self, week) + 3);

  if (month == self->focus_month)
    return;
  self->focus_month = month;
  g_signal_emit (self, signals[MOVED], 0);
}

static void
set_top_week (CalMonthView *self, double top_week)
{
  self->top_week = top_week;
  update_focus (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* --- drawing */

static void
draw_header (CalMonthView *self, GtkSnapshot *snapshot, int scale, double width)
{
  GdkRGBA line = cal_argb (CAL_GRID_LINE);
  double baseline;

  /* the weekday names (TextViews: Regular + 300 = 700), uppercase, at the end of each column less 6 dp */
  cal_text_line (DAY_NAME_SIZE, scale, &baseline, NULL);
  for (int i = 0; i < 7; i++)
    {
      int weekday = (self->week_start + i) % 7;
      g_autofree char *name = NULL, *upper = NULL;
      g_autoptr(PangoLayout) layout = NULL;
      /* Sunday and Saturday: month_sunday_dark / month_saturday_dark */
      GdkRGBA color = cal_argb (weekday == 0 || weekday == 6 ? 0xfff5f5f5 : CAL_MONTH_DAY_NAMES);
      /* any week's day with that weekday: the base is a week's first day */
      int day = self->base + i;

      {
        g_autoptr(GDateTime) date = g_date_time_new_from_unix_local (cal_day_start (day));

        name = g_date_time_format (date, "%a");
      }
      upper = g_utf8_strup (name, -1);
      layout = cal_text_layout (GTK_WIDGET (self), upper, DAY_NAME_SIZE, 700);
      cal_text_draw (snapshot, layout, day_left (self, i + 1) - DAY_NAME_END_MARGIN, HEADER_TOP_PADDING + baseline, 1,
                     &color);
    }
  cal_fill_rect (snapshot, scale, 0, list_top (self) - 1.0 / scale, width, 1.0 / scale, &line);
}

typedef struct {
  CalEvent *event;
  int first, last;              /* the days of the week it covers, 0 … 6 */
  gboolean bar;                 /* drawn as all-day: a filled bar over its days */
  int slot;                     /* its line in the day boxes */
} Item;

static int
compare_bars (gconstpointer a, gconstpointer b)
{
  const Item *x = a, *y = b;

  /* sortedAddRemainingEventToList: the longest first, else in the events' order */
  if (y->last - y->first != x->last - x->first)
    return (y->last - y->first) - (x->last - x->first);
  return cal_event_compare (&x->event, &y->event);
}

#define MAX_SLOTS 64

/* the bars first, the longest first; then the timed events in their order */
static void
sort_items (GArray *items)
{
  GArray *bars = g_array_new (FALSE, FALSE, sizeof (Item)), *timed = g_array_new (FALSE, FALSE, sizeof (Item));

  for (guint i = 0; i < items->len; i++)
    g_array_append_val (g_array_index (items, Item, i).bar ? bars : timed, g_array_index (items, Item, i));
  g_array_sort (bars, compare_bars);
  g_array_set_size (items, 0);
  g_array_append_vals (items, bars->data, bars->len);
  g_array_append_vals (items, timed->data, timed->len);
  g_array_unref (bars);
  g_array_unref (timed);
}

/* the week's events and the line each takes: bars first, each on the first line free on all its days, then the
 * timed events of each day on its first free line (a simpler form of WeekEventFormatter, which also moves events
 * between lines to fill gaps) */
static GArray *
week_items (CalMonthView *self, int first_day)
{
  GArray *items = g_array_new (FALSE, FALSE, sizeof (Item));
  guint64 used[7] = { 0 };

  for (guint i = 0; i < self->events->len; i++)
    {
      CalEvent *event = self->events->pdata[i];
      int a = cal_event_first_day (event), b = cal_event_last_day (event);
      Item item = { event, MAX (a, first_day) - first_day, MIN (b, first_day + 6) - first_day, FALSE, -1 };

      if (b < first_day || a > first_day + 6)
        continue;
      if (cal_event_draws_as_all_day (event))
        {
          item.bar = TRUE;
          g_array_append_val (items, item);
        }
      else
        /* a timed event over midnight shows on each of its days */
        for (int d = item.first; d <= item.last; d++)
          {
            Item one = item;

            one.first = one.last = d;
            g_array_append_val (items, one);
          }
    }
  sort_items (items);
  for (guint i = 0; i < items->len; i++)
    {
      Item *item = &g_array_index (items, Item, i);
      guint64 busy = 0;

      for (int d = item->first; d <= item->last; d++)
        busy |= used[d];
      for (int slot = 0; slot < MAX_SLOTS; slot++)
        if (!(busy & ((guint64) 1 << slot)))
          {
            item->slot = slot;
            break;
          }
      if (item->slot < 0)
        continue;
      for (int d = item->first; d <= item->last; d++)
        used[d] |= (guint64) 1 << item->slot;
    }
  return items;
}

static void
draw_week (CalMonthView *self, GtkSnapshot *snapshot, int scale, int first_day, double y0, double height,
           int today)
{
  double width = gtk_widget_get_width (GTK_WIDGET (self)), px = 1.0 / scale;
  GdkRGBA line = cal_argb (CAL_GRID_LINE), white = cal_argb (CAL_MONTH_NUMBER);
  double number_ascent, number_height, event_ascent, event_height, extras_ascent, extras_height, extras_descent;
  double events_top, slot_height, available;
  int visible[7], hidden[7] = { 0 };
  g_autoptr(GArray) items = NULL;

  cal_paint_line (MONTH_NUMBER_SIZE, scale, &number_ascent, &number_height);
  cal_paint_line (EVENT_TITLE_SIZE, scale, &event_ascent, &event_height);
  cal_paint_line (EXTRAS_SIZE, scale, &extras_ascent, &extras_height);
  extras_descent = extras_height - extras_ascent;

  gtk_snapshot_save (snapshot);
  gtk_snapshot_push_clip (snapshot, &GRAPHENE_RECT_INIT (0, (float) y0, (float) width, (float) height));
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (0, (float) y0));

  /* drawBackground: the month in focus #212121; other months alternate black and the list's #141414 by the parity
   * of their month (mOddMonth); today #6EFFFFFA over it */
  for (int i = 0; i < 7; i++)
    {
      int day = first_day + i, month = month_of (day);
      double x = day_left (self, i), w = day_left (self, i + 1) - x;

      if (month == self->focus_month)
        {
          GdkRGBA bg = cal_argb (CAL_MONTH_FOCUS_BG);

          cal_fill_rect (snapshot, scale, x, SEPARATOR_WIDTH, w, height - SEPARATOR_WIDTH, &bg);
        }
      else if ((month % 12) % 2 == 0)
        {
          GdkRGBA bg = cal_argb (CAL_MONTH_OTHER_BG);

          cal_fill_rect (snapshot, scale, x, SEPARATOR_WIDTH, w, height - SEPARATOR_WIDTH, &bg);
        }
      if (day == today)
        {
          GdkRGBA bg = cal_argb (CAL_MONTH_TODAY_BG);

          cal_fill_rect (snapshot, scale, x, SEPARATOR_WIDTH, w, height - SEPARATOR_WIDTH, &bg);
        }
    }

  /* drawWeekNums: the day numbers, 20 sp Regular (bold for today), right-aligned 4 dp in from the column's end */
  for (int i = 0; i < 7; i++)
    {
      int day = first_day + i;
      GDate date;
      g_autofree char *number = NULL;
      g_autoptr(PangoLayout) layout = NULL;
      GdkRGBA color = cal_argb (month_of (day) == self->focus_month || day == today ? CAL_MONTH_NUMBER
                                                                                    : CAL_MONTH_OTHER_NUMBER);

      g_date_clear (&date, 1);
      g_date_set_julian (&date, day);
      number = g_strdup_printf ("%d", g_date_get_day (&date));
      layout = cal_text_layout (GTK_WIDGET (self), number, MONTH_NUMBER_SIZE, day == today ? 700 : 400);
      cal_text_draw (snapshot, layout, day_left (self, i + 1) - MONTH_NUMBER_SIDE, number_ascent + MONTH_NUMBER_TOP, 1,
                     &color);
    }

  /* drawDaySeparators: 1 dp lines, the horizontal one along the top, centred on y 0 and x of each column (the
   * first only half shows) */
  cal_fill_rect (snapshot, scale, 0, 0, width, ceil (SEPARATOR_WIDTH * scale / 2.0) * px, &line);
  for (int i = 0; i < 7; i++)
    {
      double x = day_left (self, i);
      double half = floor (SEPARATOR_WIDTH * scale / 2.0) * px;

      cal_fill_rect (snapshot, scale, MAX (0, x - half), 0, i == 0 ? ceil (SEPARATOR_WIDTH * scale / 2.0) * px
                                                                   : SEPARATOR_WIDTH, height, &line);
    }

  /* drawEvents: from under the number down, each event one line (FixedHeightRegularBoundariesSetter: the title's
   * height, a 1 px border above and below, 2 dp between); "+N" at the bottom when not all fit */
  items = week_items (self, first_day);
  events_top = EVENT_Y_OFFSET + number_height + MONTH_NUMBER_TOP;
  slot_height = event_height + 2 * px + EVENT_LINE_PADDING;
  available = height - events_top - px;
  for (int d = 0; d < 7; d++)
    {
      int used = 0;

      for (guint i = 0; i < items->len; i++)
        {
          Item *item = &g_array_index (items, Item, i);

          if (item->slot >= 0 && item->first <= d && d <= item->last)
            used = MAX (used, item->slot + 1);
        }
      visible[d] = (int) floor ((available + EVENT_LINE_PADDING) / slot_height + 1e-6);
      if (used > visible[d])
        visible[d] = (int) floor ((available - extras_height + EVENT_LINE_PADDING) / slot_height + 1e-6);
      visible[d] = MAX (0, visible[d]);
    }
  for (guint i = 0; i < items->len; i++)
    {
      Item *item = &g_array_index (items, Item, i);
      CalEvent *event = item->event;
      GdkRGBA color = cal_argb (0xff000000 | cal_event_display_color (event));
      const char *title = event->title && *event->title ? event->title : _("(No title)");

      for (int d = item->first; d <= item->last; d++)
        if (item->slot < 0 || item->slot >= visible[d])
          hidden[d]++;
      if (item->slot < 0)
        continue;
      for (int d = item->first; d <= item->last;)
        {
          int end = d;
          double x, right, y = events_top + item->slot * slot_height;
          g_autoptr(PangoLayout) layout = NULL;

          if (item->slot >= visible[d])
            {
              d++;
              continue;
            }
          while (end + 1 <= item->last && item->slot < visible[end + 1])
            end++;
          x = day_left (self, d) + px;
          right = day_left (self, end + 1) - px;
          layout = cal_text_layout (GTK_WIDGET (self), title, EVENT_TITLE_SIZE, 700);
          if (item->bar)
            {
              /* AllDayBoundariesSetter: the bar over its days, the title in white inside its 1 px border */
              cal_fill_rect (snapshot, scale, x, y, right - x, event_height + 2 * px, &color);
              cal_text_ellipsize (layout, right - x - 2 * px);
              gtk_snapshot_push_clip (snapshot, &GRAPHENE_RECT_INIT ((float) x, (float) y, (float) (right - x),
                                                                     (float) (event_height + 2 * px)));
              cal_text_draw (snapshot, layout, x + px, y + px + event_ascent, 0, &white);
              gtk_snapshot_pop (snapshot);
            }
          else
            {
              /* RegularBoundariesSetter: a 3 × 10 dp square in the event's colour, the title 4 dp after it */
              double text_x = x + px + EVENT_SQUARE_WIDTH + EVENT_RIGHT_PADDING - px;

              cal_fill_rect (snapshot, scale, x, y + px + event_ascent - EVENT_SQUARE_HEIGHT, EVENT_SQUARE_WIDTH,
                             EVENT_SQUARE_HEIGHT, &color);
              cal_text_ellipsize (layout, right - text_x);
              cal_text_draw (snapshot, layout, text_x, y + px + event_ascent, 0, &white);
            }
          d = end + 1;
        }
    }
  for (int d = 0; d < 7; d++)
    if (hidden[d] > 0)
      {
        /* drawMoreEvents: "+N", 12 sp fake bold, white, at the bottom of the box */
        g_autofree char *text = g_strdup_printf (g_dngettext (NULL, "+%d", "+%d", hidden[d]), hidden[d]);
        g_autoptr(PangoLayout) layout = cal_text_layout (GTK_WIDGET (self), text, EXTRAS_SIZE, 700);

        cal_text_draw (snapshot, layout, day_left (self, d) + px, height - (extras_descent + px), 0, &white);
      }

  /* drawClick: the tapped day in #33B5E5 at half alpha */
  if (self->clicked_day >= first_day && self->clicked_day < first_day + 7)
    {
      int i = self->clicked_day - first_day;
      GdkRGBA clicked = cal_argb (0xff33b5e5);

      clicked.alpha = CLICKED_ALPHA;
      cal_fill_rect (snapshot, scale, day_left (self, i), SEPARATOR_WIDTH, day_left (self, i + 1) - day_left (self, i),
                     height - SEPARATOR_WIDTH, &clicked);
    }
  gtk_snapshot_pop (snapshot);
  gtk_snapshot_restore (snapshot);
}

static void
cal_month_view_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CalMonthView *self = CAL_MONTH_VIEW (widget);
  int scale = scale_of (self);
  double width = gtk_widget_get_width (widget), height = gtk_widget_get_height (widget);
  double top = list_top (self), row = row_height (self);
  GdkRGBA bg = cal_argb (CAL_FUTURE_BG);         /* month_bgcolor_dark: bg_dark */
  GdkRGBA surface = cal_argb (CAL_SURFACE);
  int first_week = (int) floor (self->top_week);
  double offset = (self->top_week - first_week) * row;
  int today = cal_day_number (g_get_real_time () / G_USEC_PER_SEC);

  gtk_snapshot_append_color (snapshot, &surface, &GRAPHENE_RECT_INIT (0, 0, (float) width, (float) top));
  gtk_snapshot_append_color (snapshot, &bg, &GRAPHENE_RECT_INIT (0, (float) top, (float) width,
                                                                 (float) (height - top)));
  draw_header (self, snapshot, scale, width);
  gtk_snapshot_push_clip (snapshot, &GRAPHENE_RECT_INIT (0, (float) top, (float) width, (float) (height - top)));
  for (int w = 0; top - offset + w * row < height; w++)
    {
      /* each week on whole device pixels, as the list lays its children out */
      double y = round ((top - offset + w * row) * scale) / scale;

      draw_week (self, snapshot, scale, week_first_day (self, first_week + w), y, row, today);
    }
  gtk_snapshot_pop (snapshot);
}

/* --- scrolling */

static double
viscous_fluid (double x)
{
  /* Scroller.ViscousFluidInterpolator, the curve of AbsListView.smoothScrollBy */
  x *= 8;
  if (x < 1)
    return x - (1 - exp (-x));
  return 0.36787944117 + (1 - exp (1 - x)) * (1 - 0.36787944117);
}

static void
on_snap (NeoAnim *anim, double value, gpointer data)
{
  CalMonthView *self = data;
  double t = viscous_fluid (value) / viscous_fluid (1);

  set_top_week (self, self->snap_from + (self->snap_to - self->snap_from) * t);
}

static void
scroll_to_week (CalMonthView *self, double week, guint duration)
{
  if (!duration || !gtk_widget_get_mapped (GTK_WIDGET (self)))
    {
      neo_anim_stop (self->snap);
      set_top_week (self, week);
      return;
    }
  self->snap_from = self->top_week;
  self->snap_to = week;
  neo_anim_start (self->snap, 0, 1, duration, 0, SPONGY_LINEAR);
}

static gboolean
on_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  CalMonthView *self = CAL_MONTH_VIEW (widget);
  double unit = row_height (self) * scale_of (self);
  gboolean running = spongy_fling_update (&self->fling, gdk_frame_clock_get_frame_time (clock) / 1000.0);

  set_top_week (self, self->fling.position / unit);
  if (!running)
    {
      self->tick = 0;
      return G_SOURCE_REMOVE;
    }
  return G_SOURCE_CONTINUE;
}

static void
stop_motion (CalMonthView *self)
{
  if (self->tick)
    gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->tick);
  self->tick = 0;
  neo_anim_stop (self->snap);
}

/* MonthListView.doFling: a fast fling goes to the first week of the next (or this, scrolling back) month, or
 * further the faster it is */
static void
fling (CalMonthView *self, double velocity)
{
  int scale = scale_of (self);
  double speed = velocity;      /* dp/s, positive toward later weeks */
  GdkFrameClock *clock = gtk_widget_get_frame_clock (GTK_WIDGET (self));

  if (fabs (speed) > MIN_VELOCITY_FOR_FLING)
    {
      int jump, upper_right = week_first_day (self, (int) floor (self->top_week)) + 6;
      /* Android's velocityY is negative toward later weeks */
      double v = -speed;

      if (fabs (v) < MULTIPLE_MONTH_VELOCITY_THRESHOLD)
        jump = v < 0 ? 1 : 0;
      else if (v < 0)
        jump = 1 - (int) ((v + MULTIPLE_MONTH_VELOCITY_THRESHOLD) / FLING_VELOCITY_DIVIDER);
      else
        jump = -(int) ((v - MULTIPLE_MONTH_VELOCITY_THRESHOLD) / FLING_VELOCITY_DIVIDER);
      scroll_to_week (self, week_of (self, month_start (month_of (upper_right) + jump)), FLING_TIME);
      return;
    }
  if (!clock || fabs (speed) < 50)
    return;
  {
    double unit = row_height (self) * scale;

    spongy_fling_start (&self->fling, (float) (self->top_week * unit), (float) (speed * scale), -INFINITY, INFINITY,
                        gdk_frame_clock_get_frame_time (clock) / 1000.0);
    if (!self->tick)
      self->tick = gtk_widget_add_tick_callback (GTK_WIDGET (self), on_tick, NULL, NULL);
  }
}

static double
event_time_ms (GtkGesture *gesture)
{
  return gtk_event_controller_get_current_event_time (GTK_EVENT_CONTROLLER (gesture));
}

static void
on_drag_begin (GtkGestureDrag *gesture, double x, double y, CalMonthView *self)
{
  stop_motion (self);
  self->dragging = FALSE;
  self->down_y = y;
  self->start_top = self->top_week;
  spongy_velocity_clear (&self->velocity);
  spongy_velocity_add (&self->velocity, event_time_ms (GTK_GESTURE (gesture)), x, y);
}

static void
on_drag_update (GtkGestureDrag *gesture, double dx, double dy, CalMonthView *self)
{
  spongy_velocity_add (&self->velocity, event_time_ms (GTK_GESTURE (gesture)), dx, self->down_y + dy);
  if (!self->dragging)
    {
      if (fabs (dy) < TOUCH_SLOP)
        return;
      self->dragging = TRUE;
      self->clicked_day = 0;
      gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    }
  set_top_week (self, self->start_top - dy / row_height (self));
}

static void
on_drag_end (GtkGestureDrag *gesture, double dx, double dy, CalMonthView *self)
{
  float vx = 0, vy = 0;

  if (!self->dragging)
    return;
  self->dragging = FALSE;
  spongy_velocity_get (&self->velocity, event_time_ms (GTK_GESTURE (gesture)), &vx, &vy);
  fling (self, -vy);
}

static int
day_at (CalMonthView *self, double x, double y)
{
  double top = list_top (self), row = row_height (self);
  int week, column = 0;

  if (y < top)
    return 0;
  week = (int) floor (self->top_week + (y - top) / row);
  while (column < 6 && x >= day_left (self, column + 1))
    column++;
  return week_first_day (self, week) + column;
}

static gboolean
day_has_events (CalMonthView *self, int day)
{
  for (guint i = 0; i < self->events->len; i++)
    {
      CalEvent *event = self->events->pdata[i];

      if (cal_event_first_day (event) <= day && day <= cal_event_last_day (event))
        return TRUE;
    }
  return FALSE;
}

static void
on_pressed (GtkGestureClick *gesture, int n, double x, double y, CalMonthView *self)
{
  self->clicked_day = day_at (self, x, y);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_released (GtkGestureClick *gesture, int n, double x, double y, CalMonthView *self)
{
  int day = day_at (self, x, y);

  self->clicked_day = 0;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  if (day && !self->dragging)
    g_signal_emit (self, signals[DAY_ACTIVATED], 0, day, day_has_events (self, day));
}

static void
on_click_cancel (GtkGesture *gesture, GdkEventSequence *sequence, CalMonthView *self)
{
  self->clicked_day = 0;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static gboolean
on_scroll (GtkEventControllerScroll *controller, double dx, double dy, CalMonthView *self)
{
  /* a wheel: a week per notch; a touchpad: as it moves */
  if (gtk_event_controller_scroll_get_unit (controller) == GDK_SCROLL_UNIT_WHEEL)
    set_top_week (self, round (self->top_week) + dy);
  else
    set_top_week (self, self->top_week + dy / row_height (self));
  return TRUE;
}

/* --- GtkWidget */

static void
cal_month_view_dispose (GObject *object)
{
  CalMonthView *self = CAL_MONTH_VIEW (object);

  stop_motion (self);
  g_clear_pointer (&self->snap, neo_anim_free);
  g_clear_pointer (&self->events, g_ptr_array_unref);
  G_OBJECT_CLASS (cal_month_view_parent_class)->dispose (object);
}

static void
cal_month_view_class_init (CalMonthViewClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = cal_month_view_dispose;
  widget_class->snapshot = cal_month_view_snapshot;
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_GRID);
  signals[DAY_ACTIVATED] = g_signal_new ("day-activated", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                         NULL, NULL, G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_BOOLEAN);
  signals[MOVED] = g_signal_new ("moved", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                 G_TYPE_NONE, 0);
}

static void
cal_month_view_init (CalMonthView *self)
{
  GtkGesture *drag = gtk_gesture_drag_new ();
  GtkGesture *click = gtk_gesture_click_new ();
  GtkEventController *scroll = gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);

  self->events = g_ptr_array_new_with_free_func ((GDestroyNotify) cal_event_unref);
  self->snap = neo_anim_new (GTK_WIDGET (self), on_snap, self);
  gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);
  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_drag_end), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));
  g_signal_connect (click, "pressed", G_CALLBACK (on_pressed), self);
  g_signal_connect (click, "released", G_CALLBACK (on_released), self);
  g_signal_connect (click, "cancel", G_CALLBACK (on_click_cancel), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
  g_signal_connect (scroll, "scroll", G_CALLBACK (on_scroll), self);
  gtk_widget_add_controller (GTK_WIDGET (self), scroll);
}

GtkWidget *
cal_month_view_new (void)
{
  CalMonthView *self = g_object_new (CAL_TYPE_MONTH_VIEW, NULL);

  cal_month_view_set_week_start (self, 0);
  cal_month_view_go_to (self, cal_day_number (g_get_real_time () / G_USEC_PER_SEC));
  return GTK_WIDGET (self);
}

void
cal_month_view_set_week_start (CalMonthView *self, int week_start)
{
  int focus = self->focus_month ? month_start (self->focus_month) : 0;

  self->week_start = week_start;
  /* a week's first day near the start of GDate's days (day 0 is not one: GDate's Julian days begin at 1) */
  self->base = 8 - ((cal_day_of_week (8) - week_start + 7) % 7);
  if (focus)
    cal_month_view_go_to (self, focus);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
cal_month_view_go_to (CalMonthView *self, int day)
{
  int month = month_of (day);

  stop_motion (self);
  /* LIST_TOP_OFFSET −1: the week of the 1st at the top */
  self->top_week = week_of (self, month_start (month));
  if (month != self->focus_month)
    {
      self->focus_month = month;
      g_signal_emit (self, signals[MOVED], 0);
    }
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

int
cal_month_view_get_focus_day (CalMonthView *self)
{
  return month_start (self->focus_month);
}

void
cal_month_view_get_range (CalMonthView *self, int *first, int *last)
{
  int start = week_first_day (self, week_of (self, month_start (self->focus_month)));

  /* the six weeks of the month and as many again above and below, so a scroll finds them loaded */
  *first = start - 7 * WEEKS_SHOWN;
  *last = start + 7 * 2 * WEEKS_SHOWN - 1;
}

void
cal_month_view_set_events (CalMonthView *self, GPtrArray *events)
{
  g_ptr_array_set_size (self->events, 0);
  for (guint i = 0; events && i < events->len; i++)
    g_ptr_array_add (self->events, cal_event_ref (events->pdata[i]));
  gtk_widget_queue_draw (GTK_WIDGET (self));
}
