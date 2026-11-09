/* MaterialDatePicker (cal-date-picker.h; spec/calendar.md §8.1). Sizes are the still's, in dp. */
#include "cal-date-picker.h"
#include "cal-colors.h"
#include "cal-format.h"
#include "cal-model.h"
#include "cal-text.h"

#include <glib/gi18n.h>
#include <math.h>
#include <neo.h>
#include <spongy-gtk.h>

#define CARD_WIDTH 328.0
#define CELL 40.0                       /* a day's cell; 4 dp between columns */
#define COLUMN_GAP 4.0
#define DAY_CIRCLE 36.0                 /* the selection and today's ring: the cell inset 2 dp */
#define DAY_TEXT_SIZE 16.0
#define GRID_ROWS 6
#define YEAR_FIRST 1900
#define YEAR_LAST 2100
#define SWIPE_SLOP 8.0

typedef struct {
  NeoDialog *dialog;
  CalDateChosen chosen;
  gpointer data;
  int selected;                 /* the chosen day */
  int month;                    /* the month shown: year · 12 + month − 1 */
  int week_start;
  GtkWidget *selection, *month_label, *stack, *grid, *years, *prev, *next;
} Picker;

/* --- months */

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
month_days (int month)
{
  return g_date_get_days_in_month (month % 12 + 1, month / 12);
}

/* --- the grid of days: the weekday initials, then the month's days */

#define CAL_TYPE_DAY_GRID (cal_day_grid_get_type ())
G_DECLARE_FINAL_TYPE (CalDayGrid, cal_day_grid, CAL, DAY_GRID, GtkWidget)

struct _CalDayGrid {
  GtkWidget parent;
  Picker *picker;
  double drag_x;
  gboolean swiping;
};

G_DEFINE_FINAL_TYPE (CalDayGrid, cal_day_grid, GTK_TYPE_WIDGET)

static void update (Picker *picker);

static double
column_x (int column)
{
  return column * (CELL + COLUMN_GAP);
}

static void
cal_day_grid_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CalDayGrid *self = CAL_DAY_GRID (widget);
  Picker *picker = self->picker;
  int scale = neo_device_scale (widget), first = month_start (picker->month), n = month_days (picker->month);
  int offset = (cal_day_of_week (first) - picker->week_start + 7) % 7;
  int today = cal_day_number (g_get_real_time () / G_USEC_PER_SEC);
  GdkRGBA text = cal_argb (CAL_ON_SURFACE), primary = cal_argb (CAL_PRIMARY), on_primary = cal_argb (CAL_ON_PRIMARY);
  double baseline, height;

  cal_text_line (DAY_TEXT_SIZE, scale, &baseline, &height);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT ((float) self->drag_x, 0));
  /* the weekday initials ("S M T W T F S"), from the locale's first day */
  for (int i = 0; i < 7; i++)
    {
      g_autoptr(GDateTime) date = g_date_time_new_from_unix_local (cal_day_start (first - offset + i));
      g_autofree char *name = g_date_time_format (date, "%a");
      g_autofree char *initial = g_utf8_substring (name, 0, 1);
      g_autofree char *upper = g_utf8_strup (initial, -1);
      g_autoptr(PangoLayout) layout = cal_text_layout (widget, upper, DAY_TEXT_SIZE, 700);

      cal_text_draw (snapshot, layout, column_x (i) + CELL / 2, (CELL - height) / 2 + baseline, 0.5, &text);
    }
  for (int d = 0; d < n; d++)
    {
      int cell = offset + d, row = cell / 7, column = cell % 7, day = first + d;
      double x = column_x (column), y = CELL * (row + 1);
      g_autofree char *number = g_strdup_printf ("%d", d + 1);
      g_autoptr(PangoLayout) layout = cal_text_layout (widget, number, DAY_TEXT_SIZE, 700);
      const GdkRGBA *color = &text;
      graphene_rect_t circle = GRAPHENE_RECT_INIT ((float) (x + (CELL - DAY_CIRCLE) / 2),
                                                   (float) (y + (CELL - DAY_CIRCLE) / 2), DAY_CIRCLE, DAY_CIRCLE);
      GskRoundedRect round;

      gsk_rounded_rect_init_from_rect (&round, &circle, DAY_CIRCLE / 2);
      if (day == picker->selected)
        {
          /* the choice: filled colorPrimary, its number onPrimary */
          gtk_snapshot_push_rounded_clip (snapshot, &round);
          gtk_snapshot_append_color (snapshot, &primary, &circle);
          gtk_snapshot_pop (snapshot);
          color = &on_primary;
        }
      else if (day == today)
        {
          /* today: a 1 dp ring and its number in colorPrimary */
          float widths[4] = { 1, 1, 1, 1 };
          GdkRGBA colors[4] = { primary, primary, primary, primary };

          gtk_snapshot_append_border (snapshot, &round, widths, colors);
          color = &primary;
        }
      cal_text_draw (snapshot, layout, x + CELL / 2, y + (CELL - height) / 2 + baseline, 0.5, color);
    }
}

