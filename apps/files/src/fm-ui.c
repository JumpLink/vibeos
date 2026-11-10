/* Shared pieces of the Files screens (fm-ui.h). */
#include "fm-ui.h"

#include <math.h>
#include <neo.h>
#include <spongy-gtk.h>

GdkRGBA
fm_rgba (guint32 argb)
{
  return (GdkRGBA) { ((argb >> 16) & 0xff) / 255.0f, ((argb >> 8) & 0xff) / 255.0f, (argb & 0xff) / 255.0f,
                     ((argb >> 24) & 0xff) / 255.0f };
}

char *
fm_caps (const char *label)
{
  return g_utf8_strup (label, -1);
}

GtkWidget *
fm_label (const char *text, const char *css_class, double size, gboolean wrap)
{
  GtkWidget *label = gtk_label_new (text);

  if (css_class)
    gtk_widget_add_css_class (label, css_class);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  if (wrap)
    {
      gtk_label_set_wrap (GTK_LABEL (label), TRUE);
      gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
      /* a wrapping label asks for its natural width otherwise: the window would grow to the longest name */
      gtk_label_set_width_chars (GTK_LABEL (label), 1);
    }
  else
    {
      gtk_label_set_single_line_mode (GTK_LABEL (label), TRUE);
      gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
    }
  neo_label_set_font_padding (label, size);
  return label;
}

GtkWidget *
fm_icon_button_sized (const char *icon_name, const char *name, int width, int height, int glyph_size)
{
  GtkWidget *image = gtk_image_new_from_icon_name (icon_name);
  GtkWidget *button = neo_pressable_new (image);
  GdkRGBA ripple = fm_rgba (FM_C_RIPPLE);

  gtk_image_set_pixel_size (GTK_IMAGE (image), glyph_size);
  gtk_widget_set_size_request (button, width, height);
  gtk_widget_add_css_class (button, "fm-icon-button");
  /* ?selectableItemBackgroundBorderless: a circle of the button's half diagonal, at most 20 dp past the glyph */
  neo_pressable_set_ripple (NEO_PRESSABLE (button), NEO_RIPPLE_CIRCLE, MIN (hypot (width, height) / 2, 24));
  neo_pressable_set_ripple_color (NEO_PRESSABLE (button), &ripple);
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
  return button;
}

GtkWidget *
fm_icon_button (const char *icon_name, const char *name, int width, int height)
{
  return fm_icon_button_sized (icon_name, name, width, height, 20);
}

GtkWidget *
fm_text_button (const char *label)
{
  g_autofree char *caps = fm_caps (label);
  GtkWidget *text = gtk_label_new (caps);
  GtkWidget *button;
  GdkRGBA ripple = fm_rgba (FM_C_RIPPLE_CARD);

  neo_label_set_font_padding (text, 14);
  gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
  gtk_widget_set_halign (text, GTK_ALIGN_CENTER);
  button = neo_pressable_new (text);
  gtk_widget_add_css_class (button, "fm-text-button");
  neo_pressable_set_ripple (NEO_PRESSABLE (button), NEO_RIPPLE_BOUNDED, 2);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (button), &ripple);
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  return button;
}

/* ---- the check box and the radio button ------------------------------------------------------------------------ */

#define FM_TYPE_CHECK (fm_check_get_type ())
G_DECLARE_FINAL_TYPE (FmCheck, fm_check, FM, CHECK, GtkWidget)

struct _FmCheck {
  GtkWidget parent;
  gboolean active, radio;
};

G_DEFINE_FINAL_TYPE (FmCheck, fm_check, GTK_TYPE_WIDGET)

enum { PROP_0, PROP_ACTIVE };

static void
fm_check_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                  int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 32;
}

