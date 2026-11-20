/* The pop-up menu: see popup-menu.h. On opening, the card's clip grows down from the anchor while it fades in. */
#include "popup-menu.h"

#include <vibeos.h>

#define MIN_WIDTH       196
#define ROW_HEIGHT      48
#define TEXT_PAD        16      /* the text's inset at both ends */
#define CORNER          2
#define OVERFLOW_INSET  4       /* an overflow menu's end, this far in from its anchor's */
#define OVERFLOW_HALF   24      /* half an overflow button: the card's top on the button's top */
#define EDGE            8       /* closer than this to the window's edge, it is flush with it */
#define ENTER_MS        250     /* the clip reveal and the fade in */
#define EXIT_MS         150

typedef struct
{
  PopupMenuAction action;
  gpointer data;
} Row;

struct _DialerPopupMenu
{
  GtkWidget parent;
  GtkWidget *card;
  GArray *rows;                 /* Row, by the rows' order */
  PopupMenuGravity gravity;
  int corner;
  graphene_rect_t anchor_bounds;
  VibeosAnim *anim;
  double shown;                 /* 0 … 1 */
  gboolean closing;
};

G_DEFINE_FINAL_TYPE (DialerPopupMenu, dialer_popup_menu, GTK_TYPE_WIDGET)

static void
dialer_popup_menu_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                           int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 0;      /* it covers the window it is in, to see the taps outside the card */
}

static void
dialer_popup_menu_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  DialerPopupMenu *self = DIALER_POPUP_MENU (widget);
  const graphene_rect_t *a = &self->anchor_bounds;
  int min, nat, w, h;
  double x, y;

  gtk_widget_measure (self->card, GTK_ORIENTATION_HORIZONTAL, -1, &min, &nat, NULL, NULL);
  w = MIN (MAX (nat, MIN_WIDTH), width);
  gtk_widget_measure (self->card, GTK_ORIENTATION_VERTICAL, w, &min, &nat, NULL, NULL);
  h = MIN (nat, height);
  if (self->gravity == POPUP_MENU_OVERFLOW)
    {
      /* only kept on the screen, never snapped to its edge: its end stays OVERFLOW_INSET in from the anchor's */
      x = CLAMP (a->origin.x + a->size.width - OVERFLOW_INSET - w, 0, width - w);
      y = MIN (a->origin.y + a->size.height / 2 - OVERFLOW_HALF, height - h);
      gtk_widget_allocate (self->card, w, h, -1, gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (x, MAX (y, 0))));
      return;
    }
  x = self->gravity == POPUP_MENU_END ? a->origin.x + a->size.width - w : a->origin.x;
  if (x + w > width - EDGE)
    x = width - w;
  if (x < EDGE)
    x = 0;
  if (self->gravity == POPUP_MENU_BELOW)
    {
      /* under the anchor; with no room there, over it */
      y = a->origin.y + a->size.height;
      if (y + h > height && a->origin.y - h >= 0)
        y = a->origin.y - h;
      y = MIN (y, height - h);
    }
  else
    /* over the anchor: its top on the anchor's; one that would run off the foot goes up as far as it must */
    y = MIN (a->origin.y, height - h);
  y = MAX (y, 0);
  gtk_widget_allocate (self->card, w, h, -1, gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (x, y)));
}

static void
dialer_popup_menu_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  DialerPopupMenu *self = DIALER_POPUP_MENU (widget);
  graphene_rect_t card;
  GskRoundedRect shape;
  GdkRGBA far, near;
  double reveal = self->closing ? 1 : spongy_curve (SPONGY_FAST_OUT_SLOW_IN, self->shown);

  if (!gtk_widget_compute_bounds (self->card, widget, &card))
    return;
  gtk_snapshot_push_opacity (snapshot, self->closing ? self->shown : MIN (1, self->shown * 2));
  /* a soft shadow under the card */
  gsk_rounded_rect_init_from_rect (&shape, &card, self->corner);
  far = vibeos_color_alpha (VIBEOS_SHADOW, 0.3f);
  near = vibeos_color_alpha (VIBEOS_SHADOW, 0.2f);
  gtk_snapshot_append_outset_shadow (snapshot, &shape, &far, 0, 8, 2, 16);
  gtk_snapshot_append_outset_shadow (snapshot, &shape, &near, 0, 2, 0, 4);
  /* the clip reveal: from the top edge down */
  gtk_snapshot_push_clip (snapshot, &GRAPHENE_RECT_INIT (card.origin.x, card.origin.y, card.size.width,
                                                         card.size.height * reveal));
  gtk_widget_snapshot_child (widget, self->card, snapshot);
  gtk_snapshot_pop (snapshot);
  gtk_snapshot_pop (snapshot);
}

static void
on_anim (VibeosAnim *anim, double value, gpointer data)
{
  DialerPopupMenu *self = data;

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
dismiss (DialerPopupMenu *self)
{
  if (self->closing)
    return;
  self->closing = TRUE;
  vibeos_anim_start (self->anim, self->shown, 0, EXIT_MS, 0, SPONGY_LINEAR);
}

static void
on_row_clicked (GtkWidget *button, DialerPopupMenu *self)
{
  Row row = g_array_index (self->rows, Row, GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "menu-row")));

  if (self->closing)
    return;
  /* the menu goes, then the action runs: it may open a dialog over where the menu was */
  dismiss (self);
  row.action (row.data);
}

static void
on_pressed (GtkGestureClick *click, int n_press, double x, double y, DialerPopupMenu *self)
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
on_key (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, DialerPopupMenu *self)
{
  if (keyval != GDK_KEY_Escape && keyval != GDK_KEY_Back)
    return FALSE;
  dismiss (self);
  return TRUE;
}

