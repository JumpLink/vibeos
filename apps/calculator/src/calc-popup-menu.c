/* CalcPopupMenu: see calc-popup-menu.h. The motion and the way it covers the window come from the Dialer's
 * popup-menu.c; the sizes and colours are the calculator's own. */
#include "calc-popup-menu.h"
#include "calc-text.h"

#include <math.h>
#include <vibeos.h>

#define ROW_HEIGHT      48
#define LIST_PAD        8
#define TEXT_PAD        16
#define TEXT_SIZE       16
#define LETTER_SPACING  0.03125 /* em, between the letters of a row's text */
#define CORNER          4       /* the card's corners */
#define END_MARGIN      4       /* between the card and the window's sides */
#define TOP_OFFSET      1       /* device px: the card's top under the anchor's */
#define ENTER_MS        250
#define EXIT_MS         150

typedef struct {
  char          *label;
  CalcMenuAction action;
  gpointer       data;
  PangoLayout   *layout;
  VibeosRipple      ripple;
} Row;

struct _CalcPopupMenu {
  GtkWidget       parent;
  GArray         *rows;
  graphene_rect_t anchor;
  graphene_rect_t card;
  VibeosAnim        *anim;
  double          shown;
  gboolean        closing;
  int             pressed;      /* the row under the finger, -1: none */
};

G_DEFINE_FINAL_TYPE (CalcPopupMenu, calc_popup_menu, GTK_TYPE_WIDGET)

static void
calc_popup_menu_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                         int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 0;   /* it covers the window, to see the taps outside the card */
}

static void
calc_popup_menu_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  CalcPopupMenu *self = CALC_POPUP_MENU (widget);
  int scale = vibeos_device_scale (widget);
  double text = 0;

  for (guint i = 0; i < self->rows->len; i++) {
    Row *row = &g_array_index (self->rows, Row, i);

    if (!row->layout)
      row->layout = calc_text_ui_layout (widget, row->label, TEXT_SIZE, CALC_WEIGHT_REGULAR, LETTER_SPACING);
    text = MAX (text, ceil (calc_text_width (row->layout) * scale) / scale);
  }
  double w = MIN (text + 2 * TEXT_PAD, width - 2 * END_MARGIN);
  double h = self->rows->len * ROW_HEIGHT + 2 * LIST_PAD;
  double x = width - END_MARGIN - w;
  double y = MIN (self->anchor.origin.y + (double) TOP_OFFSET / scale, height - h);

  graphene_rect_init (&self->card, x, MAX (0, y), w, h);
}

static void
calc_popup_menu_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CalcPopupMenu *self = CALC_POPUP_MENU (widget);
  int scale = vibeos_device_scale (widget);
  double reveal = self->closing ? 1 : spongy_curve (SPONGY_FAST_OUT_SLOW_IN, self->shown);
  GdkRGBA background = vibeos_color (VIBEOS_SURFACE_CONTAINER), text = vibeos_color (VIBEOS_ON_SURFACE);
  GskRoundedRect shape;
  double px = TEXT_SIZE * scale;
  /* the text's box (font padding) centred in its row, rounded down: the baseline 90 px into a 144 px row */
  double box = ceil (CALC_BOTTOM * px) - floor (-CALC_TOP * px);
  double baseline = (floor ((ROW_HEIGHT * scale - box) / 2) - floor (-CALC_TOP * px)) / scale;

  gtk_snapshot_push_opacity (snapshot, self->closing ? self->shown : MIN (1, self->shown * 2));
  gsk_rounded_rect_init_from_rect (&shape, &self->card, CORNER);
  /* a faint shadow, just enough to lift the card off the background */
  gtk_snapshot_append_outset_shadow (snapshot, &shape, &(GdkRGBA) { 0, 0, 0, 0.3f }, 0, 2, 0, 6);
  gtk_snapshot_push_clip (snapshot, &GRAPHENE_RECT_INIT (self->card.origin.x, self->card.origin.y,
                                                         self->card.size.width, self->card.size.height * reveal));
  gtk_snapshot_push_rounded_clip (snapshot, &shape);
  gtk_snapshot_append_color (snapshot, &background, &self->card);
  for (guint i = 0; i < self->rows->len; i++) {
    Row *row = &g_array_index (self->rows, Row, i);
    graphene_rect_t r = GRAPHENE_RECT_INIT (self->card.origin.x, self->card.origin.y + LIST_PAD + i * ROW_HEIGHT,
                                            self->card.size.width, ROW_HEIGHT);

    if (vibeos_ripple_is_active (&row->ripple)) {
      GdkRGBA ripple = vibeos_color_alpha (VIBEOS_ON_SURFACE, 0.1f);
      vibeos_ripple_snapshot (&row->ripple, snapshot, &r, &ripple);
    }
    calc_text_draw (snapshot, row->layout, r.origin.x + TEXT_PAD, r.origin.y + baseline, &text);
  }
  gtk_snapshot_pop (snapshot);
  gtk_snapshot_pop (snapshot);
  gtk_snapshot_pop (snapshot);
}

static void
on_anim (VibeosAnim *anim, double value, gpointer data)
{
  CalcPopupMenu *self = data;

  self->shown = value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  if (self->closing && !vibeos_anim_is_running (anim)) {
    GtkWidget *overlay = gtk_widget_get_parent (GTK_WIDGET (self));

    if (overlay)
      gtk_overlay_remove_overlay (GTK_OVERLAY (overlay), GTK_WIDGET (self));
  }
}

void
calc_popup_menu_dismiss (CalcPopupMenu *self)
{
  if (self->closing)
    return;
  self->closing = TRUE;
  vibeos_anim_start (self->anim, self->shown, 0, EXIT_MS, 0, SPONGY_LINEAR);
}