static void
fm_check_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  FmCheck *self = FM_CHECK (widget);
  GdkRGBA on = fm_rgba (FM_C_ACCENT), off = fm_rgba (FM_C_CHECK_OFF);
  GdkRGBA color = self->active ? on : off;
  float x = (gtk_widget_get_width (widget) - 32) / 2.0f, y = (gtk_widget_get_height (widget) - 32) / 2.0f;
  g_autoptr(GskPathBuilder) builder = gsk_path_builder_new ();
  g_autoptr(GskPath) path = NULL;
  g_autoptr(GskStroke) stroke = gsk_stroke_new (2);

  if (self->radio)
    {
      /* abc_btn_radio_material: a 2 dp ring of 20 dp (r 9 to the middle of the line), a 10 dp dot when on */
      gsk_path_builder_add_circle (builder, &GRAPHENE_POINT_INIT (x + 16, y + 16), 9);
      path = gsk_path_builder_free_to_path (g_steal_pointer (&builder));
      gtk_snapshot_append_stroke (snapshot, path, stroke, &color);
      if (self->active)
        {
          g_autoptr(GskPathBuilder) dot = gsk_path_builder_new ();
          g_autoptr(GskPath) dot_path = NULL;

          gsk_path_builder_add_circle (dot, &GRAPHENE_POINT_INIT (x + 16, y + 16), 5);
          dot_path = gsk_path_builder_free_to_path (g_steal_pointer (&dot));
          gtk_snapshot_append_fill (snapshot, dot_path, GSK_FILL_RULE_WINDING, &color);
        }
      return;
    }
  /* abc_btn_check_material: an 18 dp square with 2 dp corners from (7, 7); outlined off, filled on with the mark
   * cut out (drawn in the card's colour) */
  if (self->active)
    {
      GskRoundedRect box;
      GdkRGBA mark = fm_rgba (FM_C_WINDOW);
      g_autoptr(GskPathBuilder) tick = gsk_path_builder_new ();
      g_autoptr(GskPath) tick_path = NULL;

      gsk_rounded_rect_init_from_rect (&box, &GRAPHENE_RECT_INIT (x + 7, y + 7, 18, 18), 2);
      gtk_snapshot_push_rounded_clip (snapshot, &box);
      gtk_snapshot_append_color (snapshot, &color, &box.bounds);
      gtk_snapshot_pop (snapshot);
      gsk_path_builder_move_to (tick, x + 10.5f, y + 16.2f);
      gsk_path_builder_line_to (tick, x + 14.2f, y + 19.8f);
      gsk_path_builder_line_to (tick, x + 21.5f, y + 12.4f);
      tick_path = gsk_path_builder_free_to_path (g_steal_pointer (&tick));
      gtk_snapshot_append_stroke (snapshot, tick_path, stroke, &mark);
      return;
    }
  gsk_path_builder_add_rounded_rect (builder, &(GskRoundedRect) {
                                       .bounds = GRAPHENE_RECT_INIT (x + 8, y + 8, 16, 16),
                                       .corner = { { 1, 1 }, { 1, 1 }, { 1, 1 }, { 1, 1 } } });
  path = gsk_path_builder_free_to_path (g_steal_pointer (&builder));
  gtk_snapshot_append_stroke (snapshot, path, stroke, &color);
}

static void
fm_check_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  g_value_set_boolean (value, FM_CHECK (object)->active);
}

static void
fm_check_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
  fm_check_set_active (GTK_WIDGET (object), g_value_get_boolean (value));
}

static void
fm_check_class_init (FmCheckClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = fm_check_get_property;
  object_class->set_property = fm_check_set_property;
  GTK_WIDGET_CLASS (klass)->measure = fm_check_measure;
  GTK_WIDGET_CLASS (klass)->snapshot = fm_check_snapshot;
  gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "fm-check");
  g_object_class_install_property (object_class, PROP_ACTIVE,
                                   g_param_spec_boolean ("active", NULL, NULL, FALSE,
                                                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY));
}

static void
fm_check_init (FmCheck *self)
{
  gtk_widget_set_can_target (GTK_WIDGET (self), FALSE);
}

GtkWidget *
fm_check_new (gboolean radio)
{
  FmCheck *self = g_object_new (FM_TYPE_CHECK, NULL);

  self->radio = radio;
  return GTK_WIDGET (self);
}

void
fm_check_set_active (GtkWidget *check, gboolean active)
{
  FmCheck *self = FM_CHECK (check);

  if (self->active == !!active)
    return;
  self->active = !!active;
  gtk_widget_queue_draw (check);
  g_object_notify (G_OBJECT (check), "active");
}

gboolean
fm_check_get_active (GtkWidget *check)
{
  return FM_CHECK (check)->active;
}

