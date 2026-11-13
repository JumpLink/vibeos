/* The in-call grid's button: see incall-button.h. */
#include "incall-button.h"

#include <math.h>
#include <neo.h>

#define PADDING         4
#define ICON_VIEW       64      /* the round button */
#define ICON_PADDING    14      /* around the icon, which is scaled up to fill the rest */
#define LABEL_GAP       8
#define LABEL_SIZE      12      /* the label under the button */
#define LABEL_MAX_EMS   9
#define WIDTH           104     /* a column of the grid: three to a phone's width */
#define DISABLED_ALPHA  0.3

#define PAD_TOP(size)    ((size) * 263 / 2048.0)
#define PAD_BOTTOM(size) ((size) * 55 / 2048.0)

struct _InCallButton
{
  GtkWidget parent;
  char *icon_name;
  PangoLayout *label;
  gboolean checked, pressed;
  NeoRipple ripple;
};

G_DEFINE_FINAL_TYPE (InCallButton, incall_button, GTK_TYPE_WIDGET)

enum { CLICKED, N_SIGNALS };
static guint signals[N_SIGNALS];

static double
label_height (InCallButton *self)
{
  int width, height;

  pango_layout_get_size (self->label, &width, &height);
  return PAD_TOP (LABEL_SIZE) + height / (double) PANGO_SCALE + PAD_BOTTOM (LABEL_SIZE);
}

static void
incall_button_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                       int *minimum_baseline, int *natural_baseline)
{
  InCallButton *self = INCALL_BUTTON (widget);

  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    {
      *minimum = ICON_VIEW + 2 * PADDING;
      *natural = WIDTH;
    }
  else
    *minimum = *natural = (int) ceil (PADDING + ICON_VIEW + LABEL_GAP + label_height (self) + PADDING);
}

static void
incall_button_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  InCallButton *self = INCALL_BUTTON (widget);

  /* one line, at most 9 ems, ellipsized at the end */
  pango_layout_set_width (self->label, (int) (MIN (width - 2 * PADDING, LABEL_MAX_EMS * LABEL_SIZE) * PANGO_SCALE));
}

static void
incall_button_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  InCallButton *self = INCALL_BUTTON (widget);
  double width = gtk_widget_get_width (widget), cx = width / 2;
  graphene_rect_t box = GRAPHENE_RECT_INIT (cx - ICON_VIEW / 2.0, PADDING, ICON_VIEW, ICON_VIEW);
  GskRoundedRect oval;
  PangoRectangle logical;
  /* the icon: black on the light disc, white without it; the ripple faint grey on the disc, faint white off it */
  GdkRGBA icon = self->checked ? (GdkRGBA) { 0, 0, 0, 1 } : (GdkRGBA) { 1, 1, 1, 1 };
  GdkRGBA ripple = self->checked ? (GdkRGBA) { 0x88 / 255.0, 0x88 / 255.0, 0x88 / 255.0, 0.25 }
                                 : (GdkRGBA) { 1, 1, 1, 0.25 };

  if (!gtk_widget_is_sensitive (widget))
    gtk_snapshot_push_opacity (snapshot, DISABLED_ALPHA);
  gsk_rounded_rect_init_from_rect (&oval, &box, ICON_VIEW / 2.0);
  if (self->checked)
    {
      /* a checked button: a light disc, its shadow deeper while pressed; dimmed when disabled */
      GdkRGBA disc = { 0xed / 255.0, 0xf1 / 255.0, 0xf2 / 255.0, gtk_widget_is_sensitive (widget) ? 1 : 0.6 };

      gtk_snapshot_append_outset_shadow (snapshot, &oval, &(GdkRGBA) { 0, 0, 0, 0.2 }, 0, self->pressed ? 4 : 2, 0,
                                         self->pressed ? 8 : 4);
      gtk_snapshot_push_rounded_clip (snapshot, &oval);
      gtk_snapshot_append_color (snapshot, &disc, &box);
      gtk_snapshot_pop (snapshot);
    }
  if (neo_ripple_is_active (&self->ripple))
    {
      gtk_snapshot_push_rounded_clip (snapshot, &oval);
      neo_ripple_snapshot (&self->ripple, snapshot, &box, &ripple);
      gtk_snapshot_pop (snapshot);
    }
  neo_snapshot_icon (snapshot, widget, self->icon_name, cx - ICON_VIEW / 2.0 + ICON_PADDING,
                     PADDING + ICON_PADDING, ICON_VIEW - 2 * ICON_PADDING, &icon);

  /* the layout centres its line in its own width: its logical box says where the line starts */
  pango_layout_get_pixel_extents (self->label, NULL, &logical);
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (round (cx - logical.width / 2.0) - logical.x,
                                                          PADDING + ICON_VIEW + LABEL_GAP + PAD_TOP (LABEL_SIZE)));
  gtk_snapshot_append_layout (snapshot, self->label, &(GdkRGBA) { 1, 1, 1, 1 });
  gtk_snapshot_restore (snapshot);
  if (!gtk_widget_is_sensitive (widget))
    gtk_snapshot_pop (snapshot);
}

