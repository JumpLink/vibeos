/* The overflow menu: see clock-popup-menu.h. The motion is the Dialer's: the card's clip grows away from the
 * anchor (down below it, up above it) while it fades in. */
#include "clock-popup-menu.h"

#include <vibeos.h>

#define ROW_HEIGHT      48
#define TEXT_PAD        16      /* the text's inset at both ends */
#define CORNER          4
#define END_MARGIN      4       /* the card's ends, at least this far in from the window's */
#define ANCHOR_GAP      4       /* between the anchor's edge and the card's */

typedef struct
{
  ClockMenuAction action;
  gpointer data;
} Row;

struct _ClockPopupMenu
{
  GtkWidget parent;
  GtkWidget *card;
  GArray *rows;                 /* Row, by the rows' order */
  graphene_rect_t anchor_bounds;
  VibeosAnim *anim;
  double shown;                 /* 0 … 1: the modal motion's value */
  gboolean closing;
  gboolean upward;              /* it opened above the anchor */
};

G_DEFINE_FINAL_TYPE (ClockPopupMenu, clock_popup_menu, GTK_TYPE_WIDGET)

static void
clock_popup_menu_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                           int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 0;      /* it covers the window it is in, to see the taps outside the card */
}

static void
clock_popup_menu_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  ClockPopupMenu *self = CLOCK_POPUP_MENU (widget);
  const graphene_rect_t *a = &self->anchor_bounds;
  int min, nat, w, h;
  double x, y, below, above;

  gtk_widget_measure (self->card, GTK_ORIENTATION_HORIZONTAL, -1, &min, &nat, NULL, NULL);
  w = MIN (nat, width - 2 * END_MARGIN);
  gtk_widget_measure (self->card, GTK_ORIENTATION_VERTICAL, w, &min, &nat, NULL, NULL);
  h = MIN (nat, height);
  x = MIN (a->origin.x + a->size.width, width - END_MARGIN) - w;
  x = MAX (x, 0);
  /* next to the anchor, never over it (libvibeos's VibeosMenu): below it, or above it when it does not fit below;
   * only when it fits neither way is it pushed onto the anchor, from the side with more room */
  below = height - END_MARGIN - (a->origin.y + a->size.height + ANCHOR_GAP);
  above = a->origin.y - ANCHOR_GAP - END_MARGIN;
  self->upward = h > below && (h <= above || above > below);
  if (!self->upward)
    y = MIN (a->origin.y + a->size.height + ANCHOR_GAP, height - END_MARGIN - h);
  else
    y = MAX (a->origin.y - ANCHOR_GAP - h, END_MARGIN);
  y = MAX (y, 0);
  gtk_widget_allocate (self->card, w, h, -1, gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (x, y)));
}

static void
clock_popup_menu_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  ClockPopupMenu *self = CLOCK_POPUP_MENU (widget);
  graphene_rect_t card;
  GskRoundedRect shape;
  graphene_point_t origin;

  if (!gtk_widget_compute_bounds (self->card, widget, &card))
    return;
  /* the modal motion, as About's dialog opens (vibeos-anim.h): grown out of the anchor's corner as it fades in */
  vibeos_modal_menu_origin (&card, &self->anchor_bounds, &origin);
  vibeos_modal_snapshot_push (snapshot, self->shown, &origin);
  /* elevation level 2: a faint shadow */
  gsk_rounded_rect_init_from_rect (&shape, &card, CORNER);
  gtk_snapshot_append_outset_shadow (snapshot, &shape, (GdkRGBA[]) { vibeos_color_alpha (VIBEOS_SHADOW, 0.3f) },
                                     0, 2, 0, 6);
  gtk_widget_snapshot_child (widget, self->card, snapshot);
  vibeos_modal_snapshot_pop (snapshot);
}

static void
on_anim (VibeosAnim *anim, double value, gpointer data)
{
  ClockPopupMenu *self = data;

  self->shown = value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  if (self->closing && !vibeos_anim_is_running (anim))
    {
      GtkWidget *overlay = gtk_widget_get_parent (GTK_WIDGET (self));

      if (overlay)
        gtk_overlay_remove_overlay (GTK_OVERLAY (overlay), GTK_WIDGET (self));
    }
}

static void
dismiss (ClockPopupMenu *self)
{
  if (self->closing)
    return;
  self->closing = TRUE;
  vibeos_anim_start_modal (self->anim, FALSE);
}

static void
on_row_clicked (GtkWidget *button, ClockPopupMenu *self)
{
  Row row = g_array_index (self->rows, Row, GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "menu-row")));

  if (self->closing)
    return;
  /* the menu goes, then the action runs: it may open a dialog over where the menu was */
  dismiss (self);
  row.action (row.data);
}