static void
on_check_row (NeoPressable *row, GtkWidget *check)
{
  /* a radio button is set by a tap, never cleared: its group clears the others */
  if (FM_CHECK (check)->radio)
    fm_check_set_active (check, TRUE);
  else
    fm_check_set_active (check, !fm_check_get_active (check));
}

GtkWidget *
fm_check_row (const char *label, gboolean radio, gboolean active)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *check = fm_check_new (radio);
  GtkWidget *text = fm_label (label, "fm-check-text", 16, TRUE);
  GtkWidget *row = neo_pressable_new (box);
  GdkRGBA ripple = fm_rgba (FM_C_RIPPLE_CARD);

  fm_check_set_active (check, active);
  gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (text, TRUE);
  gtk_box_append (GTK_BOX (box), check);
  gtk_box_append (GTK_BOX (box), text);
  gtk_widget_add_css_class (row, "fm-check-row");
  neo_pressable_set_ripple (NEO_PRESSABLE (row), NEO_RIPPLE_BOUNDED, 0);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (row), &ripple);
  g_object_set_data (G_OBJECT (row), "fm-check", check);
  g_signal_connect (row, "clicked", G_CALLBACK (on_check_row), check);
  gtk_accessible_update_property (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  return row;
}

GtkWidget *
fm_check_row_get_check (GtkWidget *row)
{
  return g_object_get_data (G_OBJECT (row), "fm-check");
}

/* ---- the thin progress bar ------------------------------------------------------------------------------------- */

#define FM_TYPE_PROGRESS (fm_progress_get_type ())
G_DECLARE_FINAL_TYPE (FmProgress, fm_progress, FM, PROGRESS, GtkWidget)

struct _FmProgress {
  GtkWidget parent;
  double height, fraction;
  guint tick;
  gint64 start;
};

G_DEFINE_FINAL_TYPE (FmProgress, fm_progress, GTK_TYPE_WIDGET)

static void
fm_progress_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                     int *minimum_baseline, int *natural_baseline)
{
  FmProgress *self = FM_PROGRESS (widget);

  *minimum = *natural = orientation == GTK_ORIENTATION_VERTICAL ? (int) ceil (self->height) : 0;
}

static void
segment (GtkSnapshot *snapshot, double width, double height, double from, double to, const GdkRGBA *color)
{
  from = CLAMP (from, 0, 1);
  to = CLAMP (to, 0, 1);
  if (to > from)
    gtk_snapshot_append_color (snapshot, color,
                               &GRAPHENE_RECT_INIT ((float) (from * width), 0, (float) ((to - from) * width),
                                                    (float) height));
}

static void
fm_progress_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  FmProgress *self = FM_PROGRESS (widget);
  double width = gtk_widget_get_width (widget);
  GdkRGBA accent = fm_rgba (FM_C_ACCENT);
  GdkRGBA track = accent;

  track.alpha = 0.3f;
  if (self->fraction >= 0)
    {
      segment (snapshot, width, self->height, 0, 1, &track);
      segment (snapshot, width, self->height, 0, self->fraction, &accent);
      return;
    }
  /* Material's indeterminate line: two segments chase across in 2 s, the first long, the second short */
  {
    double t = fmod ((g_get_monotonic_time () - self->start) / 2000000.0, 1.0);
    double head1 = spongy_curve (SPONGY_FAST_OUT_SLOW_IN, CLAMP (t / 0.75, 0, 1)) * 1.4;
    double tail1 = spongy_curve (SPONGY_FAST_OUT_SLOW_IN, CLAMP ((t - 0.25) / 0.75, 0, 1)) * 1.4 - 0.1;
    double head2 = spongy_curve (SPONGY_FAST_OUT_SLOW_IN, CLAMP ((t - 0.5) / 0.5, 0, 1)) * 1.2;
    double tail2 = spongy_curve (SPONGY_FAST_OUT_SLOW_IN, CLAMP ((t - 0.6) / 0.4, 0, 1)) * 1.2 - 0.05;

    segment (snapshot, width, self->height, tail1, head1, &accent);
    segment (snapshot, width, self->height, tail2, head2, &accent);
  }
}

static gboolean
on_progress_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  gtk_widget_queue_draw (widget);
  return G_SOURCE_CONTINUE;
}

