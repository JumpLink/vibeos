/* CalcHistory: see calc-history.h.
 *
 * A row is its top padding, the divider with padding around it (nothing for the oldest row, blank space when the
 * row has no date), the date, the expression and the result; the list keeps some padding under the newest row.
 * Row heights are worked out in whole device pixels. The toolbar: the back arrow, then the title with its box
 * centred in the bar. */
#include "calc-history.h"
#include "calc-formula.h"
#include "calc-text.h"

#include <glib/gi18n.h>
#include <math.h>
#include <vibeos.h>
#include <spongy-gtk.h>

#define TOOLBAR        64
#define DIVIDER_PAD    14     /* above and below a row's divider */
#define DIVIDER        1      /* the divider line */
#define TEXT_SIZE      32     /* the expression and the result */
#define DATE_SIZE      14
#define TITLE_SIZE     22     /* the toolbar's title */
#define TITLE_X        60
#define ARROW_X        83.5   /* device px: the arrow's centre from the start */
#define ICON_Y         95.5   /* device px: the icons' centres from the top */
#define OVERFLOW_X     66.5   /* px from the end */
#define BUTTON         48
#define EMPTY_SIZE     20     /* the empty history's text */
#define EMPTY_ICON     24

/* ------------------------------------------------------------------------------------------------------------ */
/* the date */

char *
calc_history_date_string (gint64 time_ms, gint64 now_ms)
{
  g_autoptr (GDateTime) time = g_date_time_new_from_unix_local (time_ms / 1000);
  g_autoptr (GDateTime) now = g_date_time_new_from_unix_local (now_ms / 1000);
  gint64 duration = ABS (now_ms - time_ms);

  if (!time || !now)
    return g_strdup ("");
  if (duration < 7 * 24 * 3600 * (gint64) 1000) {
    /* the days between the two dates, in local time */
    GDate a, b;
    int days;

    g_date_set_dmy (&a, g_date_time_get_day_of_month (time), g_date_time_get_month (time),
                    g_date_time_get_year (time));
    g_date_set_dmy (&b, g_date_time_get_day_of_month (now), g_date_time_get_month (now), g_date_time_get_year (now));
    days = g_date_days_between (&a, &b);
    if (days == 0)
      return g_strdup (_("Today"));
    if (days == 1)
      return g_strdup (_("Yesterday"));
    if (days == -1)
      return g_strdup (_("Tomorrow"));
    if (days > 0)
      return g_strdup_printf (g_dngettext (NULL, "%d day ago", "%d days ago", days), days);
    return g_strdup_printf (g_dngettext (NULL, "In %d day", "In %d days", -days), -days);
  }
  if (g_date_time_get_year (time) != g_date_time_get_year (now))
    return g_date_time_format (time, "%b %-d, %Y");
  return g_date_time_format (time, "%b %-d");
}

/* ------------------------------------------------------------------------------------------------------------ */
/* a row */

typedef enum {
  DIVIDER_VISIBLE,
  DIVIDER_INVISIBLE,
  DIVIDER_GONE,
} DividerState;

#define CALC_TYPE_HISTORY_ROW (calc_history_row_get_type ())
G_DECLARE_FINAL_TYPE (CalcHistoryRow, calc_history_row, CALC, HISTORY_ROW, GtkWidget)

struct _CalcHistoryRow {
  GtkWidget    parent;
  Evaluator   *evaluator;
  long         index;
  char        *date;           /* NULL: no header */
  DividerState divider;
  PangoLayout *date_layout;
  GtkWidget   *formula;
  GtkWidget   *result;
};

G_DEFINE_FINAL_TYPE (CalcHistoryRow, calc_history_row, GTK_TYPE_WIDGET)

static int
row_divider_px (DividerState divider, int scale)
{
  return divider == DIVIDER_GONE ? 0 : (2 * DIVIDER_PAD + DIVIDER) * scale;
}

