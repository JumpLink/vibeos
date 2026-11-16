/* VibeosNavRail: see vibeos-nav-rail.h. Drawn like VibeosNavBar, its items stacked rather than side by side. */
#include "vibeos-nav-rail.h"
#include "vibeos-anim.h"
#include "vibeos-ripple.h"
#include "vibeos-style.h"

#define RAIL_W         80
#define ITEM_H         65     /* a labelled item: the pill, the label under it */
#define PILL_W         56
#define PILL_H         32
#define PILL_TOP       4
#define LABEL_TOP      40
#define ICON_ITEM_H    64     /* an item without a label: a taller pill, round */
#define ICON_PILL_H    56
#define ICON           24
#define HEADER_TOP     8      /* the header's distance from the top */
#define HEADER_GAP     24     /* the least space between the header and the first item */
#define INDICATOR_MS   500    /* the pill coming in: a long duration, on the emphasized curve */

typedef struct {
  GtkWidget     *icon, *label;
  double         shown;       /* the pill: 0 hidden .. 1 shown */
  VibeosAnim    *indicator;
  VibeosRipple   ripple;
  VibeosNavRail *rail;
} Item;

struct _VibeosNavRail {
  GtkWidget  parent;
  GPtrArray *items;
  GtkWidget *header;
  int        selected;
  int        pressed;          /* the item under the finger, or -1 */
  gboolean   labels;
};

G_DEFINE_FINAL_TYPE (VibeosNavRail, vibeos_nav_rail, GTK_TYPE_WIDGET)

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

static int
item_height (VibeosNavRail *self)
{
  return self->labels ? ITEM_H : ICON_ITEM_H;
}

static int
header_bottom (VibeosNavRail *self)
{
  int min, nat;
  if (!self->header || !gtk_widget_get_visible (self->header))
    return 0;
  gtk_widget_measure (self->header, GTK_ORIENTATION_VERTICAL, -1, &min, &nat, NULL, NULL);
  return HEADER_TOP + nat;
}

/* where the first item starts: the items centred in the rail, but below the header */
static double
group_top (VibeosNavRail *self)
{
  double group = item_height (self) * (double) self->items->len;
  double top = (gtk_widget_get_height (GTK_WIDGET (self)) - group) / 2;
  int header = header_bottom (self);
  return header ? MAX (top, header + HEADER_GAP) : top;
}

static graphene_rect_t
pill_rect (VibeosNavRail *self, guint index)
{
  double y = group_top (self) + item_height (self) * (double) index + PILL_TOP;
  return GRAPHENE_RECT_INIT ((RAIL_W - PILL_W) / 2.0, y, PILL_W, self->labels ? PILL_H : ICON_PILL_H);
}

/* ---- layout and drawing ---------------------------------------------------------------------------------------- */

static void
vibeos_nav_rail_measure (GtkWidget *widget, GtkOrientation orientation, int for_size,
                         int *minimum, int *natural, int *minimum_baseline, int *natural_baseline)
{
  VibeosNavRail *self = VIBEOS_NAV_RAIL (widget);
  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    *minimum = *natural = RAIL_W;
  else
    *minimum = *natural = header_bottom (self) + (self->header ? HEADER_GAP : 0)
                          + item_height (self) * (int) self->items->len;
}