static void
fm_progress_class_init (FmProgressClass *klass)
{
  GTK_WIDGET_CLASS (klass)->measure = fm_progress_measure;
  GTK_WIDGET_CLASS (klass)->snapshot = fm_progress_snapshot;
  gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "fm-progress");
}

static void
fm_progress_init (FmProgress *self)
{
  gtk_widget_set_can_target (GTK_WIDGET (self), FALSE);
}

GtkWidget *
fm_progress_new (double height)
{
  FmProgress *self = g_object_new (FM_TYPE_PROGRESS, NULL);

  self->height = height;
  fm_progress_set_fraction (GTK_WIDGET (self), 0);
  return GTK_WIDGET (self);
}

void
fm_progress_set_fraction (GtkWidget *progress, double fraction)
{
  FmProgress *self = FM_PROGRESS (progress);

  self->fraction = fraction < 0 ? -1 : CLAMP (fraction, 0, 1);
  if (fraction < 0 && !self->tick)
    {
      self->start = g_get_monotonic_time ();
      self->tick = gtk_widget_add_tick_callback (progress, on_progress_tick, NULL, NULL);
    }
  else if (fraction >= 0 && self->tick)
    {
      gtk_widget_remove_tick_callback (progress, self->tick);
      self->tick = 0;
    }
  gtk_widget_queue_draw (progress);
}

/* ---- the popup menu -------------------------------------------------------------------------------------------- */

#define POPUP_EDGE   8.0
#define POPUP_ROW_H  48
#define POPUP_PAD    16
#define POPUP_MIN_W  196

typedef struct {
  GtkWidget *layer;
  FmPopupChosen chosen;
  gpointer data;
  NeoAnim *anim;
  gboolean closing;
} Popup;

static void
popup_free (gpointer data)
{
  Popup *popup = data;

  g_clear_pointer (&popup->anim, neo_anim_free);
  g_free (popup);
}

static void
on_popup_anim (NeoAnim *anim, double value, gpointer data)
{
  Popup *popup = data;
  GtkWidget *parent;

  gtk_widget_set_opacity (popup->layer, value);
  if (popup->closing && value <= 0 && (parent = gtk_widget_get_parent (popup->layer)))
    gtk_overlay_remove_overlay (GTK_OVERLAY (parent), popup->layer);
}

static void
popup_close (Popup *popup)
{
  if (popup->closing)
    return;
  popup->closing = TRUE;
  gtk_widget_set_can_target (popup->layer, FALSE);
  /* popup_exit: a fade */
  neo_anim_start (popup->anim, gtk_widget_get_opacity (popup->layer), 0, 150, 0, SPONGY_LINEAR);
}

static void
on_popup_row (NeoPressable *row, Popup *popup)
{
  if (popup->closing)
    return;
  popup_close (popup);
  popup->chosen (GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "fm-index")), popup->data);
}

static void
on_popup_scrim (GtkGestureClick *gesture, int n, double x, double y, Popup *popup)
{
  popup_close (popup);
}

static gboolean
on_popup_key (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, Popup *popup)
{
  if (keyval != GDK_KEY_Escape && keyval != GDK_KEY_Back)
    return FALSE;
  popup_close (popup);
  return TRUE;
}

