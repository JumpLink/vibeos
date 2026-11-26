/* The overflow menu: see vibeos-menu.h. It opens next to its anchor, never over it: below it, or above it when there is
 * no room below. It opens and closes with the modal motion, as About's dialog (vibeos-anim.h). */
#include "vibeos-menu.h"

#include <glib/gi18n-lib.h>

#include "vibeos-anim.h"
#include "vibeos-pressable.h"
#include "vibeos-style.h"
#include "vibeos-text.h"

#include "vibeos-sheet.h"

#include <spongy.h>

#define ROW_HEIGHT      48
#define MIN_WIDTH       196     /* Android's popup menu rows are never narrower */
#define TEXT_PAD        16      /* the text's inset at both ends */
#define CORNER          4
#define END_MARGIN      4       /* the card's ends, at least this far in from the window's */
#define ANCHOR_GAP      4       /* between the anchor's edge and the card's */

typedef struct
{
  VibeosMenuAction action;
  gpointer data;
} Row;

struct _VibeosMenu
{
  GtkWidget parent;
  GtkWidget *card;
  GArray *rows;                 /* Row, by the rows' order */
  graphene_rect_t anchor_bounds;
  VibeosAnim *anim;
  double shown;                 /* 0 … 1: the modal motion's value */
  gboolean upward;              /* it opened above the anchor */
  gboolean closing;
};

G_DEFINE_FINAL_TYPE (VibeosMenu, vibeos_menu, GTK_TYPE_WIDGET)

static void
vibeos_menu_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                           int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 0;      /* it covers the window it is in, to see the taps outside the card */
}

static void
vibeos_menu_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  VibeosMenu *self = VIBEOS_MENU (widget);
  const graphene_rect_t *a = &self->anchor_bounds;
  int min, nat, w, h;
  double x, y;

  gtk_widget_measure (self->card, GTK_ORIENTATION_HORIZONTAL, -1, &min, &nat, NULL, NULL);
  double below, above;

  w = MIN (MAX (nat, MIN_WIDTH), width - 2 * END_MARGIN);
  gtk_widget_measure (self->card, GTK_ORIENTATION_VERTICAL, w, &min, &nat, NULL, NULL);
  h = MIN (nat, height);
  /* lined up with the anchor on the side it is on: its right edge for an anchor in the window's right half (a
   * header's ⋮), its left edge for one in the left half (a rail's menu button) */
  if (a->origin.x + a->size.width / 2 > width / 2.0)
    x = a->origin.x + a->size.width - w;
  else
    x = a->origin.x;
  x = CLAMP (x, END_MARGIN, MAX (END_MARGIN, width - END_MARGIN - w));
  /* below the anchor, never over it; above it when it does not fit below; only when it fits neither way is it pushed
   * onto the anchor, from the side with more room */
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
vibeos_menu_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  VibeosMenu *self = VIBEOS_MENU (widget);
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
  VibeosMenu *self = data;

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
dismiss (VibeosMenu *self)
{
  if (self->closing)
    return;
  self->closing = TRUE;
  vibeos_anim_start_modal (self->anim, FALSE);
}

static void
on_row_clicked (GtkWidget *button, VibeosMenu *self)
{
  Row row = g_array_index (self->rows, Row, GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "menu-row")));

  if (self->closing)
    return;
  /* the menu goes, then the action runs: it may open a dialog over where the menu was */
  dismiss (self);
  row.action (row.data);
}

static void
on_pressed (GtkGestureClick *click, int n_press, double x, double y, VibeosMenu *self)
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
on_key (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, VibeosMenu *self)
{
  gboolean alt_left = (keyval == GDK_KEY_Left || keyval == GDK_KEY_KP_Left) && (state & GDK_ALT_MASK);

  /* the window closes on Escape and Alt+Left: while the menu shows, they close only the menu */
  if (keyval != GDK_KEY_Escape && keyval != GDK_KEY_Back && !alt_left)
    return FALSE;
  dismiss (self);
  return TRUE;
}

static void
vibeos_menu_dispose (GObject *object)
{
  VibeosMenu *self = VIBEOS_MENU (object);

  g_clear_pointer (&self->anim, vibeos_anim_free);
  g_clear_pointer (&self->card, gtk_widget_unparent);
  g_clear_pointer (&self->rows, g_array_unref);
  G_OBJECT_CLASS (vibeos_menu_parent_class)->dispose (object);
}

static void
vibeos_menu_class_init (VibeosMenuClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = vibeos_menu_dispose;
  widget_class->measure = vibeos_menu_measure;
  widget_class->size_allocate = vibeos_menu_size_allocate;
  widget_class->snapshot = vibeos_menu_snapshot;
  gtk_widget_class_set_css_name (widget_class, "vibeos-menu");
}

