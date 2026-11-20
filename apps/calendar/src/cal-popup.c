/* The popup list and the spinner (cal-popup.h). */
#include "cal-popup.h"

#include <vibeos.h>
#include <spongy-gtk.h>

#define ROW_HEIGHT 48.0
#define EDGE 8.0                        /* the popup keeps this far from the window's edges */

typedef struct {
  GtkWidget *layer;
  CalPopupChosen chosen;
  gpointer data;
} Popup;

static void
popup_close (Popup *popup)
{
  GtkWidget *parent = gtk_widget_get_parent (popup->layer);

  if (parent)
    gtk_overlay_remove_overlay (GTK_OVERLAY (parent), popup->layer);
}

static void
on_row (VibeosPressable *row, Popup *popup)
{
  int index = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "cal-index"));
  CalPopupChosen chosen = popup->chosen;
  gpointer data = popup->data;

  /* the popup goes first: the callback may open another */
  popup_close (popup);
  chosen (index, data);
}

static void
on_scrim (GtkGestureClick *gesture, int n, double x, double y, Popup *popup)
{
  popup_close (popup);
}

void
cal_popup_show (GtkWidget *anchor, const char *const *labels, int n_labels, int selected, double width,
                gboolean end, CalPopupChosen chosen, gpointer data)
{
  GtkOverlay *overlay = vibeos_window_get_overlay (anchor);
  GtkWidget *layer, *scrim, *card, *scroll, *list;
  GtkGesture *press;
  Popup *popup;
  graphene_point_t at;
  double window_height, window_width, height, top;

  if (!overlay)
    return;
  cal_popup_dismiss (anchor);
  if (!gtk_widget_compute_point (anchor, GTK_WIDGET (overlay), &GRAPHENE_POINT_INIT (0, 0), &at))
    return;
  window_width = gtk_widget_get_width (GTK_WIDGET (overlay));
  window_height = gtk_widget_get_height (GTK_WIDGET (overlay));
  if (width <= 0)
    width = gtk_widget_get_width (anchor);

  popup = g_new0 (Popup, 1);
  popup->chosen = chosen;
  popup->data = data;
  layer = gtk_overlay_new ();
  popup->layer = layer;
  g_object_set_data_full (G_OBJECT (layer), "cal-popup", popup, g_free);
  gtk_widget_add_css_class (layer, "cal-popup-layer");

  scrim = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  press = gtk_gesture_click_new ();
  g_signal_connect (press, "pressed", G_CALLBACK (on_scrim), popup);
  gtk_widget_add_controller (scrim, GTK_EVENT_CONTROLLER (press));
  gtk_overlay_set_child (GTK_OVERLAY (layer), scrim);

  list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  for (int i = 0; i < n_labels; i++)
    {
      GtkWidget *text = gtk_label_new (labels[i]);
      GtkWidget *row = vibeos_pressable_new (text);

      gtk_label_set_xalign (GTK_LABEL (text), 0);
      gtk_label_set_ellipsize (GTK_LABEL (text), PANGO_ELLIPSIZE_END);
      vibeos_label_set_font_padding_sp (text, 16);
      gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class (row, "cal-menu-item");
      if (i == selected)
        gtk_widget_add_css_class (row, "selected");
      vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (row), VIBEOS_RIPPLE_BOUNDED, 0);
      g_object_set_data (G_OBJECT (row), "cal-index", GINT_TO_POINTER (i));
      g_signal_connect (row, "clicked", G_CALLBACK (on_row), popup);
      gtk_box_append (GTK_BOX (list), row);
    }
  scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), list);
  card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (card, "cal-menu");
  /* a popup menu at the bar's end (no selection) is an overflow menu: its items' letter spacing (calendar.css) */
  if (end && selected < 0)
    gtk_widget_add_css_class (card, "cal-overflow-menu-items");
  gtk_widget_set_overflow (card, GTK_OVERFLOW_HIDDEN);
  gtk_box_append (GTK_BOX (card), scroll);

  /* as tall as the rows (and the card's padding), within the window; over the anchor, moved
   * up if it would run off the bottom */
  height = MIN (n_labels * ROW_HEIGHT + 16, window_height - 2 * EDGE);
  top = CLAMP (at.y, EDGE, window_height - EDGE - height);
  gtk_widget_set_size_request (card, (int) width, (int) height);
  gtk_widget_set_halign (card, GTK_ALIGN_START);
  gtk_widget_set_valign (card, GTK_ALIGN_START);
  gtk_widget_set_margin_top (card, (int) top);
  gtk_widget_set_margin_start (card, (int) CLAMP (end ? at.x + gtk_widget_get_width (anchor) - width : at.x, 0,
                                                  window_width - width));
  gtk_overlay_add_overlay (GTK_OVERLAY (layer), card);
  gtk_overlay_add_overlay (overlay, layer);
  /* the selected row in view */
  if (selected > 0)
    spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (scroll), MAX (0, (selected - 2) * ROW_HEIGHT));
}