static void
on_pressed (GtkGestureClick *click, int n_press, double x, double y, ClockPopupMenu *self)
{
  graphene_rect_t card;

  if (gtk_widget_compute_bounds (self->card, GTK_WIDGET (self), &card)
      && graphene_rect_contains_point (&card, &GRAPHENE_POINT_INIT (x, y)))
    return;
  /* a touch outside: the menu goes and the touch does nothing else */
  gtk_gesture_set_state (GTK_GESTURE (click), GTK_EVENT_SEQUENCE_CLAIMED);
  dismiss (self);
}

static gboolean
on_key (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, ClockPopupMenu *self)
{
  if (keyval != GDK_KEY_Escape && keyval != GDK_KEY_Back)
    return FALSE;
  dismiss (self);
  return TRUE;
}

static void
clock_popup_menu_dispose (GObject *object)
{
  ClockPopupMenu *self = CLOCK_POPUP_MENU (object);

  g_clear_pointer (&self->anim, vibeos_anim_free);
  g_clear_pointer (&self->card, gtk_widget_unparent);
  g_clear_pointer (&self->rows, g_array_unref);
  G_OBJECT_CLASS (clock_popup_menu_parent_class)->dispose (object);
}

static void
clock_popup_menu_class_init (ClockPopupMenuClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = clock_popup_menu_dispose;
  widget_class->measure = clock_popup_menu_measure;
  widget_class->size_allocate = clock_popup_menu_size_allocate;
  widget_class->snapshot = clock_popup_menu_snapshot;
  gtk_widget_class_set_css_name (widget_class, "clock-popup-menu");
}

static void
clock_popup_menu_init (ClockPopupMenu *self)
{
  GtkGesture *click = gtk_gesture_click_new ();
  GtkEventController *keys = gtk_event_controller_key_new ();

  self->rows = g_array_new (FALSE, FALSE, sizeof (Row));
  self->card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (self->card, "clock-popup-menu");
  gtk_widget_set_overflow (self->card, GTK_OVERFLOW_HIDDEN);
  gtk_widget_set_parent (self->card, GTK_WIDGET (self));
  self->anim = vibeos_anim_new (GTK_WIDGET (self), on_anim, self);
  g_signal_connect (click, "pressed", G_CALLBACK (on_pressed), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key), self);
  gtk_widget_add_controller (GTK_WIDGET (self), keys);
}

ClockPopupMenu *
clock_popup_menu_new (void)
{
  return g_object_new (CLOCK_TYPE_POPUP_MENU, NULL);
}

void
clock_popup_menu_append (ClockPopupMenu *self, const char *label, ClockMenuAction action, gpointer data)
{
  GtkWidget *text = gtk_label_new (label);
  Row row = { action, data };
  GtkWidget *item;

  /* one line, ellipsized */
  gtk_label_set_xalign (GTK_LABEL (text), 0);
  gtk_label_set_ellipsize (GTK_LABEL (text), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (GTK_LABEL (text), 40);
  gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (text, TEXT_PAD);
  gtk_widget_set_margin_end (text, TEXT_PAD);
  gtk_widget_add_css_class (text, "clock-popup-menu-text");
  vibeos_label_set_font_padding_sp (text, 16);
  item = vibeos_pressable_new (text);
  gtk_widget_set_size_request (item, -1, ROW_HEIGHT);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (item), VIBEOS_RIPPLE_BOUNDED, 0);
  g_object_set_data (G_OBJECT (item), "menu-row", GUINT_TO_POINTER (self->rows->len));
  g_signal_connect (item, "clicked", G_CALLBACK (on_row_clicked), self);
  gtk_box_append (GTK_BOX (self->card), item);
  g_array_append_val (self->rows, row);
}

static void
present (ClockPopupMenu *self, GtkOverlay *overlay)
{
  gtk_overlay_add_overlay (overlay, GTK_WIDGET (self));
  gtk_overlay_set_measure_overlay (overlay, GTK_WIDGET (self), FALSE);
  gtk_overlay_set_clip_overlay (overlay, GTK_WIDGET (self), TRUE);
  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
  gtk_widget_grab_focus (GTK_WIDGET (self));
  vibeos_anim_start_modal (self->anim, TRUE);
}

void
clock_popup_menu_present (ClockPopupMenu *self, GtkWidget *anchor)
{
  GtkOverlay *overlay = vibeos_window_get_overlay (anchor);
  GtkWidget *root = GTK_WIDGET (gtk_widget_get_root (anchor));

  g_return_if_fail (overlay != NULL);
  if (!gtk_widget_compute_bounds (anchor, root, &self->anchor_bounds))
    graphene_rect_init (&self->anchor_bounds, 0, 0, 0, 0);
  present (self, overlay);
}