static void
vibeos_nav_rail_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  VibeosNavRail *self = VIBEOS_NAV_RAIL (widget);

  if (self->header && gtk_widget_get_visible (self->header)) {
    int w, h, unused;
    gtk_widget_measure (self->header, GTK_ORIENTATION_HORIZONTAL, -1, &unused, &w, NULL, NULL);
    gtk_widget_measure (self->header, GTK_ORIENTATION_VERTICAL, w, &unused, &h, NULL, NULL);
    w = MIN (w, width);
    gtk_widget_allocate (self->header, w, h, -1,
                         gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT ((width - w) / 2.0, HEADER_TOP)));
  }
  for (guint i = 0; i < self->items->len; i++) {
    Item *item = g_ptr_array_index (self->items, i);
    graphene_rect_t pill = pill_rect (self, i);
    gtk_widget_allocate (item->icon, ICON, ICON, -1,
                         gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (
                           pill.origin.x + (PILL_W - ICON) / 2.0, pill.origin.y + (pill.size.height - ICON) / 2)));
    if (!self->labels)
      continue;
    int label_h, unused;
    gtk_widget_measure (item->label, GTK_ORIENTATION_VERTICAL, width, &label_h, &unused, NULL, NULL);
    gtk_widget_allocate (item->label, width, label_h, -1,
                         gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (
                           0, pill.origin.y - PILL_TOP + LABEL_TOP)));
  }
}

static void
vibeos_nav_rail_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  VibeosNavRail *self = VIBEOS_NAV_RAIL (widget);
  GdkRGBA pill = vibeos_color (VIBEOS_SECONDARY_CONTAINER);

  if (self->header)
    gtk_widget_snapshot_child (widget, self->header, snapshot);
  for (guint i = 0; i < self->items->len; i++) {
    Item *item = g_ptr_array_index (self->items, i);
    graphene_rect_t rect = pill_rect (self, i);
    GskRoundedRect rounded;
    /* the pill widens from 40 % and fades in over the first fifth */
    if (item->shown > 0) {
      graphene_rect_t r = rect;
      GdkRGBA c = pill;
      r.size.width = rect.size.width * (0.4 + 0.6 * item->shown);
      r.origin.x += (rect.size.width - r.size.width) / 2;
      gsk_rounded_rect_init_from_rect (&rounded, &r, rect.size.height / 2);
      c.alpha = MIN (1, item->shown * 5);
      gtk_snapshot_push_rounded_clip (snapshot, &rounded);
      gtk_snapshot_append_color (snapshot, &c, &r);
      gtk_snapshot_pop (snapshot);
    }
    /* the ripple plays in the pill's shape */
    if (vibeos_ripple_is_active (&item->ripple)) {
      GdkRGBA ripple = vibeos_ripple_default_color ();
      gsk_rounded_rect_init_from_rect (&rounded, &rect, rect.size.height / 2);
      gtk_snapshot_push_rounded_clip (snapshot, &rounded);
      vibeos_ripple_snapshot (&item->ripple, snapshot, &rect, &ripple);
      gtk_snapshot_pop (snapshot);
    }
    gtk_widget_snapshot_child (widget, item->icon, snapshot);
    if (self->labels)
      gtk_widget_snapshot_child (widget, item->label, snapshot);
  }
}

/* ---- selection ------------------------------------------------------------------------------------------------- */

static void
on_indicator (VibeosAnim *anim, double value, gpointer data)
{
  Item *item = data;
  item->shown = value;
  gtk_widget_queue_draw (GTK_WIDGET (item->rail));
}

static void
select_item (VibeosNavRail *self, int index, gboolean animate)
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

/* the item whose row (the full width of the rail) is under y, or -1 */
static int
item_at (VibeosNavRail *self, double y)
{
  double from = y - group_top (self);
  int index = from >= 0 ? (int) (from / item_height (self)) : -1;
  return index >= 0 && index < (int) self->items->len ? index : -1;
}

static void
on_pressed (GtkGestureClick *click, int n_press, double x, double y, VibeosNavRail *self)
{
  self->pressed = item_at (self, y);
  if (self->pressed < 0) {
    gtk_gesture_set_state (GTK_GESTURE (click), GTK_EVENT_SEQUENCE_DENIED);
    return;
  }
  Item *item = g_ptr_array_index (self->items, self->pressed);
  graphene_rect_t pill = pill_rect (self, self->pressed);
  vibeos_ripple_press (&item->ripple, x - pill.origin.x, y - pill.origin.y);
}

static void
release (VibeosNavRail *self)
{
  if (self->pressed >= 0)
    vibeos_ripple_release (&((Item *) g_ptr_array_index (self->items, self->pressed))->ripple);
}