static int
row_at (CalcPopupMenu *self, double x, double y)
{
  if (!graphene_rect_contains_point (&self->card, &GRAPHENE_POINT_INIT (x, y)))
    return -2;   /* outside */
  y -= self->card.origin.y + LIST_PAD;
  if (y < 0 || y >= self->rows->len * ROW_HEIGHT)
    return -1;
  return (int) (y / ROW_HEIGHT);
}

static void
on_pressed (GtkGestureClick *click, int n_press, double x, double y, CalcPopupMenu *self)
{
  int row = row_at (self, x, y);

  if (self->closing)
    return;
  if (row == -2) {
    /* a touch outside: the menu goes, and the touch does nothing else */
    gtk_gesture_set_state (GTK_GESTURE (click), GTK_EVENT_SEQUENCE_CLAIMED);
    calc_popup_menu_dismiss (self);
    return;
  }
  self->pressed = row;
  if (row >= 0) {
    Row *r = &g_array_index (self->rows, Row, row);
    vibeos_ripple_press (&r->ripple, x - self->card.origin.x, y - self->card.origin.y - LIST_PAD - row * ROW_HEIGHT);
  }
}

static void
on_released (GtkGestureClick *click, int n_press, double x, double y, CalcPopupMenu *self)
{
  int row = self->pressed;

  self->pressed = -1;
  if (row < 0)
    return;
  vibeos_ripple_release (&g_array_index (self->rows, Row, row).ripple);
  if (row_at (self, x, y) != row || self->closing)
    return;
  /* the menu goes, then the action runs: it may open a dialog where the menu was */
  calc_popup_menu_dismiss (self);
  {
    Row *r = &g_array_index (self->rows, Row, row);
    r->action (r->data);
  }
}

static void
on_stopped (GtkGestureClick *click, CalcPopupMenu *self)
{
  if (self->pressed >= 0)
    vibeos_ripple_release (&g_array_index (self->rows, Row, self->pressed).ripple);
  self->pressed = -1;
}

static gboolean
on_key (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, CalcPopupMenu *self)
{
  if (keyval != GDK_KEY_Escape && keyval != GDK_KEY_Back)
    return FALSE;
  calc_popup_menu_dismiss (self);
  return TRUE;
}

static void
calc_popup_menu_dispose (GObject *object)
{
  CalcPopupMenu *self = CALC_POPUP_MENU (object);

  g_clear_pointer (&self->anim, vibeos_anim_free);
  if (self->rows) {
    for (guint i = 0; i < self->rows->len; i++) {
      Row *row = &g_array_index (self->rows, Row, i);
      vibeos_ripple_clear (&row->ripple);
      g_clear_object (&row->layout);
      g_free (row->label);
    }
    g_clear_pointer (&self->rows, g_array_unref);
  }
  G_OBJECT_CLASS (calc_popup_menu_parent_class)->dispose (object);
}

static void
calc_popup_menu_class_init (CalcPopupMenuClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = calc_popup_menu_dispose;
  widget_class->measure = calc_popup_menu_measure;
  widget_class->size_allocate = calc_popup_menu_size_allocate;
  widget_class->snapshot = calc_popup_menu_snapshot;
  gtk_widget_class_set_css_name (widget_class, "calc-popup-menu");
  /* the shell's back gesture sends a dialog Escape, which closes it, rather than leaving the app */
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_DIALOG);
}

static void
calc_popup_menu_init (CalcPopupMenu *self)
{
  GtkGesture *click = gtk_gesture_click_new ();
  GtkEventController *keys = gtk_event_controller_key_new ();

  self->rows = g_array_new (FALSE, TRUE, sizeof (Row));
  self->pressed = -1;
  self->anim = vibeos_anim_new (GTK_WIDGET (self), on_anim, self);
  g_signal_connect (click, "pressed", G_CALLBACK (on_pressed), self);
  g_signal_connect (click, "released", G_CALLBACK (on_released), self);
  g_signal_connect (click, "stopped", G_CALLBACK (on_stopped), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key), self);
  gtk_widget_add_controller (GTK_WIDGET (self), keys);
}

CalcPopupMenu *
calc_popup_menu_new (void)
{
  return g_object_new (CALC_TYPE_POPUP_MENU, NULL);
}

void
calc_popup_menu_append (CalcPopupMenu *self, const char *label, CalcMenuAction action, gpointer data)
{
  Row row = { g_strdup (label), action, data, NULL };

  g_array_append_val (self->rows, row);
  vibeos_ripple_init (&g_array_index (self->rows, Row, self->rows->len - 1).ripple, GTK_WIDGET (self));
}

void
calc_popup_menu_present (CalcPopupMenu *self, GtkWidget *anchor)
{
  GtkOverlay *overlay = vibeos_window_get_overlay (anchor);

  g_return_if_fail (overlay != NULL);
  /* the rows' ripples point at this widget: their array must not move from here on */
  if (!gtk_widget_compute_bounds (anchor, GTK_WIDGET (overlay), &self->anchor))
    graphene_rect_init (&self->anchor, 0, 0, 0, 0);
  gtk_overlay_add_overlay (overlay, GTK_WIDGET (self));
  gtk_overlay_set_measure_overlay (overlay, GTK_WIDGET (self), FALSE);
  gtk_overlay_set_clip_overlay (overlay, GTK_WIDGET (self), TRUE);
  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
  gtk_widget_grab_focus (GTK_WIDGET (self));
  vibeos_anim_start (self->anim, 0, 1, ENTER_MS, 0, SPONGY_LINEAR);
}
