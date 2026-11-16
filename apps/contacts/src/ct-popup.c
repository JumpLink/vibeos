/* The popup menu (ct-popup.h). */
#include "ct-popup.h"
#include "ct-ui.h"

#include <vibeos.h>
#include <spongy-gtk.h>

#define EDGE 8.0                /* the menu keeps this far from the window's edges */

typedef struct {
  GtkWidget *layer;
  CtPopupChosen chosen;
  CtPopupToggled toggled;
  gpointer data;
  VibeosAnim *anim;
  gboolean closing;
} Popup;

static void
popup_free (gpointer data)
{
  Popup *popup = data;

  g_clear_pointer (&popup->anim, vibeos_anim_free);
  g_free (popup);
}

static void
on_anim (VibeosAnim *anim, double value, gpointer data)
{
  Popup *popup = data;
  GtkWidget *parent;

  gtk_widget_set_opacity (popup->layer, value);
  if (popup->closing && value <= 0 && (parent = gtk_widget_get_parent (popup->layer)))
    gtk_overlay_remove_overlay (GTK_OVERLAY (parent), popup->layer);
}

/* the way out: a 300 ms fade; the menu takes no more touches meanwhile */
static void
popup_close (Popup *popup)
{
  if (popup->closing)
    return;
  popup->closing = TRUE;
  gtk_widget_set_can_target (popup->layer, FALSE);
  vibeos_anim_start (popup->anim, gtk_widget_get_opacity (popup->layer), 0, CT_POPUP_EXIT_MS, 0, SPONGY_LINEAR);
}

static void
on_row (VibeosPressable *row, Popup *popup)
{
  int index = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "ct-index"));
  GtkWidget *check = g_object_get_data (G_OBJECT (row), "ct-check");

  if (popup->closing)
    return;
  if (check)
    {
      /* a check row toggles in place */
      gboolean checked = !ct_check_get_active (check);

      ct_check_set_active (check, checked);
      popup->toggled (index, checked, popup->data);
      return;
    }
  popup_close (popup);
  if (popup->chosen)
    popup->chosen (index, popup->data);
}

static void
on_scrim (GtkGestureClick *gesture, int n, double x, double y, Popup *popup)
{
  popup_close (popup);
}

