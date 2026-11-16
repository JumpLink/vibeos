/* VibeosPressable: see vibeos-pressable.h. */
#include "vibeos-pressable.h"
#include "vibeos-ripple.h"
#include "vibeos-style.h"

#include <math.h>
#include <spongy-gtk.h>

#define TAP_TIMEOUT_MS        100      /* in a scroll view, the press shows only after this */
#define LONG_PRESS_TIMEOUT_MS 400      /* held still this long, a press is a long press */

struct _VibeosPressable {
  GtkWidget       parent;
  GtkWidget      *child;

  VibeosRipple       ripple;
  VibeosRippleShape  shape;
  double          radius;
  GdkRGBA         color;
  gboolean        has_color;          /* else the default ripple colour, as it is when the ripple draws */
  GdkRGBA         fill;               /* drawn in the shape, under the ripple; alpha 0 for none */
  int             fill_role;          /* a FAB's fill: the theme's colour as it is when the button draws; -1 none */
  gboolean        elevated;
  double          scale;

  gboolean        down;               /* a press that may still become a click */
  double          down_x, down_y;
  guint           tap_timeout;        /* the ripple is waiting to start */
  guint           long_press_timeout;
  gboolean        long_pressed;       /* this press was a long press: its release is not a click */
};

G_DEFINE_FINAL_TYPE (VibeosPressable, vibeos_pressable, GTK_TYPE_WIDGET)

enum { CLICKED, LONG_PRESSED, ACTIVATE, N_SIGNALS };
static guint signals[N_SIGNALS];

/* ---- the press ------------------------------------------------------------------------------------------------- */

static void
clear_timeouts (VibeosPressable *self)
{
  g_clear_handle_id (&self->tap_timeout, g_source_remove);
  g_clear_handle_id (&self->long_press_timeout, g_source_remove);
}

static gboolean
in_scroll_view (GtkWidget *widget)
{
  for (GtkWidget *w = gtk_widget_get_parent (widget); w; w = gtk_widget_get_parent (w))
    if (SPONGY_IS_SCROLL_VIEW (w) || GTK_IS_SCROLLED_WINDOW (w))
      return TRUE;
  return FALSE;
}

static gboolean
on_tap_timeout (gpointer data)
{
  VibeosPressable *self = data;
  self->tap_timeout = 0;
  if (self->shape != VIBEOS_RIPPLE_NONE)
    vibeos_ripple_press (&self->ripple, self->down_x, self->down_y);
  return G_SOURCE_REMOVE;
}

static gboolean
on_long_press_timeout (gpointer data)
{
  VibeosPressable *self = data;
  self->long_press_timeout = 0;
  self->long_pressed = TRUE;
  g_signal_emit (self, signals[LONG_PRESSED], 0);
  return G_SOURCE_REMOVE;
}

/* a tap on something inside that takes taps itself (a switch in a card, a button in a row) is its, not ours: the
 * child's gesture runs first but does not claim the touch, so that a drag from it can still scroll the list */
gboolean
vibeos_pick_is_tappable (GtkWidget *widget, double x, double y)
{
  GtkWidget *picked = gtk_widget_pick (widget, x, y, GTK_PICK_DEFAULT);
  for (GtkWidget *w = picked; w && w != widget; w = gtk_widget_get_parent (w))
    if (g_object_get_data (G_OBJECT (w), "vibeos-tappable"))
      return TRUE;
  return FALSE;
}

static void
on_pressed (GtkGestureClick *click, int n_press, double x, double y, VibeosPressable *self)
{
  if (self->scale <= 0 || vibeos_pick_is_tappable (GTK_WIDGET (self), x, y)) {
    gtk_gesture_set_state (GTK_GESTURE (click), GTK_EVENT_SEQUENCE_DENIED);
    return;
  }
  clear_timeouts (self);
  self->down = TRUE;
  self->long_pressed = FALSE;
  self->down_x = x;
  self->down_y = y;
  if (in_scroll_view (GTK_WIDGET (self)))
    self->tap_timeout = g_timeout_add (TAP_TIMEOUT_MS, on_tap_timeout, self);
  else
    on_tap_timeout (self);
  if (g_signal_has_handler_pending (self, signals[LONG_PRESSED], 0, FALSE))
    self->long_press_timeout = g_timeout_add (LONG_PRESS_TIMEOUT_MS, on_long_press_timeout, self);
}

