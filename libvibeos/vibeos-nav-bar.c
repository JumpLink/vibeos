/* VibeosNavBar: see vibeos-nav-bar.h. */
#include "vibeos-nav-bar.h"
#include "vibeos-anim.h"
#include "vibeos-ripple.h"
#include "vibeos-style.h"

#define BAR_H          80
#define PILL_W         64
#define PILL_H         32
#define PILL_TOP       12
#define ICON           24
#define LABEL_TOP      48
#define INDICATOR_MS   500    /* the pill coming in: a long duration, on the emphasized curve */

typedef struct {
  GtkWidget *icon, *label;
  double     shown;            /* the pill: 0 hidden .. 1 shown */
  VibeosAnim   *indicator;
  VibeosRipple  ripple;
  VibeosNavBar *bar;
} Item;

struct _VibeosNavBar {
  GtkWidget  parent;
  GPtrArray *items;
  int        selected;
  int        pressed;          /* the item under the finger, or -1 */
};

G_DEFINE_FINAL_TYPE (VibeosNavBar, vibeos_nav_bar, GTK_TYPE_WIDGET)

enum { SELECTED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
item_free (gpointer data)
{
  Item *item = data;
  vibeos_anim_free (item->indicator);
  vibeos_ripple_clear (&item->ripple);
  gtk_widget_unparent (item->icon);
  gtk_widget_unparent (item->label);
  g_free (item);
}

static double
column_width (VibeosNavBar *self)
{
  return self->items->len ? gtk_widget_get_width (GTK_WIDGET (self)) / (double) self->items->len : 0;
}

static graphene_rect_t
pill_rect (VibeosNavBar *self, guint index)
{
  double col = column_width (self);
  return GRAPHENE_RECT_INIT (col * index + (col - PILL_W) / 2, PILL_TOP, PILL_W, PILL_H);
}

/* ---- layout and drawing ---------------------------------------------------------------------------------------- */

static void
vibeos_nav_bar_measure (GtkWidget *widget, GtkOrientation orientation, int for_size,
                     int *minimum, int *natural, int *minimum_baseline, int *natural_baseline)
{
  VibeosNavBar *self = VIBEOS_NAV_BAR (widget);
  *minimum = *natural = orientation == GTK_ORIENTATION_VERTICAL ? BAR_H : (int) self->items->len * PILL_W;
}

static void
vibeos_nav_bar_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  VibeosNavBar *self = VIBEOS_NAV_BAR (widget);
  double col = column_width (self);
  for (guint i = 0; i < self->items->len; i++) {
    Item *item = g_ptr_array_index (self->items, i);
    double x = col * i;
    gtk_widget_allocate (item->icon, ICON, ICON, -1,
                         gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (x + (col - ICON) / 2,
                                                                              PILL_TOP + (PILL_H - ICON) / 2)));
    int label_h, unused;
    gtk_widget_measure (item->label, GTK_ORIENTATION_VERTICAL, (int) col, &label_h, &unused, NULL, NULL);
    gtk_widget_allocate (item->label, (int) col, label_h, -1,
                         gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (x, LABEL_TOP)));
  }
}

static void
vibeos_nav_bar_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  VibeosNavBar *self = VIBEOS_NAV_BAR (widget);
  GdkRGBA bar = vibeos_color (VIBEOS_SURFACE_CONTAINER_HIGHEST);
  gtk_snapshot_append_color (snapshot, &bar,
                             &GRAPHENE_RECT_INIT (0, 0, gtk_widget_get_width (widget), gtk_widget_get_height (widget)));

  GdkRGBA pill = vibeos_color (VIBEOS_SECONDARY_CONTAINER);
  for (guint i = 0; i < self->items->len; i++) {
    Item *item = g_ptr_array_index (self->items, i);
    graphene_rect_t rect = pill_rect (self, i);
    /* the pill widens from 40 % and fades in over the first fifth */
    if (item->shown > 0) {
      double scale_x = 0.4 + 0.6 * item->shown;
      graphene_rect_t r = rect;
      r.size.width = PILL_W * scale_x;
      r.origin.x += (PILL_W - r.size.width) / 2;
      GskRoundedRect rounded;
      gsk_rounded_rect_init_from_rect (&rounded, &r, PILL_H / 2.0);
      GdkRGBA c = pill;
      c.alpha = MIN (1, item->shown * 5);
      gtk_snapshot_push_rounded_clip (snapshot, &rounded);
      gtk_snapshot_append_color (snapshot, &c, &r);
      gtk_snapshot_pop (snapshot);
    }
    /* the ripple plays in the pill's shape, not the whole item's */
    if (vibeos_ripple_is_active (&item->ripple)) {
      GskRoundedRect rounded;
      gsk_rounded_rect_init_from_rect (&rounded, &rect, PILL_H / 2.0);
      GdkRGBA ripple = vibeos_ripple_default_color ();
      gtk_snapshot_push_rounded_clip (snapshot, &rounded);
      vibeos_ripple_snapshot (&item->ripple, snapshot, &rect, &ripple);
      gtk_snapshot_pop (snapshot);
    }
    gtk_widget_snapshot_child (widget, item->icon, snapshot);
    gtk_widget_snapshot_child (widget, item->label, snapshot);
  }
}

/* ---- selection ------------------------------------------------------------------------------------------------- */

