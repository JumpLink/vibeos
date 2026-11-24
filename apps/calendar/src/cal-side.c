/* CalSide: see cal-side.h. Etar's mini-month and calendar list, in the theme's colours. */
#include "cal-side.h"
#include "cal-colors.h"
#include "cal-format.h"
#include "cal-text.h"

#include <glib/gi18n.h>
#include <vibeos.h>
#include <spongy-gtk.h>

#define MONTH_HEIGHT 262.0              /* the small month in the column */
#define MONTH_WIDTH  350.0              /* ... and in the strip */
#define MONTH_PAD    5.0                /* the small month inside its box, at the sides */
#define MONTH_TOP    7.0                /* ... over and under it */
#define TITLE_HEIGHT 38.5               /* "November 2026" */
#define TITLE_SIZE   18.0
#define NAMES_HEIGHT 16.5               /* the weekday initials */
#define NAMES_SIZE   12.0
#define DAY_SIZE     14.0
#define TODAY_SIZE   18.0               /* today's number, larger and bold */
#define WEEKS        6
#define SWIPE        40.0               /* a vertical drag this far on the month changes it */

#define LIST_PAD     5.0                /* the calendars inside their box */
#define ROW_HEIGHT   49.0               /* a calendar's row */
#define SWATCH       20.0               /* its colour square */
#define SWATCH_START 16.0
#define NAME_START   48.0               /* its name */
#define FRAME        2.0                /* a hidden calendar's square: a frame this wide */

/* --- the small month */

#define CAL_TYPE_MINI_MONTH (cal_mini_month_get_type ())
G_DECLARE_FINAL_TYPE (CalMiniMonth, cal_mini_month, CAL, MINI_MONTH, GtkWidget)

struct _CalMiniMonth {
  GtkWidget parent;
  int month;                            /* year × 12 + month − 1 */
  int week_start;
  int first, last;                      /* the days the view shows */
  double drag_y;
  gboolean swiped;
};

G_DEFINE_FINAL_TYPE (CalMiniMonth, cal_mini_month, GTK_TYPE_WIDGET)

enum { DAY_ACTIVATED, N_SIGNALS };
static guint mini_signals[N_SIGNALS], side_signals[N_SIGNALS];

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

/* the first day of the six weeks shown: the week holding the month's first day */
static int
grid_start (CalMiniMonth *self)
{
  int first = month_start (self->month);

  return first - (cal_day_of_week (first) - self->week_start + 7) % 7;
}

typedef struct {
  double x, y, width, row;              /* the weeks' box and a week's height */
} Grid;

static void
grid_of (CalMiniMonth *self, Grid *grid)
{
  double width = gtk_widget_get_width (GTK_WIDGET (self)), height = gtk_widget_get_height (GTK_WIDGET (self));

  grid->x = MONTH_PAD;
  grid->width = MAX (0, width - 2 * MONTH_PAD);
  grid->y = MONTH_TOP + TITLE_HEIGHT + NAMES_HEIGHT;
  grid->row = MAX (0, (height - MONTH_TOP - grid->y) / WEEKS);
}