static void
on_released (GtkGestureClick *click, int n_press, double x, double y, VibeosPressable *self)
{
  if (!self->down)
    return;
  self->down = FALSE;
  /* a tap quicker than the tap timeout: Android sets the pressed state now, so the ripple still plays */
  if (self->tap_timeout) {
    g_clear_handle_id (&self->tap_timeout, g_source_remove);
    on_tap_timeout (self);
  }
  g_clear_handle_id (&self->long_press_timeout, g_source_remove);
  vibeos_ripple_release (&self->ripple);
  if (!self->long_pressed)
    g_signal_emit (self, signals[CLICKED], 0);
}

/* the press is lost: the finger moved away (on_stopped, on_update) or a scroll view took the touch ("cancel") */
static void
on_lost (GtkGesture *gesture, gpointer unused, VibeosPressable *self)
{
  if (!self->down)
    return;
  self->down = FALSE;
  clear_timeouts (self);
  vibeos_ripple_release (&self->ripple);
}

/* the finger is up, or further from where it went down than a click allows (gtk-double-click-distance, the
 * distance GtkGestureClick itself stops at) */
static gboolean
moved_away (VibeosPressable *self, GtkGesture *gesture)
{
  GdkEventSequence *sequence = gtk_gesture_single_get_current_sequence (GTK_GESTURE_SINGLE (gesture));
  int distance = 5;
  double x, y;

  if (!gtk_gesture_get_point (gesture, sequence, &x, &y))
    return TRUE;
  g_object_get (gtk_widget_get_settings (GTK_WIDGET (self)), "gtk-double-click-distance", &distance, NULL);
  return hypot (x - self->down_x, y - self->down_y) > distance;
}

/* GtkGestureClick stops for two reasons: the finger moved away, or its double-click time (400 ms) ran out with the
 * finger still down. Only the first loses the press: the second is a press held, which goes on to be a long press
 * (whose 400 ms the click's timer would otherwise always beat) */
static void
on_stopped (GtkGestureClick *click, VibeosPressable *self)
{
  if (moved_away (self, GTK_GESTURE (click)))
    on_lost (GTK_GESTURE (click), NULL, self);
}

/* once the click has stopped on its timer it says nothing more, so a finger moving away after that is seen here */
static void
on_update (GtkGesture *gesture, GdkEventSequence *sequence, VibeosPressable *self)
{
  if (self->down && moved_away (self, gesture))
    on_lost (gesture, NULL, self);
}

/* ---- drawing --------------------------------------------------------------------------------------------------- */

/* the widget's shape: its box with rounded corners, or a circle around its centre */
static GskRoundedRect
shape_rect (VibeosPressable *self, double w, double h)
{
  GskRoundedRect rect;
  if (self->shape == VIBEOS_RIPPLE_CIRCLE) {
    double r = self->radius;
    gsk_rounded_rect_init_from_rect (&rect, &GRAPHENE_RECT_INIT (w / 2 - r, h / 2 - r, 2 * r, 2 * r), r);
  } else {
    gsk_rounded_rect_init_from_rect (&rect, &GRAPHENE_RECT_INIT (0, 0, w, h), self->radius);
  }
  return rect;
}