static void
vibeos_menu_init (VibeosMenu *self)
{
  GtkGesture *click = gtk_gesture_click_new ();
  GtkEventController *keys = gtk_event_controller_key_new ();

  self->rows = g_array_new (FALSE, FALSE, sizeof (Row));
  self->card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (self->card, "vibeos-menu");
  gtk_widget_set_overflow (self->card, GTK_OVERFLOW_HIDDEN);
  gtk_widget_set_parent (self->card, GTK_WIDGET (self));
  self->anim = vibeos_anim_new (GTK_WIDGET (self), on_anim, self);
  g_signal_connect (click, "pressed", G_CALLBACK (on_pressed), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key), self);
  gtk_widget_add_controller (GTK_WIDGET (self), keys);
}

VibeosMenu *
vibeos_menu_new (void)
{
  return g_object_new (VIBEOS_TYPE_MENU, NULL);
}

void
vibeos_menu_append (VibeosMenu *self, const char *label, VibeosMenuAction action, gpointer data)
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
  gtk_widget_add_css_class (text, "vibeos-menu-text");
  vibeos_label_set_font_padding_sp (text, 16);
  item = vibeos_pressable_new (text);
  gtk_widget_set_size_request (item, -1, ROW_HEIGHT);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (item), VIBEOS_RIPPLE_BOUNDED, 0);
  g_object_set_data (G_OBJECT (item), "menu-row", GUINT_TO_POINTER (self->rows->len));
  g_signal_connect (item, "clicked", G_CALLBACK (on_row_clicked), self);
  gtk_box_append (GTK_BOX (self->card), item);
  g_array_append_val (self->rows, row);
}

/* the About row: the sheet opens over the window the menu was in (the menu is leaving it) */
static void
on_about (gpointer data)
{
  VibeosMenu *self = data;
  GtkWidget *parent = g_object_get_data (G_OBJECT (self), "vibeos-menu-anchor");

  if (parent)
    vibeos_about_present (parent, g_object_get_data (G_OBJECT (self), "vibeos-menu-about"));
}

void
vibeos_menu_append_about (VibeosMenu *self, const VibeosAboutInfo *info)
{
  g_object_set_data (G_OBJECT (self), "vibeos-menu-about", (gpointer) info);
  vibeos_menu_append (self, _("About"), on_about, self);
}

static void
present (VibeosMenu *self, GtkOverlay *overlay)
{
  gtk_overlay_add_overlay (overlay, GTK_WIDGET (self));
  gtk_overlay_set_measure_overlay (overlay, GTK_WIDGET (self), FALSE);
  gtk_overlay_set_clip_overlay (overlay, GTK_WIDGET (self), TRUE);
  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
  gtk_widget_grab_focus (GTK_WIDGET (self));
  vibeos_anim_start_modal (self->anim, TRUE);
}

void
vibeos_menu_present (VibeosMenu *self, GtkWidget *anchor)
{
  GtkOverlay *overlay = vibeos_window_get_overlay (anchor);

  g_return_if_fail (overlay != NULL);
  /* the About sheet's parent: the anchor while it is in the window */
  g_object_set_data_full (G_OBJECT (self), "vibeos-menu-anchor", g_object_ref (anchor), g_object_unref);
  /* where the menu is laid out: in the overlay */
  if (!gtk_widget_compute_bounds (anchor, GTK_WIDGET (overlay), &self->anchor_bounds))
    graphene_rect_init (&self->anchor_bounds, 0, 0, 0, 0);
  present (self, overlay);
}

/* ---- a menu made of the app's own widgets: the bin that moves it --------------------------------------------- */

#define VIBEOS_TYPE_MENU_BIN (vibeos_menu_bin_get_type ())
G_DECLARE_FINAL_TYPE (VibeosMenuBin, vibeos_menu_bin, VIBEOS, MENU_BIN, GtkWidget)

struct _VibeosMenuBin
{
  GtkWidget parent;
  GtkWidget *card;
  GtkWidget *anchor;            /* weak */
  GtkWidget *layer;             /* weak: what goes once it has closed */
  graphene_rect_t anchor_bounds;
  VibeosAnim *anim;
  double shown;
  gboolean closing;
};

G_DEFINE_FINAL_TYPE (VibeosMenuBin, vibeos_menu_bin, GTK_TYPE_WIDGET)

static void
vibeos_menu_bin_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                         int *minimum_baseline, int *natural_baseline)
{
  gtk_widget_measure (VIBEOS_MENU_BIN (widget)->card, orientation, for_size, minimum, natural, minimum_baseline,
                      natural_baseline);
}

static void
vibeos_menu_bin_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  gtk_widget_allocate (VIBEOS_MENU_BIN (widget)->card, width, height, baseline, NULL);
}