static void
cal_day_grid_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                      int *min_baseline, int *nat_baseline)
{
  *min = *nat = orientation == GTK_ORIENTATION_HORIZONTAL ? (int) (7 * CELL + 6 * COLUMN_GAP)
                                                          : (int) ((GRID_ROWS + 1) * CELL);
}

static void
show_month (Picker *picker, int month)
{
  picker->month = CLAMP (month, YEAR_FIRST * 12, YEAR_LAST * 12 + 11);
  update (picker);
}

static void
on_grid_released (GtkGestureClick *gesture, int n, double x, double y, CalDayGrid *self)
{
  Picker *picker = self->picker;
  int first = month_start (picker->month), offset = (cal_day_of_week (first) - picker->week_start + 7) % 7;
  int column = (int) floor (x / (CELL + COLUMN_GAP)), row = (int) floor (y / CELL) - 1, d;

  if (self->swiping || row < 0 || column < 0 || column > 6 || x - column_x (column) > CELL)
    return;
  d = row * 7 + column - offset;
  if (d < 0 || d >= month_days (picker->month))
    return;
  picker->selected = first + d;
  update (picker);
}

static void
on_grid_drag_begin (GtkGestureDrag *gesture, double x, double y, CalDayGrid *self)
{
  self->swiping = FALSE;
}

static void
on_grid_drag_update (GtkGestureDrag *gesture, double dx, double dy, CalDayGrid *self)
{
  if (!self->swiping)
    {
      if (fabs (dx) < SWIPE_SLOP || fabs (dx) < fabs (dy))
        return;
      self->swiping = TRUE;
      gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    }
  self->drag_x = dx;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_grid_drag_end (GtkGestureDrag *gesture, double dx, double dy, CalDayGrid *self)
{
  /* the months page (ViewPager2): a swipe past a third of the width goes to the next or previous one */
  if (self->swiping && fabs (dx) > gtk_widget_get_width (GTK_WIDGET (self)) / 3)
    show_month (self->picker, self->picker->month + (dx < 0 ? 1 : -1));
  self->drag_x = 0;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
cal_day_grid_class_init (CalDayGridClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  widget_class->snapshot = cal_day_grid_snapshot;
  widget_class->measure = cal_day_grid_measure;
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_GRID);
}

static void
cal_day_grid_init (CalDayGrid *self)
{
  GtkGesture *click = gtk_gesture_click_new ();
  GtkGesture *drag = gtk_gesture_drag_new ();

  gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);
  g_signal_connect (click, "released", G_CALLBACK (on_grid_released), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_grid_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_grid_drag_update), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_grid_drag_end), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));
}

/* --- the dialog */

static void
update (Picker *picker)
{
  g_autofree char *selection = cal_format_picker_date (picker->selected);
  g_autofree char *month = cal_format_month_year (month_start (picker->month));
  g_autofree char *current = g_strdup_printf (_("Current selection: %s"), selection);

  gtk_label_set_label (GTK_LABEL (picker->selection), selection);
  gtk_accessible_update_property (GTK_ACCESSIBLE (picker->selection), GTK_ACCESSIBLE_PROPERTY_LABEL, current, -1);
  gtk_label_set_label (GTK_LABEL (picker->month_label), month);
  gtk_widget_queue_draw (picker->grid);
}

static void
on_prev (NeoPressable *button, Picker *picker)
{
  show_month (picker, picker->month - 1);
}

static void
on_next (NeoPressable *button, Picker *picker)
{
  show_month (picker, picker->month + 1);
}

static void
on_year (NeoPressable *button, Picker *picker)
{
  int year = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "cal-year"));

  /* the year list: the same month in that year, back to the days */
  show_month (picker, year * 12 + picker->month % 12);
  gtk_stack_set_visible_child_name (GTK_STACK (picker->stack), "days");
  gtk_widget_set_visible (picker->prev, TRUE);
  gtk_widget_set_visible (picker->next, TRUE);
}

/* the year shown in view, rows of three, 52 dp each: once the list has its size (scrolled before it is laid out,
 * the view clamps the offset to the top) */