static void
vibeos_pressable_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  VibeosPressable *self = VIBEOS_PRESSABLE (widget);
  double w = gtk_widget_get_width (widget), h = gtk_widget_get_height (widget);
  if (self->scale <= 0)
    return;

  /* everything below scales with the button: the shadow and the fill too, which is why they are drawn here
   * rather than by CSS (GTK draws a CSS background before this, outside the scale) */
  gtk_snapshot_save (snapshot);
  if (self->scale != 1) {
    gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (w / 2, h / 2));
    gtk_snapshot_scale (snapshot, self->scale, self->scale);
    gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (-w / 2, -h / 2));
  }

  GskRoundedRect shape = shape_rect (self, w, h);
  if (self->elevated) {
    /* the shadow under a raised button: a soft, wide one and a sharp, close one */
    gtk_snapshot_append_outset_shadow (snapshot, &shape, &(GdkRGBA) { 0, 0, 0, 0.15f }, 0, 4, 3, 8);
    gtk_snapshot_append_outset_shadow (snapshot, &shape, &(GdkRGBA) { 0, 0, 0, 0.3f }, 0, 1, 0, 3);
  }
  gboolean ripple = self->shape != VIBEOS_RIPPLE_NONE && vibeos_ripple_is_active (&self->ripple);
  GdkRGBA fill = self->fill_role >= 0 ? vibeos_color (self->fill_role) : self->fill;
  if (fill.alpha > 0 || ripple) {
    GdkRGBA color = self->has_color ? self->color : vibeos_ripple_default_color ();
    gtk_snapshot_push_rounded_clip (snapshot, &shape);
    if (fill.alpha > 0)
      gtk_snapshot_append_color (snapshot, &fill, &shape.bounds);
    if (ripple)
      /* the wave grows over the widget's box, from where the finger is, whatever its clip */
      vibeos_ripple_snapshot (&self->ripple, snapshot, &GRAPHENE_RECT_INIT (0, 0, w, h), &color);
    gtk_snapshot_pop (snapshot);
  }

  if (self->child)
    gtk_widget_snapshot_child (widget, self->child, snapshot);
  gtk_snapshot_restore (snapshot);
}

/* ---- GObject --------------------------------------------------------------------------------------------------- */

static void
vibeos_pressable_dispose (GObject *object)
{
  VibeosPressable *self = VIBEOS_PRESSABLE (object);
  clear_timeouts (self);
  vibeos_ripple_clear (&self->ripple);
  g_clear_pointer (&self->child, gtk_widget_unparent);
  G_OBJECT_CLASS (vibeos_pressable_parent_class)->dispose (object);
}

static void
vibeos_pressable_unmap (GtkWidget *widget)
{
  VibeosPressable *self = VIBEOS_PRESSABLE (widget);
  self->down = FALSE;
  clear_timeouts (self);
  vibeos_ripple_clear (&self->ripple);
  GTK_WIDGET_CLASS (vibeos_pressable_parent_class)->unmap (widget);
}

/* "activate": a click that came from elsewhere than a finger, such as the accessibility bus's DoAction (the shell's
 * back gesture presses a "Back" button that way) */
static void
on_activate (VibeosPressable *self)
{
  g_signal_emit (self, signals[CLICKED], 0);
}

static void
on_click_action (GtkWidget *widget, const char *action, GVariant *parameter)
{
  g_signal_emit (widget, signals[CLICKED], 0);
}

static void
on_long_press_action (GtkWidget *widget, const char *action, GVariant *parameter)
{
  g_signal_emit (widget, signals[LONG_PRESSED], 0);
}

static void
vibeos_pressable_class_init (VibeosPressableClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  object_class->dispose = vibeos_pressable_dispose;
  widget_class->snapshot = vibeos_pressable_snapshot;
  widget_class->unmap = vibeos_pressable_unmap;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "vibeos-pressable");

  signals[CLICKED] = g_signal_new ("clicked", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                   NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[LONG_PRESSED] = g_signal_new ("long-pressed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                        NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[ACTIVATE] = g_signal_new_class_handler ("activate", G_TYPE_FROM_CLASS (klass),
                                                  G_SIGNAL_RUN_FIRST | G_SIGNAL_ACTION, G_CALLBACK (on_activate),
                                                  NULL, NULL, NULL, G_TYPE_NONE, 0);
  gtk_widget_class_set_activate_signal (widget_class, signals[ACTIVATE]);
  /* GTK offers a custom widget's class actions as its AT-SPI actions, the last installed first: 0 clicks it (what
   * the back gesture's DoAction (0) does), 1 long-presses it (a screen reader's long-click) */
  gtk_widget_class_install_action (widget_class, "pressable.long-press", NULL, on_long_press_action);
  gtk_widget_class_install_action (widget_class, "pressable.click", NULL, on_click_action);
  /* a button to assistive technology: its label is read, and it can be pressed (GENERIC would drop the label) */
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_BUTTON);
}