static void
on_released (GtkGestureClick *click, int n_press, double x, double y, VibeosNavRail *self)
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
on_stopped (GtkGestureClick *click, VibeosNavRail *self)
{
  release (self);
  self->pressed = -1;
}

/* ---- GObject --------------------------------------------------------------------------------------------------- */

static void
vibeos_nav_rail_dispose (GObject *object)
{
  VibeosNavRail *self = VIBEOS_NAV_RAIL (object);
  g_clear_pointer (&self->items, g_ptr_array_unref);
  g_clear_pointer (&self->header, gtk_widget_unparent);
  G_OBJECT_CLASS (vibeos_nav_rail_parent_class)->dispose (object);
}

static void
vibeos_nav_rail_class_init (VibeosNavRailClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  object_class->dispose = vibeos_nav_rail_dispose;
  widget_class->measure = vibeos_nav_rail_measure;
  widget_class->size_allocate = vibeos_nav_rail_size_allocate;
  widget_class->snapshot = vibeos_nav_rail_snapshot;
  gtk_widget_class_set_css_name (widget_class, "vibeos-nav-rail");

  signals[SELECTED] = g_signal_new ("selected", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                    NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
}

static void
vibeos_nav_rail_init (VibeosNavRail *self)
{
  self->items = g_ptr_array_new_with_free_func (item_free);
  self->selected = -1;
  self->pressed = -1;
  self->labels = TRUE;
  gtk_widget_add_css_class (GTK_WIDGET (self), "vibeos-nav-rail");

  GtkGesture *click = gtk_gesture_click_new ();
  g_signal_connect (click, "pressed", G_CALLBACK (on_pressed), self);
  g_signal_connect (click, "released", G_CALLBACK (on_released), self);
  g_signal_connect (click, "stopped", G_CALLBACK (on_stopped), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
}

GtkWidget *
vibeos_nav_rail_new (void)
{
  return g_object_new (VIBEOS_TYPE_NAV_RAIL, NULL);
}

int
vibeos_nav_rail_append (VibeosNavRail *self, const char *icon_name, const char *label)
{
  Item *item = g_new0 (Item, 1);
  item->rail = self;
  item->icon = gtk_image_new_from_icon_name (icon_name);
  gtk_image_set_pixel_size (GTK_IMAGE (item->icon), ICON);
  gtk_widget_add_css_class (item->icon, "vibeos-nav-item");
  item->label = gtk_label_new (label);
  gtk_label_set_ellipsize (GTK_LABEL (item->label), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (item->label, "vibeos-nav-item");
  gtk_widget_add_css_class (item->label, "vibeos-nav-label");
  gtk_widget_set_child_visible (item->label, self->labels);
  /* the icon names the item when the label is hidden */
  gtk_accessible_update_property (GTK_ACCESSIBLE (item->icon), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
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
vibeos_nav_rail_set_selected (VibeosNavRail *self, int index)
{
  select_item (self, index, FALSE);
}

int
vibeos_nav_rail_get_selected (VibeosNavRail *self)
{
  return self->selected;
}

void
vibeos_nav_rail_set_labels (VibeosNavRail *self, gboolean labels)
{
  self->labels = !!labels;
  for (guint i = 0; i < self->items->len; i++)
    gtk_widget_set_child_visible (((Item *) g_ptr_array_index (self->items, i))->label, self->labels);
  if (labels)
    gtk_widget_remove_css_class (GTK_WIDGET (self), "icons-only");
  else
    gtk_widget_add_css_class (GTK_WIDGET (self), "icons-only");
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

void
vibeos_nav_rail_set_header (VibeosNavRail *self, GtkWidget *header)
{
  g_clear_pointer (&self->header, gtk_widget_unparent);
  self->header = header;
  if (header)
    gtk_widget_set_parent (header, GTK_WIDGET (self));
  gtk_widget_queue_resize (GTK_WIDGET (self));
}