static void
popup_show (GtkWidget *anchor, const char *const *labels, int n_labels, CtPopupPlace place, double x, double y,
            double width, const gboolean *checked, int n_checks, CtPopupToggled toggled, CtPopupChosen chosen,
            gpointer data)
{
  GtkOverlay *overlay = vibeos_window_get_overlay (anchor);
  GtkWidget *layer, *scrim, *card, *scroll, *list;
  GtkGesture *press;
  Popup *popup;
  graphene_point_t at;
  double window_width, window_height, height, left, top, widest = 0;

  if (!overlay)
    return;
  ct_popup_dismiss (anchor);
  if (!gtk_widget_compute_point (anchor, GTK_WIDGET (overlay), &GRAPHENE_POINT_INIT (0, 0), &at))
    return;
  window_width = gtk_widget_get_width (GTK_WIDGET (overlay));
  window_height = gtk_widget_get_height (GTK_WIDGET (overlay));

  popup = g_new0 (Popup, 1);
  popup->chosen = chosen;
  popup->toggled = toggled;
  popup->data = data;
  layer = gtk_overlay_new ();
  popup->layer = layer;
  popup->anim = vibeos_anim_new (layer, on_anim, popup);
  g_object_set_data_full (G_OBJECT (layer), "ct-popup", popup, popup_free);

  scrim = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  press = gtk_gesture_click_new ();
  g_signal_connect (press, "pressed", G_CALLBACK (on_scrim), popup);
  gtk_widget_add_controller (scrim, GTK_EVENT_CONTROLLER (press));
  gtk_overlay_set_child (GTK_OVERLAY (layer), scrim);

  list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  for (int i = 0; i < n_labels; i++)
    {
      GtkWidget *text = gtk_label_new (labels[i]);
      GtkWidget *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      GtkWidget *row = vibeos_pressable_new (line);
      g_autoptr(PangoLayout) layout = text_layout (anchor, CT_TEXT (CT_POPUP_TEXT_SP, CT_WEIGHT_REGULAR), labels[i], 0);

      widest = MAX (widest, text_width (layout));
      gtk_label_set_xalign (GTK_LABEL (text), 0);
      gtk_label_set_ellipsize (GTK_LABEL (text), PANGO_ELLIPSIZE_END);
      gtk_widget_set_hexpand (text, TRUE);
      gtk_widget_add_css_class (text, "ct-popup-text");
      gtk_box_append (GTK_BOX (line), text);
      gtk_widget_add_css_class (row, "ct-popup-item");
      if (place == CT_POPUP_SPINNER)
        gtk_widget_add_css_class (row, "spinner");
      if (checked)
        gtk_widget_add_css_class (row, "check");
      if (checked && i < n_checks)
        {
          GtkWidget *check = ct_check_new ();

          ct_check_set_active (check, checked[i]);
          gtk_box_append (GTK_BOX (line), check);
          g_object_set_data (G_OBJECT (row), "ct-check", check);
        }
      vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (row), VIBEOS_RIPPLE_BOUNDED, 0);
      g_object_set_data (G_OBJECT (row), "ct-index", GINT_TO_POINTER (i));
      g_signal_connect (row, "clicked", G_CALLBACK (on_row), popup);
      gtk_box_append (GTK_BOX (list), row);
    }
  scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), list);
  card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (card, "ct-popup");
  gtk_widget_set_overflow (card, GTK_OVERFLOW_HIDDEN);
  gtk_box_append (GTK_BOX (card), scroll);

  /* the widest row with its insets, at least the minimum, at most half the
   * screen or 320, whichever is more */
  if (place == CT_POPUP_SPINNER)
    width = MAX (widest + 2 * CT_POPUP_SPINNER_PAD_DP, gtk_widget_get_width (anchor));
  else if (width <= 0)
    width = MIN (MAX (widest + 2 * CT_POPUP_ITEM_PAD_DP, CT_POPUP_MIN_W_DP), MAX (window_width / 2, 320));
  width = MIN (width, window_width - 2 * EDGE);
  /* the card's padding above and below its rows */
  height = MIN (n_labels * CT_POPUP_ITEM_H_DP + 16, window_height - 2 * EDGE);
  switch (place)
    {
    case CT_POPUP_OVER_END:
      /* over the anchor, with a horizontal offset: the top on the anchor's top, the end a little in */
      left = at.x + gtk_widget_get_width (anchor) - width + CT_POPUP_H_OFFSET_DP;
      top = at.y;
      break;
    case CT_POPUP_BELOW:
      left = at.x;
      top = at.y + gtk_widget_get_height (anchor);
      /* no room below: above the anchor instead */
      if (top + height > window_height - EDGE)
        top = at.y - height;
      break;
    case CT_POPUP_SPINNER:
      left = at.x;
      top = at.y;
      break;
    case CT_POPUP_AT:
    default:
      left = at.x + x;
      top = at.y + y;
      break;
    }
  left = CLAMP (left, EDGE, window_width - EDGE - width);
  top = CLAMP (top, EDGE, window_height - EDGE - height);
  gtk_widget_set_size_request (card, (int) width, (int) height);
  gtk_widget_set_halign (card, GTK_ALIGN_START);
  gtk_widget_set_valign (card, GTK_ALIGN_START);
  gtk_widget_set_margin_top (card, (int) top);
  gtk_widget_set_margin_start (card, (int) left);
  gtk_overlay_add_overlay (GTK_OVERLAY (layer), card);
  gtk_overlay_add_overlay (overlay, layer);
  /* the way in: a fade from 0.1 over 100 ms */
  gtk_widget_set_opacity (layer, 0.1);
  vibeos_anim_start (popup->anim, 0.1, 1, CT_POPUP_ENTER_FADE_MS, 0, SPONGY_LINEAR);
}

void
ct_popup_show (GtkWidget *anchor, const char *const *labels, int n_labels, CtPopupPlace place, double x, double y,
               double width, CtPopupChosen chosen, gpointer data)
{
  popup_show (anchor, labels, n_labels, place, x, y, width, NULL, 0, NULL, chosen, data);
}

void
ct_popup_show_checks (GtkWidget *anchor, const char *const *labels, int n_labels, const gboolean *checked,
                      int n_checks, CtPopupToggled toggled, CtPopupChosen chosen, gpointer data)
{
  popup_show (anchor, labels, n_labels, CT_POPUP_BELOW, 0, 0, gtk_widget_get_width (anchor), checked, n_checks,
              toggled, chosen, data);
}

gboolean
ct_popup_dismiss (GtkWidget *widget)
{
  GtkOverlay *overlay = vibeos_window_get_overlay (widget);

  if (!overlay)
    return FALSE;
  for (GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (overlay)); child;
       child = gtk_widget_get_next_sibling (child))
    {
      Popup *popup = g_object_get_data (G_OBJECT (child), "ct-popup");

      if (popup && !popup->closing)
        {
          popup_close (popup);
          return TRUE;
        }
    }
  return FALSE;
}