static int
date_px (int scale)
{
  double px = DATE_SIZE * scale;

  /* with font padding: the font's yMax (floored) to yMin (rounded up) */
  return (int) (ceil (CALC_BOTTOM * px) - floor (-CALC_TOP * px));
}

static int
text_px (int scale)
{
  return (int) round (calc_text_view_height (8, 16, TEXT_SIZE, scale) * scale);
}

static int
row_height_px (gboolean header, DividerState divider, int scale)
{
  return DIVIDER_PAD * scale + row_divider_px (divider, scale) + (header ? date_px (scale) : 0) + 2 * text_px (scale);
}

static void
calc_history_row_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                          int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 0;
}

static void
calc_history_row_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  CalcHistoryRow *self = CALC_HISTORY_ROW (widget);
  int scale = vibeos_device_scale (widget);
  int y = DIVIDER_PAD * scale + row_divider_px (self->divider, scale) + (self->date ? date_px (scale) : 0);
  double text = (double) text_px (scale) / scale;

  calc_formula_set_exact_height (CALC_FORMULA (self->formula), text);
  gtk_widget_allocate (self->formula, width, (int) ceil (text - 0.001), -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (0, (double) y / scale)));
  calc_result_set_exact_height (CALC_RESULT (self->result), text);
  gtk_widget_allocate (self->result, width, (int) ceil (text - 0.001), -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (0, (double) y / scale + text)));
}

static void
calc_history_row_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CalcHistoryRow *self = CALC_HISTORY_ROW (widget);
  int scale = vibeos_device_scale (widget);
  double w = gtk_widget_get_width (widget);
  double y = DIVIDER_PAD;

  if (self->divider == DIVIDER_VISIBLE) {
    /* a faint line of the text colour */
    GdkRGBA divider = vibeos_color_alpha (VIBEOS_ON_SURFACE, 0x1f / 255.0);

    gtk_snapshot_append_color (snapshot, &divider, &GRAPHENE_RECT_INIT (0, y + DIVIDER_PAD, w, DIVIDER));
  }
  y += (double) row_divider_px (self->divider, scale) / scale;
  if (self->date) {
    GdkRGBA color = vibeos_color (VIBEOS_ON_SURFACE_VARIANT);
    double top = floor (-CALC_TOP * DATE_SIZE * scale);

    if (!self->date_layout)
      self->date_layout = calc_text_layout (widget, self->date, DATE_SIZE, CALC_WEIGHT_MEDIUM);
    /* in line with the expression and the result */
    calc_text_draw (snapshot, self->date_layout, 16, y - top / scale, &color);
  }
  gtk_widget_snapshot_child (widget, self->formula, snapshot);
  gtk_widget_snapshot_child (widget, self->result, snapshot);
}

static void
calc_history_row_dispose (GObject *object)
{
  CalcHistoryRow *self = CALC_HISTORY_ROW (object);

  /* the row's computation is not needed any more */
  if (self->evaluator && self->result)
    evaluator_cancel (self->evaluator, self->index, TRUE);
  g_clear_pointer (&self->formula, gtk_widget_unparent);
  g_clear_pointer (&self->result, gtk_widget_unparent);
  g_clear_object (&self->date_layout);
  G_OBJECT_CLASS (calc_history_row_parent_class)->dispose (object);
}

static void
calc_history_row_finalize (GObject *object)
{
  g_free (CALC_HISTORY_ROW (object)->date);
  G_OBJECT_CLASS (calc_history_row_parent_class)->finalize (object);
}

static void
calc_history_row_class_init (CalcHistoryRowClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = calc_history_row_dispose;
  G_OBJECT_CLASS (klass)->finalize = calc_history_row_finalize;
  widget_class->measure = calc_history_row_measure;
  widget_class->size_allocate = calc_history_row_size_allocate;
  widget_class->snapshot = calc_history_row_snapshot;
  gtk_widget_class_set_css_name (widget_class, "calc-history-row");
}

static void
calc_history_row_init (CalcHistoryRow *self)
{
}