static void
cal_mini_month_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CalMiniMonth *self = CAL_MINI_MONTH (widget);
  int scale = vibeos_device_scale (widget), start = grid_start (self);
  int today = cal_day_number (g_get_real_time () / G_USEC_PER_SEC);
  GdkRGBA title = cal_argb (CAL_ON_SURFACE_VARIANT), text = cal_argb (CAL_ON_SURFACE);
  GdkRGBA other = cal_argb (CAL_OUTLINE), band = cal_argb (CAL_SECONDARY_CONTAINER);
  GdkRGBA band_text = cal_argb (CAL_ON_SECONDARY_CONTAINER), primary = cal_argb (CAL_PRIMARY);
  g_autofree char *name = cal_format_month_year (month_start (self->month));
  g_autoptr(PangoLayout) title_layout = cal_text_layout (widget, name, TITLE_SIZE, 700);
  double baseline, height, column;
  Grid grid;

  grid_of (self, &grid);
  column = grid.width / 7;

  cal_text_line (TITLE_SIZE, scale, &baseline, &height);
  cal_text_ellipsize (title_layout, grid.width);
  cal_text_draw (snapshot, title_layout, grid.x + grid.width / 2, MONTH_TOP + (TITLE_HEIGHT - height) / 2 + baseline,
                 0.5, &title);

  /* the weekday initials, from the week's first day */
  cal_text_line (NAMES_SIZE, scale, &baseline, &height);
  for (int i = 0; i < 7; i++)
    {
      g_autoptr(GDateTime) date = g_date_time_new_from_unix_local (cal_day_start (start + i));
      g_autofree char *initial = vibeos_format_date (date, "EEEEE");
      g_autofree char *upper = g_utf8_strup (initial, -1);
      g_autoptr(PangoLayout) layout = cal_text_layout (widget, upper, NAMES_SIZE, 700);

      cal_text_draw (snapshot, layout, grid.x + column * (i + 0.5),
                     MONTH_TOP + TITLE_HEIGHT + (NAMES_HEIGHT - height) / 2 + baseline, 0.5, &title);
    }

  /* the days the view shows on a band, one piece per week */
  for (int week = 0; week < WEEKS; week++)
    {
      int from = MAX (self->first, start + 7 * week), to = MIN (self->last, start + 7 * week + 6);

      if (from <= to)
        cal_fill_rect (snapshot, scale, grid.x + column * (from - start - 7 * week), grid.y + grid.row * week,
                       column * (to - from + 1), grid.row, &band);
    }

  for (int i = 0; i < 7 * WEEKS; i++)
    {
      int day = start + i;
      double x = grid.x + column * (i % 7), y = grid.y + grid.row * (i / 7);
      gboolean is_today = day == today, on_band = day >= self->first && day <= self->last;
      GDate date;
      g_autofree char *number = NULL;
      g_autoptr(PangoLayout) layout = NULL;
      const GdkRGBA *color = on_band ? &band_text : month_of (day) == self->month ? &text : &other;

      g_date_clear (&date, 1);
      g_date_set_julian (&date, day);
      number = g_strdup_printf ("%d", g_date_get_day (&date));
      layout = cal_text_layout (widget, number, is_today ? TODAY_SIZE : DAY_SIZE, is_today ? 900 : 700);
      cal_text_line (is_today ? TODAY_SIZE : DAY_SIZE, scale, &baseline, &height);
      if (is_today)
        {
          /* today: framed in the primary colour */
          GskRoundedRect frame;
          float widths[4] = { 2, 2, 2, 2 };
          GdkRGBA colors[4] = { primary, primary, primary, primary };

          gsk_rounded_rect_init_from_rect (&frame, &GRAPHENE_RECT_INIT ((float) x + 1, (float) y + 1,
                                                                        (float) column - 2, (float) grid.row - 2), 4);
          gtk_snapshot_append_border (snapshot, &frame, widths, colors);
          color = &primary;
        }
      cal_text_draw (snapshot, layout, x + column / 2, y + (grid.row - height) / 2 + baseline, 0.5, color);
    }
}

static void
cal_mini_month_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                        int *min_baseline, int *nat_baseline)
{
  *min = *nat = 0;
}