/* ---- touch ---------------------------------------------------------------------------------------------------- */

static void
release (InCallButton *self)
{
  if (!self->pressed)
    return;
  self->pressed = FALSE;
  neo_ripple_release (&self->ripple);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_pressed (GtkGestureClick *gesture, int n_press, double x, double y, InCallButton *self)
{
  double cx = gtk_widget_get_width (GTK_WIDGET (self)) / 2;

  self->pressed = TRUE;
  /* the icon view takes the parent's pressed state: its ripple starts where the finger is */
  neo_ripple_press (&self->ripple, x - (cx - ICON_VIEW / 2.0), y - PADDING);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_released (GtkGestureClick *gesture, int n_press, double x, double y, InCallButton *self)
{
  gboolean inside = x >= 0 && y >= 0 && x < gtk_widget_get_width (GTK_WIDGET (self))
                    && y < gtk_widget_get_height (GTK_WIDGET (self));

  release (self);
  if (inside)
    g_signal_emit (self, signals[CLICKED], 0);
}

static void
on_stopped (GtkGestureClick *gesture, InCallButton *self)
{
  release (self);
}

/* ---- GObject -------------------------------------------------------------------------------------------------- */

static void
incall_button_dispose (GObject *object)
{
  InCallButton *self = INCALL_BUTTON (object);

  neo_ripple_clear (&self->ripple);
  g_clear_object (&self->label);
  g_clear_pointer (&self->icon_name, g_free);
  G_OBJECT_CLASS (incall_button_parent_class)->dispose (object);
}

static void
incall_button_class_init (InCallButtonClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = incall_button_dispose;
  widget_class->measure = incall_button_measure;
  widget_class->size_allocate = incall_button_size_allocate;
  widget_class->snapshot = incall_button_snapshot;
  gtk_widget_class_set_css_name (widget_class, "incall-button");
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_TOGGLE_BUTTON);
  signals[CLICKED] = g_signal_new ("clicked", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                   G_TYPE_NONE, 0);
}

static void
incall_button_init (InCallButton *self)
{
  GtkGesture *click = gtk_gesture_click_new ();
  g_autoptr(PangoFontDescription) font = pango_font_description_from_string ("Neo Roboto");

  neo_ripple_init (&self->ripple, GTK_WIDGET (self));
  self->label = gtk_widget_create_pango_layout (GTK_WIDGET (self), NULL);
  /* 700, not 400: the phone's Bold text setting is on */
  pango_font_description_set_absolute_size (font, LABEL_SIZE * PANGO_SCALE);
  pango_font_description_set_weight (font, 700);
  pango_layout_set_font_description (self->label, font);
  pango_layout_set_ellipsize (self->label, PANGO_ELLIPSIZE_END);
  pango_layout_set_alignment (self->label, PANGO_ALIGN_CENTER);

  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), 0);
  g_signal_connect (click, "pressed", G_CALLBACK (on_pressed), self);
  g_signal_connect (click, "released", G_CALLBACK (on_released), self);
  g_signal_connect (click, "stopped", G_CALLBACK (on_stopped), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
}

GtkWidget *
incall_button_new (const char *icon_name, const char *label)
{
  InCallButton *self = g_object_new (INCALL_TYPE_BUTTON, NULL);

  incall_button_set_icon (self, icon_name);
  incall_button_set_label (self, label);
  return GTK_WIDGET (self);
}

void
incall_button_set_checked (InCallButton *self, gboolean checked)
{
  if (self->checked == checked)
    return;
  self->checked = checked;
  gtk_accessible_update_state (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_STATE_PRESSED,
                               checked ? GTK_ACCESSIBLE_TRISTATE_TRUE : GTK_ACCESSIBLE_TRISTATE_FALSE, -1);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

gboolean
incall_button_get_checked (InCallButton *self)
{
  return self->checked;
}

void
incall_button_set_icon (InCallButton *self, const char *icon_name)
{
  g_free (self->icon_name);
  self->icon_name = g_strdup (icon_name);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
incall_button_set_label (InCallButton *self, const char *label)
{
  pango_layout_set_text (self->label, label, -1);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  gtk_widget_queue_resize (GTK_WIDGET (self));
}