static GtkWidget *
calc_history_row_new (Evaluator *evaluator, long index, const char *date, DividerState divider)
{
  CalcHistoryRow *self = g_object_new (CALC_TYPE_HISTORY_ROW, NULL);
  g_autofree char *formula = calc_expr_to_string (evaluator_get_expr (evaluator, index));

  self->evaluator = evaluator;
  self->index = index;
  self->date = g_strdup (date);
  self->divider = divider;
  /* one fixed size: a history row does not shrink its text to fit, as the display does */
  self->formula = calc_formula_new (TEXT_SIZE, TEXT_SIZE, 0);
  calc_formula_set_padding (CALC_FORMULA (self->formula), 8, 16, 16, 16);
  calc_formula_set_scroll_to_end (CALC_FORMULA (self->formula), FALSE);
  calc_formula_set_text (CALC_FORMULA (self->formula), formula, NULL);
  gtk_widget_set_parent (self->formula, GTK_WIDGET (self));
  self->result = calc_result_new (TEXT_SIZE);
  calc_result_set_padding (CALC_RESULT (self->result), 8, 16, 16, 16);
  /* the default request, SHOULD_REQUIRE, with the result as its own listener */
  calc_result_set_evaluator (CALC_RESULT (self->result), evaluator, index);
  gtk_widget_set_parent (self->result, GTK_WIDGET (self));
  return GTK_WIDGET (self);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* the list: a GtkScrollable for SpongyScrollView, laying out only the rows in view */

#define CALC_TYPE_HISTORY_LIST (calc_history_list_get_type ())
G_DECLARE_FINAL_TYPE (CalcHistoryList, calc_history_list, CALC, HISTORY_LIST, GtkWidget)

typedef struct {
  gboolean     header;
  DividerState divider;
  int          height;          /* px */
  int          offset;          /* px from the list's end (its bottom padding included) to the row's bottom */
} RowInfo;

struct _CalcHistoryList {
  GtkWidget      parent;
  Evaluator     *evaluator;
  long           max_index;
  GArray        *info;          /* RowInfo by position: 0 the newest */
  int            total;         /* px, the rows and the bottom padding */
  int            scale;         /* the device scale the heights are for; 0: not worked out yet */
  GHashTable    *rows;          /* position -> CalcHistoryRow */
  GtkAdjustment *vadjustment, *hadjustment;
  double         from_end;      /* how far it is scrolled up from its end, px */
  gboolean       configuring;
};

enum { PROP_0, PROP_HADJUSTMENT, PROP_VADJUSTMENT, PROP_HSCROLL_POLICY, PROP_VSCROLL_POLICY };

G_DEFINE_FINAL_TYPE_WITH_CODE (CalcHistoryList, calc_history_list, GTK_TYPE_WIDGET,
                               G_IMPLEMENT_INTERFACE (GTK_TYPE_SCROLLABLE, NULL))

static guint list_signals[1];

static double
content_height (CalcHistoryList *self, double view, int scale)
{
  return MAX ((double) self->total / scale, view);
}

static void
on_row_long_pressed (CalcResult *result, CalcHistoryList *self)
{
  g_signal_emit (self, list_signals[0], 0, result);
}

/* the rows' heights and places, in device pixels */
static void
ensure_metrics (CalcHistoryList *self, int scale)
{
  int offset = DIVIDER_PAD * scale;   /* the list's bottom padding */

  if (self->scale == scale)
    return;
  for (guint p = 0; p < self->info->len; p++) {
    RowInfo *info = &g_array_index (self->info, RowInfo, p);

    info->height = row_height_px (info->header, info->divider, scale);
    info->offset = offset;
    offset += info->height;
  }
  self->total = offset;
  self->scale = scale;
}

static void
calc_history_list_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  CalcHistoryList *self = CALC_HISTORY_LIST (widget);
  int scale = vibeos_device_scale (widget);
  double content, value;
  GHashTableIter iter;
  gpointer key, row;

  ensure_metrics (self, scale);
  content = content_height (self, height, scale);
  value = MAX (0, content - height - self->from_end);
  self->from_end = content - height - value;
  if (self->vadjustment) {
    self->configuring = TRUE;
    gtk_adjustment_configure (self->vadjustment, value, 0, content, height * 0.1, height * 0.9, height);
    self->configuring = FALSE;
  }
  /* the rows that show: the content's bottom is `content`, a row's bottom `offset` px above it */
  for (guint p = 0; p < self->info->len; p++) {
    RowInfo *info = &g_array_index (self->info, RowInfo, p);
    double bottom = content - (double) info->offset / scale - value;
    double top = bottom - (double) info->height / scale;
    GtkWidget *child;

    if (bottom < 0)
      break;   /* this and every older row is above the view */
    child = g_hash_table_lookup (self->rows, GUINT_TO_POINTER (p));
    if (top > height) {
      if (child) {
        g_hash_table_remove (self->rows, GUINT_TO_POINTER (p));
      }
      continue;
    }
    if (!child) {
      long index = self->max_index - p;
      g_autofree char *date = NULL;
      CalcHistoryRow *r;

      if (info->header)
        date = calc_history_date_string (evaluator_get_time_stamp (self->evaluator, index),
                                         g_get_real_time () / 1000);
      child = calc_history_row_new (self->evaluator, index, date, info->divider);
      r = CALC_HISTORY_ROW (child);
      g_signal_connect (r->result, "long-pressed", G_CALLBACK (on_row_long_pressed), self);
      gtk_widget_set_parent (child, widget);
      g_hash_table_insert (self->rows, GUINT_TO_POINTER (p), child);
    }
    gtk_widget_allocate (child, width, (int) ceil ((double) info->height / scale - 0.001), -1,
                         gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (0, top)));
  }
  /* the rows scrolled out above */
  g_hash_table_iter_init (&iter, self->rows);
  while (g_hash_table_iter_next (&iter, &key, &row)) {
    RowInfo *info = &g_array_index (self->info, RowInfo, GPOINTER_TO_UINT (key));
    double bottom = content - (double) info->offset / scale - value;

    if (bottom < 0)
      g_hash_table_iter_remove (&iter);
  }
}