gboolean
cal_popup_dismiss (GtkWidget *widget)
{
  GtkOverlay *overlay = vibeos_window_get_overlay (widget);

  if (!overlay)
    return FALSE;
  for (GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (overlay)); child;
       child = gtk_widget_get_next_sibling (child))
    {
      Popup *popup = g_object_get_data (G_OBJECT (child), "cal-popup");

      if (popup)
        {
          popup_close (popup);
          return TRUE;
        }
    }
  return FALSE;
}

/* --- CalSpinner */

struct _CalSpinner {
  GtkWidget parent;
  GtkWidget *pressable, *label;
  GStrv labels;
  int selected;
};

G_DEFINE_FINAL_TYPE (CalSpinner, cal_spinner, GTK_TYPE_WIDGET)

enum { CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
on_chosen (int index, gpointer data)
{
  CalSpinner *self = data;

  if (index == self->selected)
    return;
  cal_spinner_set_selected (self, index);
  g_signal_emit (self, signals[CHANGED], 0);
}

static void
on_clicked (VibeosPressable *pressable, CalSpinner *self)
{
  if (!self->labels)
    return;
  cal_popup_show (GTK_WIDGET (self), (const char *const *) self->labels, (int) g_strv_length (self->labels),
                  self->selected, 0, FALSE, on_chosen, self);
}

static void
cal_spinner_dispose (GObject *object)
{
  CalSpinner *self = CAL_SPINNER (object);

  g_clear_pointer (&self->pressable, gtk_widget_unparent);
  g_clear_pointer (&self->labels, g_strfreev);
  G_OBJECT_CLASS (cal_spinner_parent_class)->dispose (object);
}

static void
cal_spinner_class_init (CalSpinnerClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = cal_spinner_dispose;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "cal-spinner");
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_COMBO_BOX);
  signals[CHANGED] = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                   G_TYPE_NONE, 0);
}

static void
cal_spinner_init (CalSpinner *self)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *arrow = gtk_image_new_from_icon_name ("cal-drop-down-symbolic");

  self->selected = -1;
  self->label = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (self->label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (self->label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (self->label, TRUE);
  gtk_widget_add_css_class (self->label, "cal-spinner-text");
  gtk_image_set_pixel_size (GTK_IMAGE (arrow), 24);
  gtk_widget_add_css_class (arrow, "cal-spinner-arrow");
  gtk_box_append (GTK_BOX (box), self->label);
  gtk_box_append (GTK_BOX (box), arrow);
  self->pressable = vibeos_pressable_new (box);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (self->pressable), VIBEOS_RIPPLE_BOUNDED, 4);
  g_signal_connect (self->pressable, "clicked", G_CALLBACK (on_clicked), self);
  gtk_widget_set_parent (self->pressable, GTK_WIDGET (self));
}

GtkWidget *
cal_spinner_new (void)
{
  return g_object_new (CAL_TYPE_SPINNER, NULL);
}

void
cal_spinner_set_items (CalSpinner *self, const char *const *labels, int n_labels)
{
  GStrvBuilder *builder = g_strv_builder_new ();

  for (int i = 0; i < n_labels; i++)
    g_strv_builder_add (builder, labels[i]);
  g_clear_pointer (&self->labels, g_strfreev);
  self->labels = g_strv_builder_end (builder);
  g_strv_builder_unref (builder);
  cal_spinner_set_selected (self, MIN (MAX (self->selected, 0), n_labels - 1));
}

void
cal_spinner_set_selected (CalSpinner *self, int index)
{
  self->selected = index;
  gtk_label_set_label (GTK_LABEL (self->label),
                       self->labels && index >= 0 && index < (int) g_strv_length (self->labels)
                       ? self->labels[index] : "");
  gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  gtk_label_get_label (GTK_LABEL (self->label)), -1);
}

int
cal_spinner_get_selected (CalSpinner *self)
{
  return self->selected;
}