static void
dialer_popup_menu_dispose (GObject *object)
{
  DialerPopupMenu *self = DIALER_POPUP_MENU (object);

  g_clear_pointer (&self->anim, vibeos_anim_free);
  g_clear_pointer (&self->card, gtk_widget_unparent);
  g_clear_pointer (&self->rows, g_array_unref);
  G_OBJECT_CLASS (dialer_popup_menu_parent_class)->dispose (object);
}

static void
dialer_popup_menu_class_init (DialerPopupMenuClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = dialer_popup_menu_dispose;
  widget_class->measure = dialer_popup_menu_measure;
  widget_class->size_allocate = dialer_popup_menu_size_allocate;
  widget_class->snapshot = dialer_popup_menu_snapshot;
  gtk_widget_class_set_css_name (widget_class, "dialer-popup-menu");
}

static void
dialer_popup_menu_init (DialerPopupMenu *self)
{
  GtkGesture *click = gtk_gesture_click_new ();
  GtkEventController *keys = gtk_event_controller_key_new ();

  self->rows = g_array_new (FALSE, FALSE, sizeof (Row));
  self->corner = CORNER;
  self->card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (self->card, "popup-menu");
  gtk_widget_set_overflow (self->card, GTK_OVERFLOW_HIDDEN);
  gtk_widget_set_parent (self->card, GTK_WIDGET (self));
  self->anim = vibeos_anim_new (GTK_WIDGET (self), on_anim, self);
  g_signal_connect (click, "pressed", G_CALLBACK (on_pressed), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key), self);
  gtk_widget_add_controller (GTK_WIDGET (self), keys);
}

DialerPopupMenu *
dialer_popup_menu_new (void)
{
  return g_object_new (DIALER_TYPE_POPUP_MENU, NULL);
}

void
dialer_popup_menu_append (DialerPopupMenu *self, const char *label, PopupMenuAction action, gpointer data)
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
  gtk_widget_add_css_class (text, "popup-menu-text");
  vibeos_label_set_font_padding_sp (text, 16);
  if (!action)
    {
      gtk_widget_add_css_class (text, "disabled");
      gtk_widget_set_size_request (text, -1, ROW_HEIGHT);
      gtk_box_append (GTK_BOX (self->card), text);
      g_array_append_val (self->rows, row);
      return;
    }
  item = vibeos_pressable_new (text);
  gtk_widget_set_size_request (item, -1, ROW_HEIGHT);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (item), VIBEOS_RIPPLE_BOUNDED, 0);
  g_object_set_data (G_OBJECT (item), "menu-row", GUINT_TO_POINTER (self->rows->len));
  g_signal_connect (item, "clicked", G_CALLBACK (on_row_clicked), self);
  gtk_box_append (GTK_BOX (self->card), item);
  g_array_append_val (self->rows, row);
}

void
dialer_popup_menu_set_header (DialerPopupMenu *self, const char *header)
{
  GtkWidget *text = gtk_label_new (header);

  gtk_widget_add_css_class (self->card, "context");
  gtk_label_set_xalign (GTK_LABEL (text), 0);
  gtk_label_set_ellipsize (GTK_LABEL (text), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (GTK_LABEL (text), 40);
  gtk_widget_set_size_request (text, -1, ROW_HEIGHT);
  gtk_widget_set_margin_start (text, TEXT_PAD);
  gtk_widget_set_margin_end (text, TEXT_PAD);
  gtk_widget_add_css_class (text, "popup-menu-header");
  gtk_box_prepend (GTK_BOX (self->card), text);
}

void
dialer_popup_menu_set_corner (DialerPopupMenu *self, int radius)
{
  self->corner = radius;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
present (DialerPopupMenu *self, GtkOverlay *overlay)
{
  gtk_overlay_add_overlay (overlay, GTK_WIDGET (self));
  gtk_overlay_set_measure_overlay (overlay, GTK_WIDGET (self), FALSE);
  gtk_overlay_set_clip_overlay (overlay, GTK_WIDGET (self), TRUE);
  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
  gtk_widget_grab_focus (GTK_WIDGET (self));
  vibeos_anim_start (self->anim, 0, 1, ENTER_MS, 0, SPONGY_LINEAR);
}

void
dialer_popup_menu_present (DialerPopupMenu *self, GtkWidget *anchor, PopupMenuGravity gravity)
{
  GtkOverlay *overlay = vibeos_window_get_overlay (anchor);
  GtkWidget *root = GTK_WIDGET (gtk_widget_get_root (anchor));

  g_return_if_fail (overlay != NULL);
  self->gravity = gravity;
  if (!gtk_widget_compute_bounds (anchor, root, &self->anchor_bounds))
    graphene_rect_init (&self->anchor_bounds, 0, 0, 0, 0);
  present (self, overlay);
}

void
dialer_popup_menu_present_at (DialerPopupMenu *self, GtkWidget *widget, double x, double y)
{
  GtkOverlay *overlay = vibeos_window_get_overlay (widget);
  GtkWidget *root = GTK_WIDGET (gtk_widget_get_root (widget));
  graphene_point_t point;

  g_return_if_fail (overlay != NULL);
  self->gravity = POPUP_MENU_START;
  if (!gtk_widget_compute_point (widget, root, &GRAPHENE_POINT_INIT (x, y), &point))
    point = GRAPHENE_POINT_INIT (0, 0);
  graphene_rect_init (&self->anchor_bounds, point.x, point.y, 0, 0);
  present (self, overlay);
}
