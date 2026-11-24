/* CalcDisplay: see calc-display.h. In portrait the formula and the result split the height between the toolbar
 * and the handle in device pixels, the formula taking the share rounded down and the result the rest. */
#include "calc-display.h"
#include "calc-text.h"

#include <math.h>
#include <vibeos.h>

#define TOOLBAR           64      /* the toolbar's height */
#define HANDLE_WIDTH      24      /* the drag handle, a pill under the result */
#define HANDLE_HEIGHT     4
#define HANDLE_RADIUS     2
#define HANDLE_MARGIN     8       /* above and below the handle */
#define TABLET_HANDLE_MARGIN 16   /* ... on a tablet */
#define CORNER            16      /* the display's bottom corners */
#define MODE_START        16      /* the mode label's inset from the start */
#define MODE_SIZE         16      /* the mode label's text size */
#define OVERFLOW_X        (66.5 / 3)  /* from the end to the ⋮'s dots (66.5 device px on the reference phone) */
#define OVERFLOW_Y        (95.5 / 3)  /* down from the toolbar's top to the ⋮'s dots (95.5 device px there) */
#define BUTTON            48      /* the ⋮ button's touch target */

struct _CalcDisplay {
  GtkWidget    parent;
  gboolean     one_line;
  int          handle_margin;
  double       formula_top, formula_bottom, formula_end;
  GtkWidget   *formula;
  GtkWidget   *result;
  GtkWidget   *overflow;
  char        *mode;
  PangoLayout *mode_layout;
  double       exact_width, exact_height;

  /* the result in the formula's place, after "=" */
  gboolean     showing_result;
  double       result_scale;
  /* where the formula and the result are, px: for the move after "=" */
  double       formula_y, formula_h, result_y, result_h;
};

G_DEFINE_FINAL_TYPE (CalcDisplay, calc_display, GTK_TYPE_WIDGET)

static double
display_width (CalcDisplay *self)
{
  return self->exact_width > 0 ? self->exact_width : gtk_widget_get_width (GTK_WIDGET (self));
}

static double
display_height (CalcDisplay *self)
{
  return self->exact_height > 0 ? self->exact_height : gtk_widget_get_height (GTK_WIDGET (self));
}

static void
calc_display_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                      int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 0;
}

static void
calc_display_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  CalcDisplay *self = CALC_DISPLAY (widget);
  int scale = vibeos_device_scale (widget);
  int W = (int) round (display_width (self) * scale), H = (int) round (display_height (self) * scale);
  int toolbar = TOOLBAR * scale;
  int handle = (HANDLE_HEIGHT + 2 * self->handle_margin) * scale;
  int inner = MAX (0, H - toolbar - handle);
  double w = (double) W / scale;
  double formula_ty = 0, result_ty = 0;
  GskTransform *t;

  if (self->one_line) {
    self->formula_y = self->result_y = (double) toolbar / scale;
    self->formula_h = self->result_h = (double) inner / scale;
  } else {
    int formula = inner / 2;
    self->formula_y = (double) toolbar / scale;
    self->formula_h = (double) formula / scale;
    self->result_y = (double) (toolbar + formula) / scale;
    self->result_h = (double) (inner - formula) / scale;
  }

  if (self->showing_result) {
    double formula_bottom = self->formula_y + self->formula_h, result_bottom = self->result_y + self->result_h;
    double result_pad_bottom, result_pad_end;

    calc_result_get_padding (CALC_RESULT (self->result), &result_pad_bottom, &result_pad_end);
    /* to the formula's bottom padding */
    result_ty = (formula_bottom - result_bottom) - (self->formula_bottom - result_pad_bottom);
    formula_ty = self->one_line ? -((double) toolbar / scale + formula_bottom) : -formula_bottom;
  }

  calc_formula_set_exact_height (CALC_FORMULA (self->formula), self->formula_h);
  gtk_widget_allocate (self->formula, (int) ceil (w - 0.001), (int) ceil (self->formula_h - 0.001), -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (0, self->formula_y + formula_ty)));

  calc_result_set_exact_height (CALC_RESULT (self->result), self->result_h);
  t = gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (0, self->result_y + result_ty));
  if (self->showing_result && self->result_scale != 1) {
    /* about the bottom end, less the padding */
    double pad_bottom, pad_end;

    calc_result_get_padding (CALC_RESULT (self->result), &pad_bottom, &pad_end);
    graphene_point_t pivot = GRAPHENE_POINT_INIT (w - pad_end, self->result_h - pad_bottom);
    t = gsk_transform_translate (t, &pivot);
    t = gsk_transform_scale (t, self->result_scale, self->result_scale);
    t = gsk_transform_translate (t, &GRAPHENE_POINT_INIT (-pivot.x, -pivot.y));
  }
  gtk_widget_allocate (self->result, (int) ceil (w - 0.001), (int) ceil (self->result_h - 0.001), -1, t);

  /* the ⋮: an icon button centred on the dots */
  gtk_widget_allocate (self->overflow, BUTTON, BUTTON, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (w - OVERFLOW_X - BUTTON / 2.0,
                                                                            OVERFLOW_Y - BUTTON / 2.0)));
}