static void
show_month (CalMiniMonth *self, int month)
{
  self->month = month;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_released (GtkGestureClick *gesture, int n, double x, double y, CalMiniMonth *self)
{
  Grid grid;
  int column, row;

  grid_of (self, &grid);
  if (self->swiped || grid.width <= 0 || grid.row <= 0 || y < grid.y || x < grid.x || x >= grid.x + grid.width)
    return;
  column = (int) ((x - grid.x) / (grid.width / 7));
  row = (int) ((y - grid.y) / grid.row);
  if (row >= WEEKS)
    return;
  g_signal_emit (self, mini_signals[DAY_ACTIVATED], 0, grid_start (self) + 7 * row + column);
}

static void
on_drag_begin (GtkGestureDrag *gesture, double x, double y, CalMiniMonth *self)
{
  self->drag_y = 0;
  self->swiped = FALSE;
}

static void
on_drag_update (GtkGestureDrag *gesture, double dx, double dy, CalMiniMonth *self)
{
  /* a month at a time, each time the finger has gone SWIPE further: up shows the next */
  while (dy - self->drag_y <= -SWIPE || dy - self->drag_y >= SWIPE)
    {
      int step = dy - self->drag_y < 0 ? 1 : -1;

      self->drag_y -= step * SWIPE;
      self->swiped = TRUE;
      gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
      show_month (self, self->month + step);
    }
}

static gboolean
on_scroll (GtkEventControllerScroll *scroll, double dx, double dy, CalMiniMonth *self)
{
  if (dy == 0)
    return FALSE;
  show_month (self, self->month + (dy > 0 ? 1 : -1));
  return TRUE;
}

static void
cal_mini_month_class_init (CalMiniMonthClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  widget_class->snapshot = cal_mini_month_snapshot;
  widget_class->measure = cal_mini_month_measure;
  gtk_widget_class_set_css_name (widget_class, "cal-mini-month");
  mini_signals[DAY_ACTIVATED] = g_signal_new ("day-activated", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                              NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
}

static void
cal_mini_month_init (CalMiniMonth *self)
{
  GtkGesture *click = gtk_gesture_click_new ();
  GtkGesture *drag = gtk_gesture_drag_new ();
  GtkEventController *scroll = gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_VERTICAL
                                                                | GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
  int today = cal_day_number (g_get_real_time () / G_USEC_PER_SEC);

  self->month = month_of (today);
  self->first = self->last = today;
  g_signal_connect (click, "released", G_CALLBACK (on_released), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));
  g_signal_connect (scroll, "scroll", G_CALLBACK (on_scroll), self);
  gtk_widget_add_controller (GTK_WIDGET (self), scroll);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Month"), -1);
}

/* --- the side */

struct _CalSide {
  GtkWidget parent;
  CalStore *store;
  GtkWidget *month, *scroll, *list;
  gboolean strip;
};

G_DEFINE_FINAL_TYPE (CalSide, cal_side, GTK_TYPE_WIDGET)

/* a calendar's square: filled when it shows, a frame when it is hidden */
static void
draw_swatch (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  GtkWidget *row = data;
  guint32 rgb = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (row), "cal-color"));
  gboolean visible = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "cal-visible"));

  cairo_set_source_rgb (cr, ((rgb >> 16) & 0xff) / 255.0, ((rgb >> 8) & 0xff) / 255.0, (rgb & 0xff) / 255.0);
  if (visible)
    {
      cairo_rectangle (cr, 0, 0, width, height);
      cairo_fill (cr);
    }
  else
    {
      cairo_set_line_width (cr, FRAME);
      cairo_rectangle (cr, FRAME / 2, FRAME / 2, width - FRAME, height - FRAME);
      cairo_stroke (cr);
    }
}

static void
on_calendar_clicked (VibeosPressable *row, CalSide *self)
{
  const char *uid = g_object_get_data (G_OBJECT (row), "cal-uid");
  const CalCalendar *calendar = cal_store_find_calendar (self->store, uid);

  if (calendar)
    cal_store_set_visible (self->store, uid, !calendar->visible);
}

static void
fill_calendars (CalSide *self)
{
  GPtrArray *calendars = cal_store_get_calendars (self->store);
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (self->list)))
    gtk_box_remove (GTK_BOX (self->list), child);
  for (guint i = 0; calendars && i < calendars->len; i++)
    {
      const CalCalendar *calendar = calendars->pdata[i];
      GtkWidget *content = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      GtkWidget *swatch = gtk_drawing_area_new ();
      GtkWidget *name = gtk_label_new (calendar->name);
      GtkWidget *row = vibeos_pressable_new (content);

      gtk_widget_set_size_request (swatch, SWATCH, SWATCH);
      gtk_widget_set_valign (swatch, GTK_ALIGN_CENTER);
      gtk_widget_set_margin_start (swatch, SWATCH_START);
      gtk_widget_set_margin_end (swatch, NAME_START - SWATCH_START - SWATCH);
      gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (swatch), draw_swatch, row, NULL);
      gtk_box_append (GTK_BOX (content), swatch);
      gtk_label_set_xalign (GTK_LABEL (name), 0);
      gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
      gtk_widget_set_hexpand (name, TRUE);
      gtk_widget_set_valign (name, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class (name, "cal-side-calendar-name");
      gtk_widget_set_margin_end (name, 16);
      gtk_box_append (GTK_BOX (content), name);

      gtk_widget_add_css_class (row, "cal-side-calendar");
      gtk_widget_set_size_request (row, -1, ROW_HEIGHT);
      vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (row), VIBEOS_RIPPLE_BOUNDED, 0);
      g_object_set_data_full (G_OBJECT (row), "cal-uid", g_strdup (calendar->uid), g_free);
      g_object_set_data (G_OBJECT (row), "cal-color", GUINT_TO_POINTER (calendar->color));
      g_object_set_data (G_OBJECT (row), "cal-visible", GINT_TO_POINTER (calendar->visible));
      gtk_accessible_update_state (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_STATE_CHECKED,
                                   calendar->visible ? GTK_ACCESSIBLE_TRISTATE_TRUE : GTK_ACCESSIBLE_TRISTATE_FALSE, -1);
      g_signal_connect (row, "clicked", G_CALLBACK (on_calendar_clicked), self);
      gtk_box_append (GTK_BOX (self->list), row);
    }
}