static gboolean
scroll_to_year (GtkWidget *years, GdkFrameClock *clock, gpointer data)
{
  Picker *picker = data;

  if (gtk_widget_get_height (years) <= 0)
    return G_SOURCE_CONTINUE;
  spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (years),
                                MAX (0, ((picker->month / 12 - YEAR_FIRST) / 3 - 1) * 52.0));
  return G_SOURCE_REMOVE;
}

static void
on_month_toggle (NeoPressable *button, Picker *picker)
{
  gboolean years = g_str_equal (gtk_stack_get_visible_child_name (GTK_STACK (picker->stack)), "days");

  gtk_stack_set_visible_child_name (GTK_STACK (picker->stack), years ? "years" : "days");
  gtk_widget_set_visible (picker->prev, !years);
  gtk_widget_set_visible (picker->next, !years);
  if (years)
    gtk_widget_add_tick_callback (picker->years, scroll_to_year, picker, NULL);
}

static void
on_response (NeoDialog *dialog, int response, Picker *picker)
{
  if (response == NEO_RESPONSE_OK)
    picker->chosen (picker->selected, picker->data);
}

/* The year list is as tall as the days (six rows and the weekday row) and scrolls the rest. A size request alone
 * no longer does it: NeoDialog wraps its content's natural height, and the list's is all of its 67 rows, which
 * would stretch the card to the window. Its holder asks for that one height, natural and minimum. */
static void
years_measure (GtkWidget *holder, GtkOrientation orientation, int for_size, int *min, int *nat, int *min_base,
               int *nat_base)
{
  if (orientation == GTK_ORIENTATION_VERTICAL)
    *min = *nat = (int) ((GRID_ROWS + 1) * CELL);
  else
    gtk_widget_measure (gtk_widget_get_first_child (holder), orientation, for_size, min, nat, NULL, NULL);
}

static void
years_allocate (GtkWidget *holder, int width, int height, int baseline)
{
  gtk_widget_allocate (gtk_widget_get_first_child (holder), width, height, baseline, NULL);
}

/* the years page: picker->years is the list, the page its holder */
static GtkWidget *
years_list (Picker *picker)
{
  GtkWidget *grid = gtk_grid_new ();
  GtkWidget *scroll = spongy_scroll_view_new ();
  int this_year = month_of (cal_day_number (g_get_real_time () / G_USEC_PER_SEC)) / 12;

  gtk_grid_set_column_homogeneous (GTK_GRID (grid), TRUE);
  for (int year = YEAR_FIRST; year <= YEAR_LAST; year++)
    {
      g_autofree char *text = g_strdup_printf ("%d", year);
      GtkWidget *label = gtk_label_new (text);
      GtkWidget *button = neo_pressable_new (label);

      gtk_widget_add_css_class (button, "cal-year");
      gtk_widget_set_halign (button, GTK_ALIGN_CENTER);
      /* YearGridAdapter: the year of the chosen day filled, this year outlined */
      if (year == month_of (picker->selected) / 12)
        gtk_widget_add_css_class (button, "selected");
      else if (year == this_year)
        gtk_widget_add_css_class (button, "today");
      neo_pressable_set_ripple (NEO_PRESSABLE (button), NEO_RIPPLE_BOUNDED, 18);
      g_object_set_data (G_OBJECT (button), "cal-year", GINT_TO_POINTER (year));
      g_signal_connect (button, "clicked", G_CALLBACK (on_year), picker);
      gtk_grid_attach (GTK_GRID (grid), button, (year - YEAR_FIRST) % 3, (year - YEAR_FIRST) / 3, 1, 1);
    }
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), grid);
  picker->years = scroll;
  {
    GtkWidget *holder = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

    gtk_widget_set_layout_manager (holder, gtk_custom_layout_new (NULL, years_measure, years_allocate));
    gtk_box_append (GTK_BOX (holder), scroll);
    return holder;
  }
}

static void
picker_free (gpointer data, GClosure *closure)
{
  g_free (data);
}