static void
on_indicator (VibeosAnim *anim, double value, gpointer data)
{
  Item *item = data;
  item->shown = value;
  gtk_widget_queue_draw (GTK_WIDGET (item->bar));
}

static void
select_item (VibeosNavBar *self, int index, gboolean animate)
{
  for (guint i = 0; i < self->items->len; i++) {
    Item *item = g_ptr_array_index (self->items, i);
    gboolean on = (int) i == index;
    if (on) {
      gtk_widget_add_css_class (item->icon, "selected");
      gtk_widget_add_css_class (item->label, "selected");
    } else {
      gtk_widget_remove_css_class (item->icon, "selected");
      gtk_widget_remove_css_class (item->label, "selected");
    }
    if (animate && on && (int) i != self->selected)
      vibeos_anim_start (item->indicator, item->shown, 1, INDICATOR_MS, 0, SPONGY_EMPHASIZED);
    else if (!animate || !on) {
      /* the old pill goes at once: only the new one animates */
      vibeos_anim_stop (item->indicator);
      item->shown = on;
    }
  }
  self->selected = index;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static int
item_at (VibeosNavBar *self, double x)
{
  double col = column_width (self);
  int index = col > 0 ? (int) (x / col) : -1;
  return index >= 0 && index < (int) self->items->len ? index : -1;
}

static void
on_pressed (GtkGestureClick *click, int n_press, double x, double y, VibeosNavBar *self)
{
  self->pressed = item_at (self, x);
  if (self->pressed < 0)
    return;
  Item *item = g_ptr_array_index (self->items, self->pressed);
  graphene_rect_t pill = pill_rect (self, self->pressed);
  vibeos_ripple_press (&item->ripple, x - pill.origin.x, y - pill.origin.y);
}

static void
release (VibeosNavBar *self)
{
  if (self->pressed >= 0)
    vibeos_ripple_release (&((Item *) g_ptr_array_index (self->items, self->pressed))->ripple);
}

static void
on_released (GtkGestureClick *click, int n_press, double x, double y, VibeosNavBar *self)
{
  int index = self->pressed;
  release (self);
  self->pressed = -1;
  if (index < 0 || index == self->selected)
    return;
  select_item (self, index, TRUE);
  g_signal_emit (self, signals[SELECTED], 0, index);
}

static void
on_stopped (GtkGestureClick *click, VibeosNavBar *self)
{
  release (self);
  self->pressed = -1;
}

/* ---- GObject --------------------------------------------------------------------------------------------------- */

static void
vibeos_nav_bar_dispose (GObject *object)
{
  VibeosNavBar *self = VIBEOS_NAV_BAR (object);
  g_clear_pointer (&self->items, g_ptr_array_unref);
  G_OBJECT_CLASS (vibeos_nav_bar_parent_class)->dispose (object);
}

static void
vibeos_nav_bar_class_init (VibeosNavBarClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  object_class->dispose = vibeos_nav_bar_dispose;
  widget_class->measure = vibeos_nav_bar_measure;
  widget_class->size_allocate = vibeos_nav_bar_size_allocate;
  widget_class->snapshot = vibeos_nav_bar_snapshot;
  gtk_widget_class_set_css_name (widget_class, "vibeos-nav-bar");

  signals[SELECTED] = g_signal_new ("selected", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                    NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
}

static void
vibeos_nav_bar_init (VibeosNavBar *self)
{
  self->items = g_ptr_array_new_with_free_func (item_free);
  self->selected = -1;
  self->pressed = -1;
  gtk_widget_add_css_class (GTK_WIDGET (self), "vibeos-nav-bar");

  GtkGesture *click = gtk_gesture_click_new ();
  g_signal_connect (click, "pressed", G_CALLBACK (on_pressed), self);
  g_signal_connect (click, "released", G_CALLBACK (on_released), self);
  g_signal_connect (click, "stopped", G_CALLBACK (on_stopped), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
}

GtkWidget *
vibeos_nav_bar_new (void)
{
  return g_object_new (VIBEOS_TYPE_NAV_BAR, NULL);
}

int
vibeos_nav_bar_append (VibeosNavBar *self, const char *icon_name, const char *label)
{
  Item *item = g_new0 (Item, 1);
  item->bar = self;
  item->icon = gtk_image_new_from_icon_name (icon_name);
  gtk_image_set_pixel_size (GTK_IMAGE (item->icon), ICON);
  gtk_widget_add_css_class (item->icon, "vibeos-nav-item");
  item->label = gtk_label_new (label);
  gtk_label_set_ellipsize (GTK_LABEL (item->label), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (item->label, "vibeos-nav-item");
  gtk_widget_add_css_class (item->label, "vibeos-nav-label");
  gtk_widget_set_parent (item->icon, GTK_WIDGET (self));
  gtk_widget_set_parent (item->label, GTK_WIDGET (self));
  item->indicator = vibeos_anim_new (GTK_WIDGET (self), on_indicator, item);
  vibeos_ripple_init (&item->ripple, GTK_WIDGET (self));
  g_ptr_array_add (self->items, item);
  if (self->selected < 0)
    select_item (self, 0, FALSE);
  gtk_widget_queue_resize (GTK_WIDGET (self));
  return self->items->len - 1;
}

void
vibeos_nav_bar_set_selected (VibeosNavBar *self, int index)
{
  select_item (self, index, FALSE);
}

int
vibeos_nav_bar_get_selected (VibeosNavBar *self)
{
  return self->selected;
}