static void
calc_history_list_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                           int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 0;
}

static void
calc_history_list_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  /* the rows draw into the bottom padding, but not past the list */
  gtk_snapshot_push_clip (snapshot, &GRAPHENE_RECT_INIT (0, 0, gtk_widget_get_width (widget),
                                                         gtk_widget_get_height (widget)));
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child; child = gtk_widget_get_next_sibling (child))
    gtk_widget_snapshot_child (widget, child, snapshot);
  gtk_snapshot_pop (snapshot);
}

static void
on_value_changed (GtkAdjustment *adjustment, CalcHistoryList *self)
{
  if (self->configuring)
    return;
  self->from_end = gtk_adjustment_get_upper (adjustment) - gtk_adjustment_get_page_size (adjustment)
                   - gtk_adjustment_get_value (adjustment);
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

static void
set_vadjustment (CalcHistoryList *self, GtkAdjustment *adjustment)
{
  if (self->vadjustment) {
    g_signal_handlers_disconnect_by_func (self->vadjustment, on_value_changed, self);
    g_clear_object (&self->vadjustment);
  }
  if (adjustment) {
    self->vadjustment = g_object_ref_sink (adjustment);
    g_signal_connect (adjustment, "value-changed", G_CALLBACK (on_value_changed), self);
  }
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

static void
calc_history_list_set_property (GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
  CalcHistoryList *self = CALC_HISTORY_LIST (object);

  switch (id) {
  case PROP_VADJUSTMENT:
    set_vadjustment (self, g_value_get_object (value));
    break;
  case PROP_HADJUSTMENT:
    g_clear_object (&self->hadjustment);
    if (g_value_get_object (value))
      self->hadjustment = g_object_ref_sink (g_value_get_object (value));
    break;
  case PROP_HSCROLL_POLICY:
  case PROP_VSCROLL_POLICY:
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, pspec);
  }
}

static void
calc_history_list_get_property (GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  CalcHistoryList *self = CALC_HISTORY_LIST (object);

  switch (id) {
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
calc_history_list_dispose (GObject *object)
{
  CalcHistoryList *self = CALC_HISTORY_LIST (object);

  g_clear_pointer (&self->rows, g_hash_table_unref);
  set_vadjustment (self, NULL);
  g_clear_object (&self->hadjustment);
  g_clear_pointer (&self->info, g_array_unref);
  G_OBJECT_CLASS (calc_history_list_parent_class)->dispose (object);
}

static void
calc_history_list_class_init (CalcHistoryListClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = calc_history_list_dispose;
  object_class->set_property = calc_history_list_set_property;
  object_class->get_property = calc_history_list_get_property;
  widget_class->measure = calc_history_list_measure;
  widget_class->size_allocate = calc_history_list_size_allocate;
  widget_class->snapshot = calc_history_list_snapshot;
  g_object_class_override_property (object_class, PROP_HADJUSTMENT, "hadjustment");
  g_object_class_override_property (object_class, PROP_VADJUSTMENT, "vadjustment");
  g_object_class_override_property (object_class, PROP_HSCROLL_POLICY, "hscroll-policy");
  g_object_class_override_property (object_class, PROP_VSCROLL_POLICY, "vscroll-policy");
  gtk_widget_class_set_css_name (widget_class, "calc-history-list");
  list_signals[0] = g_signal_new ("result-long-pressed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                  NULL, NULL, G_TYPE_NONE, 1, CALC_TYPE_RESULT);
}

static void
unparent_row (gpointer row)
{
  gtk_widget_unparent (row);
}

static void
calc_history_list_init (CalcHistoryList *self)
{
  self->info = g_array_new (FALSE, FALSE, sizeof (RowInfo));
  self->rows = g_hash_table_new_full (NULL, NULL, NULL, unparent_row);
}

static gboolean
same_day (gint64 a_ms, gint64 b_ms)
{
  g_autoptr (GDateTime) a = g_date_time_new_from_unix_local (a_ms / 1000);
  g_autoptr (GDateTime) b = g_date_time_new_from_unix_local (b_ms / 1000);

  return a && b && g_date_time_get_year (a) == g_date_time_get_year (b)
         && g_date_time_get_day_of_year (a) == g_date_time_get_day_of_year (b);
}

/* one row per history entry, newest first; which ones start a day */
static GtkWidget *
calc_history_list_new (Evaluator *evaluator)
{
  CalcHistoryList *self = g_object_new (CALC_TYPE_HISTORY_LIST, NULL);
  long count;

  self->evaluator = evaluator;
  self->max_index = evaluator_get_max_index (evaluator);
  count = MAX (0, self->max_index);
  for (long p = 0; p < count; p++) {
    long index = self->max_index - p;
    RowInfo info;

    /* the oldest row has a date; any other when the next older one is of another day */
    info.header = p == count - 1
                  || !same_day (evaluator_get_time_stamp (evaluator, index),
                                evaluator_get_time_stamp (evaluator, index - 1));
    info.divider = info.header ? (p == count - 1 ? DIVIDER_GONE : DIVIDER_VISIBLE) : DIVIDER_INVISIBLE;
    info.height = info.offset = 0;
    g_array_append_val (self->info, info);
  }
  return GTK_WIDGET (self);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* the history */

struct _CalcHistory {
  GtkWidget    parent;
  Evaluator   *evaluator;
  GtkWidget   *back;
  GtkWidget   *overflow;
  GtkWidget   *scroll;          /* SpongyScrollView, or NULL when empty */
  GtkWidget   *list;
  PangoLayout *title;
  PangoLayout *empty;
  double       exact_width, exact_height;
};

G_DEFINE_FINAL_TYPE (CalcHistory, calc_history, GTK_TYPE_WIDGET)

enum { BACK, MENU, RESULT_LONG_PRESSED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
calc_history_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                      int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 0;
}

static double
history_width (CalcHistory *self)
{
  return self->exact_width > 0 ? self->exact_width : gtk_widget_get_width (GTK_WIDGET (self));
}

static double
history_height (CalcHistory *self)
{
  return self->exact_height > 0 ? self->exact_height : gtk_widget_get_height (GTK_WIDGET (self));
}

static void
calc_history_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  CalcHistory *self = CALC_HISTORY (widget);
  int scale = vibeos_device_scale (widget);
  double w = history_width (self), h = history_height (self);

  gtk_widget_allocate (self->back, BUTTON, BUTTON, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (ARROW_X / scale - BUTTON / 2.0,
                                                                            ICON_Y / scale - BUTTON / 2.0)));
  gtk_widget_allocate (self->overflow, BUTTON, BUTTON, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT ((w * scale - OVERFLOW_X) / scale
                                                                            - BUTTON / 2.0,
                                                                            ICON_Y / scale - BUTTON / 2.0)));
  if (self->scroll) {
    /* the list: under the toolbar, as tall as the frame leaves it (the frame grows as the history
     * comes down; the list keeps its end at the bottom) */
    double list_h = MAX (0, h - TOOLBAR);

    gtk_widget_allocate (self->scroll, width, (int) ceil (list_h - 0.001), -1,
                         gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (0, h - list_h)));
  }
}

static void
calc_history_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CalcHistory *self = CALC_HISTORY (widget);
  int scale = vibeos_device_scale (widget);
  double w = history_width (self), h = history_height (self);
  GdkRGBA background = vibeos_color (VIBEOS_SURFACE_CONTAINER);   /* the history's background */
  GdkRGBA on_surface = vibeos_color (VIBEOS_ON_SURFACE);

  gtk_snapshot_push_clip (snapshot, &GRAPHENE_RECT_INIT (0, 0, w, h));
  gtk_snapshot_append_color (snapshot, &background, &GRAPHENE_RECT_INIT (0, 0, w, h));
  if (self->scroll) {
    gtk_widget_snapshot_child (widget, self->scroll, snapshot);
  } else {
    /* no history yet: the icon over the text, centred in the list (its height less the bottom padding) */
    double list_top = TOOLBAR, list_h = MAX (0, h - TOOLBAR - DIVIDER_PAD);
    double px = EMPTY_SIZE * scale;
    double text_h = (ceil (CALC_BOTTOM * px) - floor (-CALC_TOP * px)) / scale;
    double box_h = EMPTY_ICON + text_h;
    double top = list_top + (list_h - box_h) / 2;
    GdkRGBA icon = vibeos_color (VIBEOS_ON_SURFACE_VARIANT);
    double tw;

    if (!self->empty)
      self->empty = calc_text_layout (widget, _("No History"), EMPTY_SIZE, CALC_WEIGHT_REGULAR);
    tw = calc_text_width (self->empty);
    vibeos_snapshot_icon (snapshot, widget, "calc-history-symbolic", (w - EMPTY_ICON) / 2, top, EMPTY_ICON, &icon);
    calc_text_draw (snapshot, self->empty, (w - tw) / 2, top + EMPTY_ICON - floor (-CALC_TOP * px) / scale,
                    &on_surface);
  }
  /* the toolbar, over the list: no background of its own */
  {
    double px = TITLE_SIZE * scale;
    double top = floor (-CALC_TOP * px), bottom = ceil (CALC_BOTTOM * px);
    double box_top = floor ((TOOLBAR * scale - (bottom - top)) / 2);

    if (!self->title)
      self->title = calc_text_layout (widget, _("History"), TITLE_SIZE, CALC_WEIGHT_REGULAR);
    calc_text_draw (snapshot, self->title, TITLE_X, (box_top - top) / scale, &on_surface);
  }
  gtk_widget_snapshot_child (widget, self->back, snapshot);
  gtk_widget_snapshot_child (widget, self->overflow, snapshot);
  gtk_snapshot_pop (snapshot);
}