static void
on_calendars_changed (CalStore *store, CalSide *self)
{
  fill_calendars (self);
}

static void
on_month_day (CalMiniMonth *month, int day, CalSide *self)
{
  g_signal_emit (self, side_signals[DAY_ACTIVATED], 0, day);
}

/* the column: the month MONTH_HEIGHT tall over the calendars; the strip: the month MONTH_WIDTH wide, the calendars
 * beside it */
static void
cal_side_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  CalSide *self = CAL_SIDE (widget);

  if (self->strip)
    {
      int month = MIN (width, (int) MONTH_WIDTH);

      gtk_widget_allocate (self->month, month, height, -1, NULL);
      gtk_widget_allocate (self->scroll, width - month, height, -1,
                           gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (month, 0)));
    }
  else
    {
      int month = MIN (height, (int) MONTH_HEIGHT);

      gtk_widget_allocate (self->month, width, month, -1, NULL);
      gtk_widget_allocate (self->scroll, width, height - month, -1,
                           gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (0, month)));
    }
}

static void
cal_side_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                  int *min_baseline, int *nat_baseline)
{
  *min = *nat = 0;
}

static void
cal_side_dispose (GObject *object)
{
  CalSide *self = CAL_SIDE (object);

  if (self->store)
    g_signal_handlers_disconnect_by_data (self->store, self);
  g_clear_object (&self->store);
  g_clear_pointer (&self->month, gtk_widget_unparent);
  g_clear_pointer (&self->scroll, gtk_widget_unparent);
  G_OBJECT_CLASS (cal_side_parent_class)->dispose (object);
}

static void
cal_side_class_init (CalSideClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = cal_side_dispose;
  widget_class->size_allocate = cal_side_size_allocate;
  widget_class->measure = cal_side_measure;
  gtk_widget_class_set_css_name (widget_class, "cal-side");
  side_signals[DAY_ACTIVATED] = g_signal_new ("day-activated", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                              NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
}

static void
cal_side_init (CalSide *self)
{
  self->month = g_object_new (CAL_TYPE_MINI_MONTH, NULL);
  g_signal_connect (self->month, "day-activated", G_CALLBACK (on_month_day), self);
  gtk_widget_set_parent (self->month, GTK_WIDGET (self));

  self->list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign (self->list, GTK_ALIGN_START);
  gtk_widget_set_margin_top (self->list, LIST_PAD);
  gtk_widget_set_margin_bottom (self->list, LIST_PAD);
  self->scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (self->scroll), self->list);
  gtk_widget_add_css_class (self->scroll, "cal-side-calendars");
  gtk_widget_set_parent (self->scroll, GTK_WIDGET (self));
}

GtkWidget *
cal_side_new (CalStore *store)
{
  CalSide *self = g_object_new (CAL_TYPE_SIDE, NULL);

  self->store = g_object_ref (store);
  g_signal_connect (store, "calendars-changed", G_CALLBACK (on_calendars_changed), self);
  fill_calendars (self);
  return GTK_WIDGET (self);
}

void
cal_side_set_strip (CalSide *self, gboolean strip)
{
  if (strip == self->strip)
    return;
  self->strip = strip;
  if (strip)
    gtk_widget_add_css_class (GTK_WIDGET (self), "strip");
  else
    gtk_widget_remove_css_class (GTK_WIDGET (self), "strip");
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

void
cal_side_set_week_start (CalSide *self, int week_start)
{
  CAL_MINI_MONTH (self->month)->week_start = week_start;
  gtk_widget_queue_draw (self->month);
}

void
cal_side_set_days (CalSide *self, int first, int last)
{
  CalMiniMonth *month = CAL_MINI_MONTH (self->month);

  month->first = first;
  month->last = last;
  show_month (month, month_of (first));
}