void
cal_date_picker_show (GtkWidget *parent, const char *title, int day, int week_start, CalDateChosen chosen,
                      gpointer data)
{
  Picker *picker = g_new0 (Picker, 1);
  NeoDialog *dialog = neo_dialog_new (NULL);
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *title_label = gtk_label_new (title);
  GtkWidget *selection_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *nav = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *toggle_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *toggle, *arrow;
  CalDayGrid *grid;

  picker->dialog = dialog;
  picker->chosen = chosen;
  picker->data = data;
  picker->selected = day;
  picker->month = month_of (day);
  picker->week_start = week_start;

  /* the header: the title, then the chosen date large. The pencil (text input mode) is left out: not built. */
  gtk_widget_add_css_class (header, "cal-date-header");
  gtk_label_set_xalign (GTK_LABEL (title_label), 0);
  gtk_widget_add_css_class (title_label, "cal-date-title");
  gtk_box_append (GTK_BOX (header), title_label);
  picker->selection = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (picker->selection), 0);
  gtk_widget_set_hexpand (picker->selection, TRUE);
  gtk_widget_set_valign (picker->selection, GTK_ALIGN_END);
  gtk_widget_set_vexpand (selection_row, TRUE);
  gtk_widget_add_css_class (picker->selection, "cal-date-selection");
  gtk_box_append (GTK_BOX (selection_row), picker->selection);
  gtk_box_append (GTK_BOX (header), selection_row);
  /* no divider under it: MaterialDatePicker draws one in its fullscreen mode only (none on still:cal-dlg-date) */
  gtk_box_append (GTK_BOX (content), header);

  /* the month: "November 2026 ▾" (the year list), ‹ › */
  gtk_widget_add_css_class (nav, "cal-date-nav");
  picker->month_label = gtk_label_new (NULL);
  gtk_widget_add_css_class (picker->month_label, "cal-date-month");
  arrow = gtk_image_new_from_icon_name ("cal-drop-down-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (arrow), 24);
  gtk_box_append (GTK_BOX (toggle_box), picker->month_label);
  gtk_box_append (GTK_BOX (toggle_box), arrow);
  toggle = neo_pressable_new (toggle_box);
  gtk_widget_add_css_class (toggle, "cal-date-month-toggle");
  neo_pressable_set_ripple (NEO_PRESSABLE (toggle), NEO_RIPPLE_BOUNDED, 24);
  gtk_accessible_update_property (GTK_ACCESSIBLE (toggle), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
                                  _("Switch to year view"), -1);
  g_signal_connect (toggle, "clicked", G_CALLBACK (on_month_toggle), picker);
  gtk_widget_set_hexpand (toggle, FALSE);
  gtk_box_append (GTK_BOX (nav), toggle);
  {
    GtkWidget *space = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

    gtk_widget_set_hexpand (space, TRUE);
    gtk_box_append (GTK_BOX (nav), space);
  }
  picker->prev = neo_icon_button_new ("cal-chevron-left-symbolic");
  gtk_widget_add_css_class (picker->prev, "cal-date-arrow");
  gtk_accessible_update_property (GTK_ACCESSIBLE (picker->prev), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _("Change to previous month"), -1);
  g_signal_connect (picker->prev, "clicked", G_CALLBACK (on_prev), picker);
  gtk_box_append (GTK_BOX (nav), picker->prev);
  picker->next = neo_icon_button_new ("cal-chevron-right-symbolic");
  gtk_widget_add_css_class (picker->next, "cal-date-arrow");
  gtk_accessible_update_property (GTK_ACCESSIBLE (picker->next), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _("Change to next month"), -1);
  g_signal_connect (picker->next, "clicked", G_CALLBACK (on_next), picker);
  gtk_box_append (GTK_BOX (nav), picker->next);
  gtk_box_append (GTK_BOX (content), nav);

  grid = g_object_new (CAL_TYPE_DAY_GRID, NULL);
  grid->picker = picker;
  picker->grid = GTK_WIDGET (grid);
  gtk_widget_set_halign (picker->grid, GTK_ALIGN_CENTER);
  picker->stack = gtk_stack_new ();
  gtk_stack_add_named (GTK_STACK (picker->stack), picker->grid, "days");
  gtk_stack_add_named (GTK_STACK (picker->stack), years_list (picker), "years");
  gtk_widget_add_css_class (picker->stack, "cal-date-days");
  gtk_box_append (GTK_BOX (content), picker->stack);

  gtk_widget_set_size_request (content, (int) CARD_WIDTH, -1);
  gtk_widget_add_css_class (content, "cal-date-picker");
  neo_dialog_set_flush (dialog, TRUE);
  neo_dialog_set_content (dialog, content);
  /* the card is the content's parent: its own class takes the M3 picker's colour and button band */
  gtk_widget_add_css_class (gtk_widget_get_parent (content), "cal-date-card");
  neo_dialog_add_button (dialog, _("Cancel"), NEO_RESPONSE_CANCEL);
  neo_dialog_add_button (dialog, _("OK"), NEO_RESPONSE_OK);
  g_signal_connect_data (dialog, "response", G_CALLBACK (on_response), picker, picker_free, 0);
  update (picker);
  neo_dialog_present (dialog, parent);
}
