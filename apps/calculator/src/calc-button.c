/* CalcButton: see calc-button.h */
#include "calc-button.h"
#include "calc-text.h"

#include <math.h>
#include <vibeos.h>

#define INSET              4       /* the pill's inset top and bottom, inside the key's cell */
#define TEXT_RATIO         0.4     /* the label's size, of the key's height */
#define RIPPLE_ALPHA       0.10f   /* the ripple, in the label's colour; it also lightens the pressed pill */
#define LONG_PRESS_MS      400     /* held this long, a press is a long press */

struct _CalcButton {
  GtkWidget        parent;
  CalcButtonTheme  theme;
  char            *label;
  char            *icon_name;
  double           icon_size;
  double           exact_width, exact_height;   /* 0: the allocation's */
  int              weight;
  PangoLayout     *layout;
  double           layout_size;
  VibeosRipple        ripple;
  guint            long_press_id;
  gboolean         long_pressed;
};

G_DEFINE_FINAL_TYPE (CalcButton, calc_button, GTK_TYPE_WIDGET)

enum { CLICKED, LONG_PRESSED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
theme_colors (CalcButtonTheme theme, GdkRGBA *container, GdkRGBA *content)
{
  static const VibeosColor roles[][2] = {
    [CALC_BUTTON_ADVANCED] = { VIBEOS_SURFACE, VIBEOS_ON_SURFACE },
    [CALC_BUTTON_NUMERIC]  = { VIBEOS_SURFACE_CONTAINER_HIGHEST, VIBEOS_ON_SURFACE },
    [CALC_BUTTON_OPERATOR] = { VIBEOS_SECONDARY_CONTAINER, VIBEOS_ON_SECONDARY_CONTAINER },
    [CALC_BUTTON_CLEAR]    = { VIBEOS_SURFACE_CONTAINER_HIGHEST, VIBEOS_ON_SURFACE_VARIANT },
    [CALC_BUTTON_ACTION]   = { VIBEOS_PRIMARY_CONTAINER, VIBEOS_ON_PRIMARY_CONTAINER },
    [CALC_BUTTON_EQUALS]   = { VIBEOS_TERTIARY, VIBEOS_ON_TERTIARY },
  };

  *container = vibeos_color (roles[theme][0]);
  *content = vibeos_color (roles[theme][1]);
}

static void
calc_button_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                     int *minimum_baseline, int *natural_baseline)
{
  /* the pads size their keys */
  *minimum = *natural = 0;
}

static void
calc_button_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CalcButton *self = CALC_BUTTON (widget);
  double w = self->exact_width > 0 ? self->exact_width : gtk_widget_get_width (widget);
  double h = self->exact_height > 0 ? self->exact_height : gtk_widget_get_height (widget);
  GdkRGBA container, content;
  GskRoundedRect pill;
  graphene_rect_t box;

  if (w <= 0 || h <= 2 * INSET)
    return;
  theme_colors (self->theme, &container, &content);
  box = GRAPHENE_RECT_INIT (0, INSET, w, h - 2 * INSET);
  gsk_rounded_rect_init_from_rect (&pill, &box, MIN (box.size.width, box.size.height) / 2);
  gtk_snapshot_push_rounded_clip (snapshot, &pill);
  if (self->theme != CALC_BUTTON_ADVANCED)
    gtk_snapshot_append_color (snapshot, &container, &box);
  if (vibeos_ripple_is_active (&self->ripple)) {
    GdkRGBA ripple = content;
    ripple.alpha = RIPPLE_ALPHA;
    vibeos_ripple_snapshot (&self->ripple, snapshot, &box, &ripple);
  }
  gtk_snapshot_pop (snapshot);

  if (self->icon_name) {
    vibeos_snapshot_icon (snapshot, widget, self->icon_name, (w - self->icon_size) / 2, (h - self->icon_size) / 2,
                       self->icon_size, &content);
    return;
  }
  if (!self->label)
    return;
  /* text size 0.4 of the key's height; the line (ascent + descent, no font padding) centred in the key */
  double size = h * TEXT_RATIO;
  if (!self->layout) {
    self->layout = calc_text_layout (widget, self->label, size, self->weight);
    self->layout_size = size;
  } else if (self->layout_size != size) {
    calc_text_set_size (self->layout, size, self->weight);
    self->layout_size = size;
  }
  double baseline = h / 2 + (CALC_ASCENT - CALC_DESCENT) * size / 2;
  calc_text_draw (snapshot, self->layout, (w - calc_text_width (self->layout)) / 2, baseline, &content);
}

/* --- touch */