void
fm_popup_show (GtkWidget *anchor, const char *const *labels, int n_labels, FmPopupPlace place, double x, double y,
               FmPopupChosen chosen, gpointer data)
{
  GtkOverlay *overlay = neo_window_get_overlay (anchor);
  GtkWidget *layer, *scrim, *card, *scroll, *list;
  GtkGesture *press;
  GtkEventController *keys;
  Popup *popup;
  graphene_point_t at;
  double window_width, window_height, width, height, left, top, widest = 0;
  GdkRGBA ripple = fm_rgba (FM_C_RIPPLE_CARD);

  if (!overlay || n_labels <= 0)
    return;
  fm_popup_dismiss (anchor);
  if (!gtk_widget_compute_point (anchor, GTK_WIDGET (overlay), &GRAPHENE_POINT_INIT (0, 0), &at))
    return;
  window_width = gtk_widget_get_width (GTK_WIDGET (overlay));
  window_height = gtk_widget_get_height (GTK_WIDGET (overlay));

  popup = g_new0 (Popup, 1);
  popup->chosen = chosen;
  popup->data = data;
  layer = gtk_overlay_new ();
  popup->layer = layer;
  popup->anim = neo_anim_new (layer, on_popup_anim, popup);
  g_object_set_data_full (G_OBJECT (layer), "fm-popup", popup, popup_free);

  scrim = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  press = gtk_gesture_click_new ();
  g_signal_connect (press, "pressed", G_CALLBACK (on_popup_scrim), popup);
  gtk_widget_add_controller (scrim, GTK_EVENT_CONTROLLER (press));
  gtk_overlay_set_child (GTK_OVERLAY (layer), scrim);
  keys = gtk_event_controller_key_new ();
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_popup_key), popup);
  gtk_widget_add_controller (layer, keys);
  gtk_widget_set_focusable (layer, TRUE);
  /* a dialog to the shell's back gesture, which then sends Escape */
  gtk_accessible_update_property (GTK_ACCESSIBLE (layer), GTK_ACCESSIBLE_PROPERTY_LABEL, "Menu", -1);

  list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  for (int i = 0; i < n_labels; i++)
    {
      GtkWidget *text = fm_label (labels[i], "fm-popup-text", 16, FALSE);
      GtkWidget *row = neo_pressable_new (text);
      PangoLayout *layout = gtk_widget_create_pango_layout (text, labels[i]);
      int w;

      pango_layout_get_pixel_size (layout, &w, NULL);
      g_object_unref (layout);
      /* measured in the label's own font only once it is styled: the CSS sets 16 sp, the default is 14 */
      widest = MAX (widest, w * 16.0 / 14.0);
      gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class (row, "fm-popup-item");
      neo_pressable_set_ripple (NEO_PRESSABLE (row), NEO_RIPPLE_BOUNDED, 0);
      neo_pressable_set_ripple_color (NEO_PRESSABLE (row), &ripple);
      g_object_set_data (G_OBJECT (row), "fm-index", GINT_TO_POINTER (i));
      g_signal_connect (row, "clicked", G_CALLBACK (on_popup_row), popup);
      gtk_box_append (GTK_BOX (list), row);
    }
  scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), list);
  card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (card, "fm-popup");
  gtk_widget_set_overflow (card, GTK_OVERFLOW_HIDDEN);
  gtk_box_append (GTK_BOX (card), scroll);

  width = MIN (MAX (widest + 2 * POPUP_PAD, POPUP_MIN_W), window_width - 2 * POPUP_EDGE);
  height = MIN (n_labels * POPUP_ROW_H + 16, window_height - 2 * POPUP_EDGE);
  if (place == FM_POPUP_OVER_END)
    {
      left = at.x + gtk_widget_get_width (anchor) - width - 4;
      top = at.y + 4;
    }
  else
    {
      left = at.x + x;
      top = at.y + y;
    }
  left = CLAMP (left, POPUP_EDGE, window_width - POPUP_EDGE - width);
  top = CLAMP (top, POPUP_EDGE, window_height - POPUP_EDGE - height);
  gtk_widget_set_size_request (card, (int) width, (int) height);
  gtk_widget_set_halign (card, GTK_ALIGN_START);
  gtk_widget_set_valign (card, GTK_ALIGN_START);
  gtk_widget_set_margin_top (card, (int) top);
  gtk_widget_set_margin_start (card, (int) left);
  gtk_overlay_add_overlay (GTK_OVERLAY (layer), card);
  gtk_overlay_add_overlay (overlay, layer);
  gtk_overlay_set_measure_overlay (overlay, layer, FALSE);
  gtk_widget_grab_focus (layer);
  gtk_widget_set_opacity (layer, 0.1);
  neo_anim_start (popup->anim, 0.1, 1, 100, 0, SPONGY_LINEAR);
}

gboolean
fm_popup_dismiss (GtkWidget *widget)
{
  GtkOverlay *overlay = neo_window_get_overlay (widget);

  if (!overlay)
    return FALSE;
  for (GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (overlay)); child;
       child = gtk_widget_get_next_sibling (child))
    {
      Popup *popup = g_object_get_data (G_OBJECT (child), "fm-popup");

      if (popup && !popup->closing)
        {
          popup_close (popup);
          return TRUE;
        }
    }
  return FALSE;
}