static void
calc_display_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CalcDisplay *self = CALC_DISPLAY (widget);
  int scale = vibeos_device_scale (widget);
  double w = display_width (self), h = display_height (self);
  GdkRGBA surface = vibeos_color (VIBEOS_SURFACE);
  GdkRGBA secondary = vibeos_color (VIBEOS_ON_SURFACE_VARIANT);
  GskRoundedRect shape;

  /* the bottom corners rounded */
  gsk_rounded_rect_init (&shape, &GRAPHENE_RECT_INIT (0, 0, w, h), &GRAPHENE_SIZE_INIT (0, 0),
                         &GRAPHENE_SIZE_INIT (0, 0), &GRAPHENE_SIZE_INIT (CORNER, CORNER),
                         &GRAPHENE_SIZE_INIT (CORNER, CORNER));
  gtk_snapshot_push_rounded_clip (snapshot, &shape);
  gtk_snapshot_append_color (snapshot, &surface, &GRAPHENE_RECT_INIT (0, 0, w, h));

  /* the mode, all caps; its box (yMax to yMin, rounded out) centred in the toolbar */
  if (self->mode) {
    double px = MODE_SIZE * scale;
    double top = floor (-CALC_TOP * px), bottom = ceil (CALC_BOTTOM * px);
    double box_top = floor ((TOOLBAR * scale - (bottom - top)) / 2);

    if (!self->mode_layout)
      self->mode_layout = calc_text_layout (widget, self->mode, MODE_SIZE, CALC_WEIGHT_MEDIUM);
    pango_layout_set_text (self->mode_layout, self->mode, -1);
    calc_text_draw (snapshot, self->mode_layout, MODE_START, (box_top - top) / scale, &secondary);
  }

  /* the children: clipped to the display (the formula leaves at the top) */
  gtk_snapshot_push_clip (snapshot, &GRAPHENE_RECT_INIT (0, 0, w, h));
  gtk_widget_snapshot_child (widget, self->formula, snapshot);
  gtk_widget_snapshot_child (widget, self->result, snapshot);
  gtk_snapshot_pop (snapshot);
  gtk_widget_snapshot_child (widget, self->overflow, snapshot);

  /* the drag handle */
  {
    double hx = round ((w * scale - HANDLE_WIDTH * scale) / 2) / scale;
    double hy = h - self->handle_margin - HANDLE_HEIGHT;
    GskRoundedRect handle;

    gsk_rounded_rect_init_from_rect (&handle, &GRAPHENE_RECT_INIT (hx, hy, HANDLE_WIDTH, HANDLE_HEIGHT),
                                     HANDLE_RADIUS);
    gtk_snapshot_push_rounded_clip (snapshot, &handle);
    gtk_snapshot_append_color (snapshot, &secondary, &handle.bounds);
    gtk_snapshot_pop (snapshot);
  }
  gtk_snapshot_pop (snapshot);
}

static void
calc_display_dispose (GObject *object)
{
  CalcDisplay *self = CALC_DISPLAY (object);

  g_clear_pointer (&self->formula, gtk_widget_unparent);
  g_clear_pointer (&self->result, gtk_widget_unparent);
  g_clear_pointer (&self->overflow, gtk_widget_unparent);
  g_clear_object (&self->mode_layout);
  G_OBJECT_CLASS (calc_display_parent_class)->dispose (object);
}

static void
calc_display_finalize (GObject *object)
{
  g_free (CALC_DISPLAY (object)->mode);
  G_OBJECT_CLASS (calc_display_parent_class)->finalize (object);
}

static void
calc_display_class_init (CalcDisplayClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = calc_display_dispose;
  G_OBJECT_CLASS (klass)->finalize = calc_display_finalize;
  widget_class->measure = calc_display_measure;
  widget_class->size_allocate = calc_display_size_allocate;
  widget_class->snapshot = calc_display_snapshot;
  gtk_widget_class_set_css_name (widget_class, "calc-display");
}

