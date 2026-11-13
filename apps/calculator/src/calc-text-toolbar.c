/* CalcTextToolbar: see calc-text-toolbar.h */
#include "calc-text-toolbar.h"
#include "calc-text.h"

#include <math.h>
#include <neo.h>

#define HEIGHT          48      /* the bar's height */
#define BUTTON_PAD      16      /* each side of a button's text */
#define MIN_BUTTON      48      /* a button's least width */
#define TEXT_SIZE       14
#define CORNER          8       /* the bar's corners */
#define MARGIN_V        8       /* between the bar and the text it is about */
#define MARGIN_H        16      /* the bar keeps this far inside the window */
#define ENTER_MS        150     /* the toolbar fades in */
#define EXIT_MS         100

typedef struct {
  char             *label;
  CalcToolbarAction action;
  gpointer          data;
  PangoLayout      *layout;
  double            x, width;   /* inside the bar */
  NeoRipple         ripple;
} Item;

struct _CalcTextToolbar {
  GtkWidget       parent;
  GArray         *items;
  graphene_rect_t content;      /* in the overlay's coordinates */
  graphene_rect_t bar;
  NeoAnim        *anim;
  double          shown;
  gboolean        closing;
  int             pressed;
};

G_DEFINE_FINAL_TYPE (CalcTextToolbar, calc_text_toolbar, GTK_TYPE_WIDGET)

enum { CLOSED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
calc_text_toolbar_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                           int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 0;
}

static void
calc_text_toolbar_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  CalcTextToolbar *self = CALC_TEXT_TOOLBAR (widget);
  double w = 0, x, y;

  for (guint i = 0; i < self->items->len; i++) {
    Item *item = &g_array_index (self->items, Item, i);

    if (!item->layout)
      item->layout = calc_text_layout (widget, item->label, TEXT_SIZE, CALC_WEIGHT_MEDIUM);
    item->x = w;
    item->width = MAX (MIN_BUTTON, ceil (calc_text_width (item->layout)) + 2 * BUTTON_PAD);
    w += item->width;
  }
  /* centred on the content, inside the window's margins; above it if it fits, else below */
  x = self->content.origin.x + self->content.size.width / 2 - w / 2;
  x = CLAMP (x, MARGIN_H, MAX (MARGIN_H, width - MARGIN_H - w));
  y = self->content.origin.y - MARGIN_V - HEIGHT;
  if (y < 0)
    y = self->content.origin.y + self->content.size.height + MARGIN_V;
  graphene_rect_init (&self->bar, x, MIN (y, height - HEIGHT), w, HEIGHT);
}

static void
calc_text_toolbar_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CalcTextToolbar *self = CALC_TEXT_TOOLBAR (widget);
  GdkRGBA background = neo_color (NEO_SURFACE_CONTAINER_HIGH), text = neo_color (NEO_ON_SURFACE);
  GskRoundedRect shape;

  gtk_snapshot_push_opacity (snapshot, self->shown);
  gsk_rounded_rect_init_from_rect (&shape, &self->bar, CORNER);
  gtk_snapshot_append_outset_shadow (snapshot, &shape, &(GdkRGBA) { 0, 0, 0, 0.35f }, 0, 4, 0, 8);
  gtk_snapshot_push_rounded_clip (snapshot, &shape);
  gtk_snapshot_append_color (snapshot, &background, &self->bar);
  for (guint i = 0; i < self->items->len; i++) {
    Item *item = &g_array_index (self->items, Item, i);
    graphene_rect_t r = GRAPHENE_RECT_INIT (self->bar.origin.x + item->x, self->bar.origin.y, item->width, HEIGHT);
    double tw = calc_text_width (item->layout);

    if (neo_ripple_is_active (&item->ripple)) {
      GdkRGBA ripple = neo_color_alpha (NEO_ON_SURFACE, 0.1f);
      neo_ripple_snapshot (&item->ripple, snapshot, &r, &ripple);
    }
    calc_text_draw (snapshot, item->layout, r.origin.x + (r.size.width - tw) / 2,
                    r.origin.y + HEIGHT / 2.0 + (CALC_ASCENT - CALC_DESCENT) * TEXT_SIZE / 2, &text);
  }
  gtk_snapshot_pop (snapshot);
  gtk_snapshot_pop (snapshot);
}

static void
on_anim (NeoAnim *anim, double value, gpointer data)
{
  CalcTextToolbar *self = data;

  self->shown = value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  if (self->closing && !neo_anim_is_running (anim)) {
    GtkWidget *overlay = gtk_widget_get_parent (GTK_WIDGET (self));

    if (overlay)
      gtk_overlay_remove_overlay (GTK_OVERLAY (overlay), GTK_WIDGET (self));
  }
}

void
calc_text_toolbar_dismiss (CalcTextToolbar *self)
{
  if (self->closing)
    return;
  self->closing = TRUE;
  /* the toolbar no longer takes touches: what is under it gets them at once */
  gtk_widget_set_can_target (GTK_WIDGET (self), FALSE);
  g_signal_emit (self, signals[CLOSED], 0);
  neo_anim_start (self->anim, self->shown, 0, EXIT_MS, 0, SPONGY_LINEAR);
}

static int
item_at (CalcTextToolbar *self, double x, double y)
{
  if (!graphene_rect_contains_point (&self->bar, &GRAPHENE_POINT_INIT (x, y)))
    return -1;
  x -= self->bar.origin.x;
  for (guint i = 0; i < self->items->len; i++) {
    Item *item = &g_array_index (self->items, Item, i);
    if (x >= item->x && x < item->x + item->width)
      return i;
  }
  return -1;
}