static void
vibeos_pressable_init (VibeosPressable *self)
{
  self->scale = 1;
  self->fill_role = -1;
  g_object_set_data (G_OBJECT (self), "vibeos-tappable", GINT_TO_POINTER (TRUE));
  vibeos_ripple_init (&self->ripple, GTK_WIDGET (self));

  GtkGesture *click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), GDK_BUTTON_PRIMARY);
  g_signal_connect (click, "pressed", G_CALLBACK (on_pressed), self);
  g_signal_connect (click, "released", G_CALLBACK (on_released), self);
  g_signal_connect (click, "stopped", G_CALLBACK (on_stopped), self);
  g_signal_connect (click, "update", G_CALLBACK (on_update), self);
  g_signal_connect (click, "cancel", G_CALLBACK (on_lost), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
}

GtkWidget *
vibeos_pressable_new (GtkWidget *child)
{
  VibeosPressable *self = g_object_new (VIBEOS_TYPE_PRESSABLE, NULL);
  vibeos_pressable_set_child (self, child);
  return GTK_WIDGET (self);
}

void
vibeos_pressable_set_child (VibeosPressable *self, GtkWidget *child)
{
  g_clear_pointer (&self->child, gtk_widget_unparent);
  self->child = child;
  if (child)
    gtk_widget_set_parent (child, GTK_WIDGET (self));
}

GtkWidget *
vibeos_pressable_get_child (VibeosPressable *self)
{
  return self->child;
}

void
vibeos_pressable_set_ripple (VibeosPressable *self, VibeosRippleShape shape, double radius)
{
  self->shape = shape;
  self->radius = radius;
}

void
vibeos_pressable_set_ripple_color (VibeosPressable *self, const GdkRGBA *color)
{
  self->color = *color;
  self->has_color = TRUE;
}

void
vibeos_pressable_set_fill (VibeosPressable *self, const GdkRGBA *fill, gboolean elevated)
{
  self->fill = *fill;
  self->fill_role = -1;
  self->elevated = elevated;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
vibeos_pressable_set_scale (VibeosPressable *self, double scale)
{
  self->scale = scale;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

double
vibeos_pressable_get_scale (VibeosPressable *self)
{
  return self->scale;
}

/* ---- ready-made buttons ---------------------------------------------------------------------------------------- */

GtkWidget *
vibeos_text_button_new (const char *label)
{
  GtkWidget *text = gtk_label_new (label);
  /* a button is a text view too: its line, font padding and all, centred in the button */
  vibeos_label_set_font_padding (text, 14);
  gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
  GtkWidget *button = vibeos_pressable_new (text);
  gtk_widget_add_css_class (button, "vibeos-text-button");
  /* the ripple: a pill inside the touch target */
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (button), VIBEOS_RIPPLE_BOUNDED, 20);
  return button;
}

GtkWidget *
vibeos_icon_button_new (const char *icon_name)
{
  GtkWidget *image = gtk_image_new_from_icon_name (icon_name);
  gtk_image_set_pixel_size (GTK_IMAGE (image), 24);
  GtkWidget *button = vibeos_pressable_new (image);
  gtk_widget_add_css_class (button, "vibeos-icon-button");
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (button), VIBEOS_RIPPLE_CIRCLE, 20);
  return button;
}

GtkWidget *
vibeos_fab_new (const char *icon_name, gboolean large)
{
  GtkWidget *image = gtk_image_new_from_icon_name (icon_name);
  gtk_image_set_pixel_size (GTK_IMAGE (image), 24);
  GtkWidget *fab = vibeos_pressable_new (image);
  gtk_widget_add_css_class (fab, large ? "vibeos-fab" : "vibeos-fab-small");
  gtk_widget_set_halign (fab, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (fab, GTK_ALIGN_CENTER);
  GdkRGBA fill = vibeos_color (large ? VIBEOS_PRIMARY : VIBEOS_SECONDARY);
  vibeos_pressable_set_fill (VIBEOS_PRESSABLE (fab), &fill, TRUE);
  /* the fill follows the theme */
  VIBEOS_PRESSABLE (fab)->fill_role = large ? VIBEOS_PRIMARY : VIBEOS_SECONDARY;
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (fab), VIBEOS_RIPPLE_CIRCLE, large ? 44 : 28);
  return fab;
}

void
vibeos_fab_set_icon (VibeosPressable *fab, const char *icon_name)
{
  gtk_image_set_from_icon_name (GTK_IMAGE (fab->child), icon_name);
}