static void
calc_display_init (CalcDisplay *self)
{
  self->result_scale = 1;
}

GtkWidget *
calc_display_new (gboolean one_line)
{
  return calc_display_new_full (one_line, FALSE);
}

/* the text sizes and paddings: a phone's upright and sideways, a tablet's (the same both ways but for the formula's
 * smallest size and the result's bottom padding) */
typedef struct {
  double formula_min, formula_max, result_size;
  double formula_padding[4], result_padding[4];      /* top, bottom, start, end */
  int handle_margin;
} Metrics;

static const Metrics phone_metrics = { 32, 48, 32, { 16, 8, 16, 16 }, { 8, 32, 16, 16 }, HANDLE_MARGIN };
static const Metrics phone_one_line = { 42, 56, 42, { 8, 28, 16, 16 }, { 8, 28, 16, 16 }, HANDLE_MARGIN };
static const Metrics tablet_metrics = { 48, 128, 72, { 32, 22, 44, 44 }, { 22, 64, 44, 44 }, TABLET_HANDLE_MARGIN };
static const Metrics tablet_one_line = { 32, 128, 72, { 32, 22, 44, 44 }, { 22, 44, 44, 44 }, TABLET_HANDLE_MARGIN };

GtkWidget *
calc_display_new_full (gboolean one_line, gboolean tablet)
{
  CalcDisplay *self = g_object_new (CALC_TYPE_DISPLAY, NULL);
  const Metrics *m = tablet ? (one_line ? &tablet_one_line : &tablet_metrics)
                            : (one_line ? &phone_one_line : &phone_metrics);

  self->one_line = one_line;
  self->handle_margin = m->handle_margin;
  self->formula_top = m->formula_padding[0];
  self->formula_bottom = m->formula_padding[1];
  self->formula_end = m->formula_padding[3];
  /* landscape: larger text, more padding below; a tablet: larger still */
  self->formula = calc_formula_new (m->formula_min, m->formula_max, 8);
  calc_formula_set_padding (CALC_FORMULA (self->formula), m->formula_padding[0], m->formula_padding[1],
                            m->formula_padding[2], m->formula_padding[3]);
  self->result = calc_result_new (m->result_size);
  calc_result_set_padding (CALC_RESULT (self->result), m->result_padding[0], m->result_padding[1],
                           m->result_padding[2], m->result_padding[3]);
  /* the one-line display's result starts invisible */
  if (one_line)
    gtk_widget_set_child_visible (self->result, FALSE);
  gtk_widget_set_parent (self->formula, GTK_WIDGET (self));
  gtk_widget_set_parent (self->result, GTK_WIDGET (self));
  self->overflow = vibeos_icon_button_new ("calc-more-symbolic");
  gtk_widget_add_css_class (self->overflow, "calc-toolbar-icon");
  gtk_widget_set_tooltip_text (self->overflow, NULL);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->overflow), GTK_ACCESSIBLE_PROPERTY_LABEL, "More options", -1);
  gtk_widget_set_parent (self->overflow, GTK_WIDGET (self));
  return GTK_WIDGET (self);
}

void
calc_display_get_formula_padding (CalcDisplay *self, double *top, double *bottom, double *end)
{
  *top = self->formula_top;
  *bottom = self->formula_bottom;
  *end = self->formula_end;
}

CalcFormula *
calc_display_get_formula (CalcDisplay *self)
{
  return CALC_FORMULA (self->formula);
}

CalcResult *
calc_display_get_result (CalcDisplay *self)
{
  return CALC_RESULT (self->result);
}

GtkWidget *
calc_display_get_overflow (CalcDisplay *self)
{
  return self->overflow;
}

gboolean
calc_display_is_one_line (CalcDisplay *self)
{
  return self->one_line;
}

void
calc_display_set_mode (CalcDisplay *self, const char *mode)
{
  g_free (self->mode);
  self->mode = g_strdup (mode);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
calc_display_set_exact_size (CalcDisplay *self, double width, double height)
{
  if (self->exact_width == width && self->exact_height == height)
    return;
  self->exact_width = width;
  self->exact_height = height;
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

void
calc_display_show_result (CalcDisplay *self, double scale)
{
  self->showing_result = TRUE;
  self->result_scale = scale;
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

void
calc_display_restore_positions (CalcDisplay *self)
{
  self->showing_result = FALSE;
  self->result_scale = 1;
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

void
calc_display_set_visible (CalcDisplay *self, gboolean formula, gboolean result)
{
  gtk_widget_set_child_visible (self->formula, formula);
  gtk_widget_set_child_visible (self->result, result);
}