static gboolean
on_long_press (gpointer data)
{
  CalcButton *self = data;

  gboolean handled = FALSE;

  self->long_press_id = 0;
  /* the long press: the click is still to come unless a handler took the long press (the delete key's) */
  g_signal_emit (self, signals[LONG_PRESSED], 0, &handled);
  self->long_pressed = handled;
  return G_SOURCE_REMOVE;
}

static void
stop_long_press (CalcButton *self)
{
  if (self->long_press_id)
    g_source_remove (self->long_press_id);
  self->long_press_id = 0;
}

static void
on_pressed (GtkGestureClick *click, int n_press, double x, double y, CalcButton *self)
{
  /* the ripple's bounds are the pill's */
  vibeos_ripple_press (&self->ripple, x, y - INSET);
  self->long_pressed = FALSE;
  stop_long_press (self);
  self->long_press_id = g_timeout_add (LONG_PRESS_MS, on_long_press, self);
}

static void
on_released (GtkGestureClick *click, int n_press, double x, double y, CalcButton *self)
{
  gboolean was_long = self->long_pressed;

  stop_long_press (self);
  vibeos_ripple_release (&self->ripple);
  if (!was_long)
    g_signal_emit (self, signals[CLICKED], 0);
}

static void
on_stopped (GtkGestureClick *click, CalcButton *self)
{
  /* the finger moved off, or another gesture took the touch */
  stop_long_press (self);
  vibeos_ripple_release (&self->ripple);
}

static void
calc_button_dispose (GObject *object)
{
  CalcButton *self = CALC_BUTTON (object);

  stop_long_press (self);
  vibeos_ripple_clear (&self->ripple);
  g_clear_object (&self->layout);
  G_OBJECT_CLASS (calc_button_parent_class)->dispose (object);
}

static void
calc_button_finalize (GObject *object)
{
  CalcButton *self = CALC_BUTTON (object);

  g_free (self->label);
  g_free (self->icon_name);
  G_OBJECT_CLASS (calc_button_parent_class)->finalize (object);
}

static void
calc_button_class_init (CalcButtonClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = calc_button_dispose;
  G_OBJECT_CLASS (klass)->finalize = calc_button_finalize;
  widget_class->measure = calc_button_measure;
  widget_class->snapshot = calc_button_snapshot;
  gtk_widget_class_set_css_name (widget_class, "calc-button");
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_BUTTON);
  signals[CLICKED] = g_signal_new ("clicked", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                   G_TYPE_NONE, 0);
  signals[LONG_PRESSED] = g_signal_new ("long-pressed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                        g_signal_accumulator_true_handled, NULL, NULL, G_TYPE_BOOLEAN, 0);
}

static void
calc_button_init (CalcButton *self)
{
  GtkGesture *click = gtk_gesture_click_new ();

  self->weight = CALC_WEIGHT_MEDIUM;
  vibeos_ripple_init (&self->ripple, GTK_WIDGET (self));
  g_signal_connect (click, "pressed", G_CALLBACK (on_pressed), self);
  g_signal_connect (click, "released", G_CALLBACK (on_released), self);
  g_signal_connect (click, "stopped", G_CALLBACK (on_stopped), self);
  g_signal_connect (click, "cancel", G_CALLBACK (on_stopped), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
  g_object_set_data (G_OBJECT (self), "vibeos-tappable", GINT_TO_POINTER (1));
}

GtkWidget *
calc_button_new (const char *label, CalcButtonTheme theme)
{
  CalcButton *self = g_object_new (CALC_TYPE_BUTTON, NULL);

  self->theme = theme;
  self->label = g_strdup (label);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  return GTK_WIDGET (self);
}

GtkWidget *
calc_button_new_icon (const char *icon_name, double icon_size, CalcButtonTheme theme)
{
  CalcButton *self = g_object_new (CALC_TYPE_BUTTON, NULL);

  self->theme = theme;
  self->icon_name = g_strdup (icon_name);
  self->icon_size = icon_size;
  return GTK_WIDGET (self);
}

void
calc_button_set_label (CalcButton *self, const char *label)
{
  if (g_strcmp0 (self->label, label) == 0)
    return;
  g_free (self->label);
  self->label = g_strdup (label);
  if (self->layout)
    pango_layout_set_text (self->layout, label, -1);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

const char *
calc_button_get_label (CalcButton *self)
{
  return self->label;
}

void
calc_button_set_exact_size (CalcButton *self, double width, double height)
{
  if (self->exact_width == width && self->exact_height == height)
    return;
  self->exact_width = width;
  self->exact_height = height;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
calc_button_set_weight (CalcButton *self, int weight)
{
  self->weight = weight;
  if (self->layout)
    calc_text_set_size (self->layout, self->layout_size, weight);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}