static gboolean
contains (GtkWidget *widget, double x, double y)
{
  CalcTextToolbar *self = CALC_TEXT_TOOLBAR (widget);

  /* only the bar takes touches: the rest of the window is the app's, which closes it on any touch */
  return !self->closing && graphene_rect_contains_point (&self->bar, &GRAPHENE_POINT_INIT (x, y));
}

static void
on_pressed (GtkGestureClick *click, int n_press, double x, double y, CalcTextToolbar *self)
{
  int i = item_at (self, x, y);

  self->pressed = i;
  if (i >= 0) {
    Item *item = &g_array_index (self->items, Item, i);
    neo_ripple_press (&item->ripple, x - self->bar.origin.x - item->x, y - self->bar.origin.y);
  }
}

static void
on_released (GtkGestureClick *click, int n_press, double x, double y, CalcTextToolbar *self)
{
  int i = self->pressed;

  self->pressed = -1;
  if (i < 0)
    return;
  neo_ripple_release (&g_array_index (self->items, Item, i).ripple);
  if (item_at (self, x, y) != i || self->closing)
    return;
  {
    Item *item = &g_array_index (self->items, Item, i);
    CalcToolbarAction action = item->action;
    gpointer data = item->data;

    /* the action, then the toolbar goes if it was done */
    g_object_ref (self);
    if (action (data))
      calc_text_toolbar_dismiss (self);
    g_object_unref (self);
  }
}

static void
on_stopped (GtkGestureClick *click, CalcTextToolbar *self)
{
  if (self->pressed >= 0)
    neo_ripple_release (&g_array_index (self->items, Item, self->pressed).ripple);
  self->pressed = -1;
}

static gboolean
on_key (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, CalcTextToolbar *self)
{
  if (keyval != GDK_KEY_Escape && keyval != GDK_KEY_Back)
    return FALSE;
  calc_text_toolbar_dismiss (self);
  return TRUE;
}

static void
calc_text_toolbar_dispose (GObject *object)
{
  CalcTextToolbar *self = CALC_TEXT_TOOLBAR (object);

  g_clear_pointer (&self->anim, neo_anim_free);
  if (self->items) {
    for (guint i = 0; i < self->items->len; i++) {
      Item *item = &g_array_index (self->items, Item, i);
      neo_ripple_clear (&item->ripple);
      g_clear_object (&item->layout);
      g_free (item->label);
    }
    g_clear_pointer (&self->items, g_array_unref);
  }
  G_OBJECT_CLASS (calc_text_toolbar_parent_class)->dispose (object);
}

static void
calc_text_toolbar_class_init (CalcTextToolbarClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = calc_text_toolbar_dispose;
  widget_class->measure = calc_text_toolbar_measure;
  widget_class->size_allocate = calc_text_toolbar_size_allocate;
  widget_class->snapshot = calc_text_toolbar_snapshot;
  widget_class->contains = contains;
  gtk_widget_class_set_css_name (widget_class, "calc-text-toolbar");
  /* the shell's back gesture sends a dialog Escape, which closes it, rather than leaving the app */
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_DIALOG);
  signals[CLOSED] = g_signal_new ("closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 0);
}

static void
calc_text_toolbar_init (CalcTextToolbar *self)
{
  GtkGesture *click = gtk_gesture_click_new ();
  GtkEventController *keys = gtk_event_controller_key_new ();

  self->items = g_array_new (FALSE, TRUE, sizeof (Item));
  self->pressed = -1;
  self->anim = neo_anim_new (GTK_WIDGET (self), on_anim, self);
  g_signal_connect (click, "pressed", G_CALLBACK (on_pressed), self);
  g_signal_connect (click, "released", G_CALLBACK (on_released), self);
  g_signal_connect (click, "stopped", G_CALLBACK (on_stopped), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key), self);
  gtk_widget_add_controller (GTK_WIDGET (self), keys);
}

CalcTextToolbar *
calc_text_toolbar_new (void)
{
  return g_object_new (CALC_TYPE_TEXT_TOOLBAR, NULL);
}

void
calc_text_toolbar_append (CalcTextToolbar *self, const char *label, CalcToolbarAction action, gpointer data)
{
  Item item = { g_strdup (label), action, data, NULL };

  g_array_append_val (self->items, item);
  neo_ripple_init (&g_array_index (self->items, Item, self->items->len - 1).ripple, GTK_WIDGET (self));
}

void
calc_text_toolbar_present (CalcTextToolbar *self, GtkWidget *widget, const graphene_rect_t *content)
{
  GtkOverlay *overlay = neo_window_get_overlay (widget);

  g_return_if_fail (overlay != NULL);
  {
    /* both corners, through whatever scale the widget is drawn at (a result in the formula's place) */
    graphene_point_t a, b;
    graphene_point_t far = GRAPHENE_POINT_INIT (content->origin.x + content->size.width,
                                                content->origin.y + content->size.height);

    if (gtk_widget_compute_point (widget, GTK_WIDGET (overlay), &content->origin, &a)
        && gtk_widget_compute_point (widget, GTK_WIDGET (overlay), &far, &b))
      graphene_rect_init (&self->content, a.x, a.y, b.x - a.x, b.y - a.y);
    else
      graphene_rect_init (&self->content, 0, 0, 0, 0);
  }
  gtk_overlay_add_overlay (overlay, GTK_WIDGET (self));
  gtk_overlay_set_measure_overlay (overlay, GTK_WIDGET (self), FALSE);
  gtk_overlay_set_clip_overlay (overlay, GTK_WIDGET (self), TRUE);
  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
  gtk_widget_grab_focus (GTK_WIDGET (self));
  neo_anim_start (self->anim, 0, 1, ENTER_MS, 0, SPONGY_LINEAR);
}