static void
vibeos_menu_bin_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  VibeosMenuBin *self = VIBEOS_MENU_BIN (widget);
  graphene_rect_t card = GRAPHENE_RECT_INIT (0, 0, gtk_widget_get_width (widget), gtk_widget_get_height (widget));
  graphene_rect_t at;
  graphene_point_t origin;

  /* where the anchor is, while it is there (the last place it was, after) */
  if (self->anchor && gtk_widget_compute_bounds (self->anchor, widget, &at))
    self->anchor_bounds = at;
  vibeos_modal_menu_origin (&card, &self->anchor_bounds, &origin);
  vibeos_modal_snapshot_push (snapshot, self->shown, &origin);
  gtk_widget_snapshot_child (widget, self->card, snapshot);
  vibeos_modal_snapshot_pop (snapshot);
}

static void
on_bin_anim (VibeosAnim *anim, double value, gpointer data)
{
  VibeosMenuBin *self = data;
  GtkWidget *gone, *parent;

  self->shown = value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  if (!self->closing || vibeos_anim_is_running (anim))
    return;
  gone = self->layer ? self->layer : GTK_WIDGET (self);
  parent = gtk_widget_get_parent (gone);
  if (GTK_IS_OVERLAY (parent))
    gtk_overlay_remove_overlay (GTK_OVERLAY (parent), gone);
}

static void
vibeos_menu_bin_map (GtkWidget *widget)
{
  VibeosMenuBin *self = VIBEOS_MENU_BIN (widget);

  GTK_WIDGET_CLASS (vibeos_menu_bin_parent_class)->map (widget);
  if (!self->closing)
    vibeos_anim_start_modal (self->anim, TRUE);
}

static void
vibeos_menu_bin_dispose (GObject *object)
{
  VibeosMenuBin *self = VIBEOS_MENU_BIN (object);

  g_clear_pointer (&self->anim, vibeos_anim_free);
  g_clear_pointer (&self->card, gtk_widget_unparent);
  g_clear_weak_pointer (&self->anchor);
  g_clear_weak_pointer (&self->layer);
  G_OBJECT_CLASS (vibeos_menu_bin_parent_class)->dispose (object);
}

static void
vibeos_menu_bin_class_init (VibeosMenuBinClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = vibeos_menu_bin_dispose;
  widget_class->measure = vibeos_menu_bin_measure;
  widget_class->size_allocate = vibeos_menu_bin_size_allocate;
  widget_class->snapshot = vibeos_menu_bin_snapshot;
  widget_class->map = vibeos_menu_bin_map;
}

static void
vibeos_menu_bin_init (VibeosMenuBin *self)
{
  self->anim = vibeos_anim_new (GTK_WIDGET (self), on_bin_anim, self);
}

GtkWidget *
vibeos_menu_bin_new (GtkWidget *card, GtkWidget *anchor)
{
  VibeosMenuBin *self;
  GtkWidget *bin;

  g_return_val_if_fail (GTK_IS_WIDGET (card), NULL);
  self = g_object_new (VIBEOS_TYPE_MENU_BIN, NULL);
  bin = GTK_WIDGET (self);
  /* the card's place is the bin's */
  gtk_widget_set_halign (bin, gtk_widget_get_halign (card));
  gtk_widget_set_valign (bin, gtk_widget_get_valign (card));
  gtk_widget_set_margin_top (bin, gtk_widget_get_margin_top (card));
  gtk_widget_set_margin_bottom (bin, gtk_widget_get_margin_bottom (card));
  gtk_widget_set_margin_start (bin, gtk_widget_get_margin_start (card));
  gtk_widget_set_margin_end (bin, gtk_widget_get_margin_end (card));
  gtk_widget_set_halign (card, GTK_ALIGN_FILL);
  gtk_widget_set_valign (card, GTK_ALIGN_FILL);
  gtk_widget_set_margin_top (card, 0);
  gtk_widget_set_margin_bottom (card, 0);
  gtk_widget_set_margin_start (card, 0);
  gtk_widget_set_margin_end (card, 0);
  self->card = card;
  gtk_widget_set_parent (card, bin);
  g_set_weak_pointer (&self->anchor, anchor);
  return bin;
}

void
vibeos_menu_bin_close (GtkWidget *bin, GtkWidget *layer)
{
  VibeosMenuBin *self;

  g_return_if_fail (VIBEOS_IS_MENU_BIN (bin));
  self = VIBEOS_MENU_BIN (bin);
  if (self->closing)
    return;
  self->closing = TRUE;
  g_set_weak_pointer (&self->layer, layer);
  gtk_widget_set_can_target (layer ? layer : bin, FALSE);
  vibeos_anim_start_modal (self->anim, FALSE);
}