static void
on_back (GtkWidget *button, CalcHistory *self)
{
  g_signal_emit (self, signals[BACK], 0);
}

static void
on_overflow (GtkWidget *button, CalcHistory *self)
{
  g_signal_emit (self, signals[MENU], 0);
}

static void
on_result_long_pressed (GtkWidget *list, CalcResult *result, CalcHistory *self)
{
  g_signal_emit (self, signals[RESULT_LONG_PRESSED], 0, result);
}

static void
calc_history_dispose (GObject *object)
{
  CalcHistory *self = CALC_HISTORY (object);

  g_clear_pointer (&self->back, gtk_widget_unparent);
  g_clear_pointer (&self->overflow, gtk_widget_unparent);
  g_clear_pointer (&self->scroll, gtk_widget_unparent);
  g_clear_object (&self->title);
  g_clear_object (&self->empty);
  /* the rows' computations go */
  if (self->evaluator)
    evaluator_cancel_non_main (self->evaluator);
  self->evaluator = NULL;
  G_OBJECT_CLASS (calc_history_parent_class)->dispose (object);
}

static void
calc_history_class_init (CalcHistoryClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = calc_history_dispose;
  widget_class->measure = calc_history_measure;
  widget_class->size_allocate = calc_history_size_allocate;
  widget_class->snapshot = calc_history_snapshot;
  gtk_widget_class_set_css_name (widget_class, "calc-history");
  signals[BACK] = g_signal_new ("back", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                G_TYPE_NONE, 0);
  signals[MENU] = g_signal_new ("menu", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                G_TYPE_NONE, 0);
  signals[RESULT_LONG_PRESSED] = g_signal_new ("result-long-pressed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                                               0, NULL, NULL, NULL, G_TYPE_NONE, 1, CALC_TYPE_RESULT);
}

static void
calc_history_init (CalcHistory *self)
{
}

GtkWidget *
calc_history_new (Evaluator *evaluator)
{
  CalcHistory *self = g_object_new (CALC_TYPE_HISTORY, NULL);

  self->evaluator = evaluator;
  self->back = vibeos_icon_button_new ("calc-back-symbolic");
  gtk_widget_add_css_class (self->back, "calc-toolbar-icon");
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->back), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  /* the shell's back gesture clicks a button named Back */
                                  _("Back"), -1);
  g_signal_connect (self->back, "clicked", G_CALLBACK (on_back), self);
  gtk_widget_set_parent (self->back, GTK_WIDGET (self));
  self->overflow = vibeos_icon_button_new ("calc-more-symbolic");
  gtk_widget_add_css_class (self->overflow, "calc-toolbar-icon");
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->overflow), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _("More options"), -1);
  g_signal_connect (self->overflow, "clicked", G_CALLBACK (on_overflow), self);
  gtk_widget_set_parent (self->overflow, GTK_WIDGET (self));
  if (evaluator_get_max_index (evaluator) > 0) {
    self->list = calc_history_list_new (evaluator);
    g_signal_connect (self->list, "result-long-pressed", G_CALLBACK (on_result_long_pressed), self);
    self->scroll = spongy_scroll_view_new ();
    spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (self->scroll), self->list);
    gtk_widget_set_parent (self->scroll, GTK_WIDGET (self));
  }
  return GTK_WIDGET (self);
}

void
calc_history_set_exact_size (CalcHistory *self, double width, double height)
{
  if (self->exact_width == width && self->exact_height == height)
    return;
  self->exact_width = width;
  self->exact_height = height;
  gtk_widget_queue_allocate (GTK_WIDGET (self));
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

GtkWidget *
calc_history_get_overflow (CalcHistory *self)
{
  return self->overflow;
}
