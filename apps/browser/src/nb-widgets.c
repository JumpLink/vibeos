/* nb-widgets: see nb-widgets.h. Numbers are dp (1 GTK px on the phone) and come from spec/jelly.md. */
#include "nb-widgets.h"

#include <math.h>
#include <neo.h>
#include <spongy-gtk.h>

/* ==== NbDialog ===================================================================================================
 *
 * still:dlg-engine: the window is 95 % of the screen's width ([27..1053] of 1080, AppCompat's
 * windowMinWidthMinor), the card inside it 16 dp in ([75,192]..[1005,2203]: MaterialAlertDialog's background
 * insets), colorSurface with 16 dp corners (Theme.Jelly.AlertDialog), the dim 32 % black (#0a0f10 → #070a0b).
 * The motion is the platform dialog's, as NeoDialog's: alpha 0 → 1 rising 20 dp in 220 ms, out 150 ms. */

#define DLG_WIDTH_FRACTION 0.95
#define DLG_INSET     16
#define DLG_MAX_WIDTH 560
#define DLG_V_INSET   24
#define DIM           0.32
#define ENTER_MS      220
#define ENTER_RISE    20
#define EXIT_MS       150
#define EXIT_RISE     10

struct _NbDialog {
  GtkWidget  parent;
  GtkWidget *card, *title, *message, *content, *buttons, *start, *end;
  double     shown, rise;
  NeoAnim   *anim;
  gboolean   closing;
  GArray    *sticky;
};

G_DEFINE_FINAL_TYPE (NbDialog, nb_dialog, GTK_TYPE_WIDGET)

enum { DLG_RESPONSE, DLG_N_SIGNALS };
static guint dialog_signals[DLG_N_SIGNALS];

static void
nb_dialog_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                   int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 0;
}

static void
nb_dialog_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  NbDialog *self = NB_DIALOG (widget);
  int w = MIN ((int) round (DLG_WIDTH_FRACTION * width) - 2 * DLG_INSET, DLG_MAX_WIDTH);
  int min_w, min_h, h;

  gtk_widget_measure (self->card, GTK_ORIENTATION_HORIZONTAL, -1, &min_w, NULL, NULL, NULL);
  w = MAX (w, MIN (min_w, width));
  gtk_widget_measure (self->card, GTK_ORIENTATION_VERTICAL, w, &min_h, &h, NULL, NULL);
  h = MAX (MIN (h, height - 2 * DLG_V_INSET), MIN (min_h, height));
  gtk_widget_allocate (self->card, w, h, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT ((width - w) / 2.0, (height - h) / 2.0)));
}

static void
nb_dialog_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  NbDialog *self = NB_DIALOG (widget);
  GdkRGBA scrim = { 0, 0, 0, DIM };

  gtk_snapshot_push_opacity (snapshot, self->shown);
  gtk_snapshot_append_color (snapshot, &scrim,
                             &GRAPHENE_RECT_INIT (0, 0, gtk_widget_get_width (widget), gtk_widget_get_height (widget)));
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (0, self->rise));
  gtk_widget_snapshot_child (widget, self->card, snapshot);
  gtk_snapshot_restore (snapshot);
  gtk_snapshot_pop (snapshot);
}

static void
on_dialog_anim (NeoAnim *anim, double value, gpointer data)
{
  NbDialog *self = data;

  self->shown = value;
  self->rise = self->closing ? -EXIT_RISE * (1 - value) : ENTER_RISE * (1 - value);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  if (self->closing && !neo_anim_is_running (anim)) {
    GtkWidget *overlay = gtk_widget_get_parent (GTK_WIDGET (self));
    if (overlay)
      gtk_overlay_remove_overlay (GTK_OVERLAY (overlay), GTK_WIDGET (self));
  }
}

void
nb_dialog_close (NbDialog *self)
{
  if (self->closing)
    return;
  self->closing = TRUE;
  gtk_widget_set_can_target (GTK_WIDGET (self), FALSE);
  neo_anim_start_cubic (self->anim, 1, 0, EXIT_MS, 0, 0.3f, 0, 1, 1);
}

static gboolean
is_sticky (NbDialog *self, int response)
{
  for (guint i = 0; i < self->sticky->len; i++)
    if (g_array_index (self->sticky, int, i) == response)
      return TRUE;
  return FALSE;
}

void
nb_dialog_respond (NbDialog *self, int response)
{
  if (self->closing)
    return;
  g_object_ref (self);
  gboolean stays = is_sticky (self, response);
  g_signal_emit (self, dialog_signals[DLG_RESPONSE], 0, response);
  if (!stays)
    nb_dialog_close (self);
  g_object_unref (self);
}

void
nb_dialog_set_sticky (NbDialog *self, int response)
{
  g_array_append_val (self->sticky, response);
}

void
nb_dialog_present (NbDialog *self, GtkWidget *parent)
{
  GtkOverlay *overlay = neo_window_get_overlay (parent);

  g_return_if_fail (overlay != NULL);
  gtk_overlay_add_overlay (overlay, GTK_WIDGET (self));
  gtk_overlay_set_measure_overlay (overlay, GTK_WIDGET (self), FALSE);
  gtk_overlay_set_clip_overlay (overlay, GTK_WIDGET (self), TRUE);
  neo_anim_start_cubic (self->anim, 0, 1, ENTER_MS, 0, 0.2f, 0, 0, 1);
  /* the dialog itself, for Escape: a field waits for a tap before the keyboard comes up, as an AlertDialog's
   * EditText does (SOFT_INPUT_STATE_UNSPECIFIED) */
  gtk_widget_grab_focus (GTK_WIDGET (self));
}

static void
on_dialog_pressed (GtkGestureClick *click, int n_press, double x, double y, NbDialog *self)
{
  graphene_rect_t card;

  if (gtk_widget_compute_bounds (self->card, GTK_WIDGET (self), &card) &&
      graphene_rect_contains_point (&card, &GRAPHENE_POINT_INIT (x, y)))
    return;
  gtk_gesture_set_state (GTK_GESTURE (click), GTK_EVENT_SEQUENCE_CLAIMED);
  nb_dialog_respond (self, NB_RESPONSE_CANCEL);
}

static gboolean
on_dialog_key (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, NbDialog *self)
{
  if (keyval != GDK_KEY_Escape && keyval != GDK_KEY_Back)
    return FALSE;
  nb_dialog_respond (self, NB_RESPONSE_CANCEL);
  return TRUE;
}

static void
on_dialog_button (NeoPressable *button, NbDialog *self)
{
  nb_dialog_respond (self, GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "nb-response")));
}

static void
nb_dialog_dispose (GObject *object)
{
  NbDialog *self = NB_DIALOG (object);

  g_clear_pointer (&self->anim, neo_anim_free);
  g_clear_pointer (&self->card, gtk_widget_unparent);
  g_clear_pointer (&self->sticky, g_array_unref);
  G_OBJECT_CLASS (nb_dialog_parent_class)->dispose (object);
}

static void
nb_dialog_class_init (NbDialogClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = nb_dialog_dispose;
  widget_class->measure = nb_dialog_measure;
  widget_class->size_allocate = nb_dialog_size_allocate;
  widget_class->snapshot = nb_dialog_snapshot;
  gtk_widget_class_set_css_name (widget_class, "nb-dialog-scrim");
  /* a dialog to the shell's back gesture, which then sends Escape */
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_DIALOG);
  dialog_signals[DLG_RESPONSE] = g_signal_new ("response", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                               NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
}

static void
nb_dialog_init (NbDialog *self)
{
  self->sticky = g_array_new (FALSE, FALSE, sizeof (int));
  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
  self->card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (self->card, "nb-dialog");
  gtk_widget_set_overflow (self->card, GTK_OVERFLOW_HIDDEN);
  gtk_widget_set_parent (self->card, GTK_WIDGET (self));

  self->title = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (self->title), 0);
  gtk_label_set_wrap (GTK_LABEL (self->title), TRUE);
  gtk_widget_add_css_class (self->title, "nb-dialog-title");
  neo_label_set_font_padding (self->title, 24);
  gtk_widget_set_visible (self->title, FALSE);
  gtk_box_append (GTK_BOX (self->card), self->title);

  self->message = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (self->message), 0);
  gtk_label_set_wrap (GTK_LABEL (self->message), TRUE);
  gtk_widget_add_css_class (self->message, "nb-dialog-message");
  neo_label_set_font_padding (self->message, 14);
  gtk_widget_set_visible (self->message, FALSE);
  gtk_box_append (GTK_BOX (self->card), self->message);

  self->buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class (self->buttons, "nb-dialog-buttons");
  self->start = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  self->end = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_hexpand (self->start, TRUE);
  gtk_box_append (GTK_BOX (self->buttons), self->start);
  gtk_box_append (GTK_BOX (self->buttons), self->end);
  gtk_widget_set_visible (self->buttons, FALSE);
  gtk_box_append (GTK_BOX (self->card), self->buttons);

  self->anim = neo_anim_new (GTK_WIDGET (self), on_dialog_anim, self);

  GtkGesture *click = gtk_gesture_click_new ();
  g_signal_connect (click, "pressed", G_CALLBACK (on_dialog_pressed), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
  GtkEventController *keys = gtk_event_controller_key_new ();
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_dialog_key), self);
  gtk_widget_add_controller (GTK_WIDGET (self), keys);
}

NbDialog *
nb_dialog_new (const char *title)
{
  NbDialog *self = g_object_new (NB_TYPE_DIALOG, NULL);

  if (title) {
    gtk_label_set_text (GTK_LABEL (self->title), title);
    gtk_widget_set_visible (self->title, TRUE);
  }
  return self;
}

void
nb_dialog_set_message (NbDialog *self, const char *message)
{
  gtk_label_set_text (GTK_LABEL (self->message), message ? message : "");
  gtk_widget_set_visible (self->message, message != NULL);
}

void
nb_dialog_set_content (NbDialog *self, GtkWidget *content)
{
  if (self->content)
    gtk_box_remove (GTK_BOX (self->card), self->content);
  self->content = content;
  if (content)
    gtk_box_insert_child_after (GTK_BOX (self->card), content, self->message);
}

GtkWidget *
nb_dialog_add_button (NbDialog *self, const char *label, int response)
{
  GtkWidget *button = neo_text_button_new (label);

  g_object_set_data (G_OBJECT (button), "nb-response", GINT_TO_POINTER (response));
  g_signal_connect (button, "clicked", G_CALLBACK (on_dialog_button), self);
  gtk_box_append (GTK_BOX (response == NB_RESPONSE_NEUTRAL ? self->start : self->end), button);
  gtk_widget_set_visible (self->buttons, TRUE);
  return button;
}

/* ---- the list dialog ---- */

typedef struct {
  NbListPicked picked;
  gpointer     data;
  NbDialog    *dialog;
} ListPick;

static void
on_list_row (NeoPressable *row, ListPick *pick)
{
  int index = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "nb-index"));
  GtkWidget *rows = gtk_widget_get_parent (GTK_WIDGET (row));

  for (GtkWidget *r = gtk_widget_get_first_child (rows); r; r = gtk_widget_get_next_sibling (r))
    nb_mark_set_active (g_object_get_data (G_OBJECT (r), "nb-mark"), r == GTK_WIDGET (row));
  if (pick->picked)
    pick->picked (index, pick->data);
  pick->picked = NULL;
  nb_dialog_close (pick->dialog);
}

NbDialog *
nb_list_dialog_present (GtkWidget *parent, const char *title, const char *const *items, int selected,
                        NbListPicked picked, gpointer data)
{
  NbDialog *d = nb_dialog_new (title);
  GtkWidget *rows = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  ListPick *pick = g_new0 (ListPick, 1);

  pick->picked = picked;
  pick->data = data;
  pick->dialog = d;
  g_object_set_data_full (G_OBJECT (d), "nb-pick", pick, g_free);
  /* select_dialog_singlechoice_material: 48 dp rows, the radio at 26 dp, the text at 72 dp (measured) */
  for (int i = 0; items[i]; i++) {
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *mark = nb_mark_new (NB_MARK_RADIO);
    GtkWidget *text = nb_label (items[i], 16, "nb-list-text", TRUE);
    gtk_widget_add_css_class (box, "nb-list-row");
    nb_mark_set_active (NB_MARK (mark), i == selected);
    gtk_widget_set_valign (mark, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand (text, TRUE);
    gtk_box_append (GTK_BOX (box), mark);
    gtk_box_append (GTK_BOX (box), text);
    GtkWidget *row = neo_pressable_new (box);
    g_object_set_data (G_OBJECT (row), "nb-index", GINT_TO_POINTER (i));
    g_object_set_data (G_OBJECT (row), "nb-mark", mark);
    g_signal_connect (row, "clicked", G_CALLBACK (on_list_row), pick);
    gtk_box_append (GTK_BOX (rows), row);
  }
  gtk_widget_add_css_class (rows, "nb-list");
  /* the RecycleListView: as tall as the rows, scrolled when the window is shorter */
  GtkWidget *scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), rows);
  gtk_widget_set_vexpand (scroll, TRUE);
  nb_dialog_set_content (d, scroll);
  nb_dialog_add_button (d, "Cancel", NB_RESPONSE_CANCEL);
  nb_dialog_present (d, parent);
  return d;
}

/* ==== NbSheet ====================================================================================================
 *
 * BottomSheetDialog: the content at the bottom, full width, sliding up from below its own height while the 32 %
 * dim fades in (design_bottom_sheet_slide_in: 150 ms, linear_out_slow_in; out the reverse, fast_out_linear_in). */

#define SHEET_IN_MS  150
#define SHEET_OUT_MS 150

struct _NbSheet {
  GtkWidget parent;
  GtkWidget *content;
  double     dim, shown;
  NeoAnim   *anim;
  gboolean   closing;
};

G_DEFINE_FINAL_TYPE (NbSheet, nb_sheet, GTK_TYPE_WIDGET)

enum { SHEET_CLOSED, SHEET_N_SIGNALS };
static guint sheet_signals[SHEET_N_SIGNALS];

static void
nb_sheet_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                  int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 0;
}

static void
nb_sheet_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  NbSheet *self = NB_SHEET (widget);
  int h, min_h;

  gtk_widget_measure (self->content, GTK_ORIENTATION_VERTICAL, width, &min_h, &h, NULL, NULL);
  h = MIN (MAX (h, min_h), height);
  gtk_widget_allocate (self->content, width, h, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (0, height - h * self->shown)));
}

static void
nb_sheet_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  NbSheet *self = NB_SHEET (widget);
  GdkRGBA scrim = { 0, 0, 0, self->dim * self->shown };

  gtk_snapshot_append_color (snapshot, &scrim,
                             &GRAPHENE_RECT_INIT (0, 0, gtk_widget_get_width (widget), gtk_widget_get_height (widget)));
  gtk_widget_snapshot_child (widget, self->content, snapshot);
}

static void
on_sheet_anim (NeoAnim *anim, double value, gpointer data)
{
  NbSheet *self = data;

  self->shown = value;
  gtk_widget_queue_allocate (GTK_WIDGET (self));
  if (self->closing && !neo_anim_is_running (anim)) {
    GtkWidget *overlay = gtk_widget_get_parent (GTK_WIDGET (self));
    g_object_ref (self);
    if (overlay)
      gtk_overlay_remove_overlay (GTK_OVERLAY (overlay), GTK_WIDGET (self));
    g_signal_emit (self, sheet_signals[SHEET_CLOSED], 0);
    g_object_unref (self);
  }
}

void
nb_sheet_dismiss (NbSheet *self)
{
  if (self->closing)
    return;
  self->closing = TRUE;
  gtk_widget_set_can_target (GTK_WIDGET (self), FALSE);
  neo_anim_start (self->anim, self->shown, 0, SHEET_OUT_MS, 0, SPONGY_FAST_OUT_LINEAR_IN);
}

void
nb_sheet_present (NbSheet *self, GtkWidget *parent)
{
  GtkOverlay *overlay = neo_window_get_overlay (parent);

  g_return_if_fail (overlay != NULL);
  gtk_overlay_add_overlay (overlay, GTK_WIDGET (self));
  gtk_overlay_set_measure_overlay (overlay, GTK_WIDGET (self), FALSE);
  gtk_overlay_set_clip_overlay (overlay, GTK_WIDGET (self), TRUE);
  neo_anim_start (self->anim, 0, 1, SHEET_IN_MS, 0, SPONGY_LINEAR_OUT_SLOW_IN);
  gtk_widget_grab_focus (GTK_WIDGET (self));
}

static void
on_sheet_pressed (GtkGestureClick *click, int n_press, double x, double y, NbSheet *self)
{
  graphene_rect_t box;

  if (gtk_widget_compute_bounds (self->content, GTK_WIDGET (self), &box) &&
      graphene_rect_contains_point (&box, &GRAPHENE_POINT_INIT (x, y)))
    return;
  gtk_gesture_set_state (GTK_GESTURE (click), GTK_EVENT_SEQUENCE_CLAIMED);
  nb_sheet_dismiss (self);
}

static gboolean
on_sheet_key (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, NbSheet *self)
{
  if (keyval != GDK_KEY_Escape && keyval != GDK_KEY_Back)
    return FALSE;
  nb_sheet_dismiss (self);
  return TRUE;
}

static void
nb_sheet_dispose (GObject *object)
{
  NbSheet *self = NB_SHEET (object);

  g_clear_pointer (&self->anim, neo_anim_free);
  g_clear_pointer (&self->content, gtk_widget_unparent);
  G_OBJECT_CLASS (nb_sheet_parent_class)->dispose (object);
}

static void
nb_sheet_class_init (NbSheetClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = nb_sheet_dispose;
  widget_class->measure = nb_sheet_measure;
  widget_class->size_allocate = nb_sheet_size_allocate;
  widget_class->snapshot = nb_sheet_snapshot;
  gtk_widget_class_set_css_name (widget_class, "nb-sheet");
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_DIALOG);
  sheet_signals[SHEET_CLOSED] = g_signal_new ("closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                              NULL, G_TYPE_NONE, 0);
}

static void
nb_sheet_init (NbSheet *self)
{
  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
  self->anim = neo_anim_new (GTK_WIDGET (self), on_sheet_anim, self);
  GtkGesture *click = gtk_gesture_click_new ();
  g_signal_connect (click, "pressed", G_CALLBACK (on_sheet_pressed), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
  GtkEventController *keys = gtk_event_controller_key_new ();
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_sheet_key), self);
  gtk_widget_add_controller (GTK_WIDGET (self), keys);
}

NbSheet *
nb_sheet_new (GtkWidget *content, double dim)
{
  NbSheet *self = g_object_new (NB_TYPE_SHEET, NULL);

  self->dim = dim;
  self->content = content;
  gtk_widget_set_parent (content, GTK_WIDGET (self));
  return self;
}

GtkWidget *
nb_sheet_row (const char *icon_name, GdkPaintable *icon, const char *text)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *image;

  if (icon) {
    image = gtk_image_new_from_paintable (icon);
    gtk_image_set_pixel_size (GTK_IMAGE (image), 24);
  } else {
    image = nb_icon (icon_name, 24);
  }
  gtk_widget_add_css_class (image, "nb-sheet-icon");
  gtk_box_append (GTK_BOX (box), image);
  GtkWidget *label = nb_label (text, 14, "nb-sheet-text", TRUE);
  gtk_box_append (GTK_BOX (box), label);
  gtk_widget_add_css_class (box, "nb-sheet-row");
  return neo_pressable_new (box);
}

/* ==== the toast ===================================================================================================
 *
 * As Calculator's CalcToast (SystemUI's text_toast.xml and ToastDefaultAnimation): a 48 dp pill 48 dp above the
 * bottom, the app's icon and 14 sp text; it scales up from 0.9 over 333 ms while fading in, stays 3.5 s
 * (LENGTH_LONG: "Cleared all the cookies" is Toast.LENGTH_LONG) and goes the reverse way in 250 ms. */

#define TOAST_LONG_MS 3500
#define TOAST_IN_MS   333
#define TOAST_OUT_MS  250
#define TOAST_ICON    "neo.Browser"

typedef struct {
  GtkWidget   *pill;
  double       scale;
  gint64       start_us;
  SpongyInterp in_curve, out_curve;
} Toast;

static void
toast_free (gpointer data)
{
  g_free (data);
}

static gboolean
on_toast_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  Toast *t = g_object_get_data (G_OBJECT (widget), "nb-toast");
  gint64 now = gdk_frame_clock_get_frame_time (clock);
  double ms, scale, opacity;

  if (!t->start_us)
    t->start_us = now;
  ms = (now - t->start_us) / 1000.0;
  if (ms < TOAST_LONG_MS) {
    scale = 0.9 + 0.1 * spongy_interp_get (&t->in_curve, CLAMP (ms / TOAST_IN_MS, 0, 1));
    opacity = CLAMP (ms / 66, 0, 1);
  } else if (ms < TOAST_LONG_MS + TOAST_OUT_MS) {
    double u = ms - TOAST_LONG_MS;
    scale = 1 - 0.1 * spongy_interp_get (&t->out_curve, CLAMP (u / TOAST_OUT_MS, 0, 1));
    opacity = 1 - CLAMP (u / TOAST_OUT_MS, 0, 1);
  } else {
    GtkWidget *overlay = gtk_widget_get_parent (widget);
    if (overlay) {
      g_object_set_data (G_OBJECT (overlay), "nb-toast", NULL);
      gtk_overlay_remove_overlay (GTK_OVERLAY (overlay), widget);
    }
    return G_SOURCE_REMOVE;
  }
  gtk_widget_set_opacity (widget, opacity);
  t->scale = scale;
  gtk_widget_queue_draw (t->pill);
  return G_SOURCE_CONTINUE;
}

void
nb_toast_show (GtkWidget *widget, const char *text)
{
  GtkOverlay *overlay = neo_window_get_overlay (widget);
  GtkWidget *old, *holder, *pill, *icon;
  GtkIconTheme *theme;

  g_return_if_fail (overlay != NULL);
  old = g_object_get_data (G_OBJECT (overlay), "nb-toast");
  if (old)
    gtk_overlay_remove_overlay (overlay, old);
  pill = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_add_css_class (pill, "nb-toast");
  theme = gtk_icon_theme_get_for_display (gtk_widget_get_display (widget));
  if (gtk_icon_theme_has_icon (theme, TOAST_ICON)) {
    icon = gtk_image_new_from_icon_name (TOAST_ICON);
    gtk_image_set_pixel_size (GTK_IMAGE (icon), 24);
    gtk_box_append (GTK_BOX (pill), icon);
  }
  gtk_box_append (GTK_BOX (pill), nb_label (text, 14, "nb-toast-text", TRUE));
  holder = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_halign (pill, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (holder, GTK_ALIGN_END);
  gtk_widget_set_margin_bottom (holder, 48);
  gtk_box_append (GTK_BOX (holder), pill);
  gtk_widget_set_can_target (holder, FALSE);
  gtk_widget_set_opacity (holder, 0);
  Toast *t = g_new0 (Toast, 1);
  t->pill = pill;
  spongy_interp_cubic (&t->in_curve, 0, 0, 0, 1);
  spongy_interp_cubic (&t->out_curve, 0.3f, 0, 1, 1);
  g_object_set_data_full (G_OBJECT (holder), "nb-toast", t, toast_free);
  g_object_set_data (G_OBJECT (overlay), "nb-toast", holder);
  gtk_overlay_add_overlay (overlay, holder);
  gtk_overlay_set_measure_overlay (overlay, holder, FALSE);
  gtk_widget_add_tick_callback (holder, on_toast_tick, NULL, NULL);
}

/* ==== NbSwitch ===================================================================================================
 *
 * Theme.Jelly.Settings.Switch: the platform Switch with settingslib_switch_track (28 dp tall, radius 35, stretched
 * to the switch's width: 2 × the 28 dp thumb drawable = 56 dp) and settingslib_switch_thumb (a 20 dp oval inset
 * 4 dp). Off: track surfaceVariant, thumb outline; on: track primary, thumb onPrimary (still:settings). The thumb
 * slides in 250 ms (Switch's THUMB_ANIMATION_DURATION); the colours change at once (colour state lists). */

#define SW_W 56
#define SW_H 28
#define SW_TARGET 48
#define SW_THUMB 20
#define SW_INSET 4
#define SW_MS 250

struct _NbSwitch {
  GtkWidget parent;
  gboolean  active;
  double    pos;              /* 0 off .. 1 on */
  NeoAnim  *anim;
};

G_DEFINE_FINAL_TYPE (NbSwitch, nb_switch, GTK_TYPE_WIDGET)

enum { SW_PROP_0, SW_PROP_ACTIVE, SW_N_PROPS };
static GParamSpec *switch_props[SW_N_PROPS];

static void
nb_switch_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                   int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = orientation == GTK_ORIENTATION_HORIZONTAL ? SW_W : SW_TARGET;
}

static void
nb_switch_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  NbSwitch *self = NB_SWITCH (widget);
  double y = (gtk_widget_get_height (widget) - SW_H) / 2.0;
  GdkRGBA track = neo_color (self->active ? NEO_PRIMARY : NEO_SURFACE_CONTAINER_HIGHEST);
  GdkRGBA thumb = neo_color (self->active ? NEO_ON_PRIMARY : NEO_OUTLINE);
  GskRoundedRect r;
  double x = SW_INSET + self->pos * (SW_W - 2 * SW_INSET - SW_THUMB);

  gsk_rounded_rect_init_from_rect (&r, &GRAPHENE_RECT_INIT (0, y, SW_W, SW_H), SW_H / 2.0);
  gtk_snapshot_push_rounded_clip (snapshot, &r);
  gtk_snapshot_append_color (snapshot, &track, &r.bounds);
  gtk_snapshot_pop (snapshot);
  gsk_rounded_rect_init_from_rect (&r, &GRAPHENE_RECT_INIT (x, y + SW_INSET, SW_THUMB, SW_THUMB), SW_THUMB / 2.0);
  gtk_snapshot_push_rounded_clip (snapshot, &r);
  gtk_snapshot_append_color (snapshot, &thumb, &r.bounds);
  gtk_snapshot_pop (snapshot);
}

static void
on_switch_anim (NeoAnim *anim, double value, gpointer data)
{
  NB_SWITCH (data)->pos = value;
  gtk_widget_queue_draw (GTK_WIDGET (data));
}

gboolean
nb_switch_get_active (NbSwitch *self)
{
  return self->active;
}

void
nb_switch_set_active (NbSwitch *self, gboolean active)
{
  active = !!active;
  if (self->active == active)
    return;
  self->active = active;
  if (gtk_widget_get_mapped (GTK_WIDGET (self)))
    neo_anim_start (self->anim, self->pos, active, SW_MS, 0, SPONGY_ACCELERATE_DECELERATE);
  else
    self->pos = active;
  gtk_accessible_update_state (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_STATE_CHECKED,
                               active ? GTK_ACCESSIBLE_TRISTATE_TRUE : GTK_ACCESSIBLE_TRISTATE_FALSE, -1);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_object_notify_by_pspec (G_OBJECT (self), switch_props[SW_PROP_ACTIVE]);
}

static void
on_switch_released (GtkGestureClick *click, int n, double x, double y, NbSwitch *self)
{
  nb_switch_set_active (self, !self->active);
}

static void
nb_switch_get_property (GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  if (id == SW_PROP_ACTIVE)
    g_value_set_boolean (value, NB_SWITCH (object)->active);
}

static void
nb_switch_set_property (GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
  if (id == SW_PROP_ACTIVE)
    nb_switch_set_active (NB_SWITCH (object), g_value_get_boolean (value));
}

static void
nb_switch_dispose (GObject *object)
{
  g_clear_pointer (&NB_SWITCH (object)->anim, neo_anim_free);
  G_OBJECT_CLASS (nb_switch_parent_class)->dispose (object);
}

static void
nb_switch_class_init (NbSwitchClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = nb_switch_dispose;
  object_class->get_property = nb_switch_get_property;
  object_class->set_property = nb_switch_set_property;
  widget_class->measure = nb_switch_measure;
  widget_class->snapshot = nb_switch_snapshot;
  gtk_widget_class_set_css_name (widget_class, "nb-switch");
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_SWITCH);
  switch_props[SW_PROP_ACTIVE] = g_param_spec_boolean ("active", NULL, NULL, FALSE, G_PARAM_READWRITE);
  g_object_class_install_properties (object_class, SW_N_PROPS, switch_props);
}

static void
nb_switch_init (NbSwitch *self)
{
  self->anim = neo_anim_new (GTK_WIDGET (self), on_switch_anim, self);
  GtkGesture *click = gtk_gesture_click_new ();
  g_signal_connect (click, "released", G_CALLBACK (on_switch_released), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
  /* a row's NeoPressable leaves a press on it to the switch */
  g_object_set_data (G_OBJECT (self), "neo-tappable", GINT_TO_POINTER (1));
}

GtkWidget *
nb_switch_new (void)
{
  return g_object_new (NB_TYPE_SWITCH, NULL);
}

/* ==== NbMark: Material 3 checkbox and radio button ================================================================
 *
 * Checkbox: an 18 dp box, 2 dp corners; off a 2 dp onSurfaceVariant border, on primary filled with an onPrimary
 * check. Radio: a 20 dp ring, 2 dp; off onSurfaceVariant, on secondary with a 10 dp dot (still:dlg-engine:
 * #a2adaf / #b1cbcf). Both in a 48 dp square target. */

struct _NbMark {
  GtkWidget  parent;
  NbMarkKind kind;
  gboolean   active;
};

G_DEFINE_FINAL_TYPE (NbMark, nb_mark, GTK_TYPE_WIDGET)

static void
nb_mark_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                 int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = NB_MARK (widget)->kind == NB_MARK_RADIO ? 20 : 48;
}

static void
nb_mark_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  NbMark *self = NB_MARK (widget);
  double w = gtk_widget_get_width (widget), h = gtk_widget_get_height (widget);
  GskRoundedRect r;

  if (self->kind == NB_MARK_RADIO) {
    GdkRGBA c = neo_color (self->active ? NEO_SECONDARY : NEO_ON_SURFACE_VARIANT);
    double cx = w / 2, cy = h / 2;
    float widths[4] = { 2, 2, 2, 2 };
    GdkRGBA colors[4] = { c, c, c, c };
    gsk_rounded_rect_init_from_rect (&r, &GRAPHENE_RECT_INIT (cx - 10, cy - 10, 20, 20), 10);
    gtk_snapshot_append_border (snapshot, &r, widths, colors);
    if (self->active) {
      gsk_rounded_rect_init_from_rect (&r, &GRAPHENE_RECT_INIT (cx - 5, cy - 5, 10, 10), 5);
      gtk_snapshot_push_rounded_clip (snapshot, &r);
      gtk_snapshot_append_color (snapshot, &c, &r.bounds);
      gtk_snapshot_pop (snapshot);
    }
    return;
  }
  double x = (w - 18) / 2, y = (h - 18) / 2;
  gsk_rounded_rect_init_from_rect (&r, &GRAPHENE_RECT_INIT (x, y, 18, 18), 2);
  if (!self->active) {
    GdkRGBA c = neo_color (NEO_ON_SURFACE_VARIANT);
    float widths[4] = { 2, 2, 2, 2 };
    GdkRGBA colors[4] = { c, c, c, c };
    gtk_snapshot_append_border (snapshot, &r, widths, colors);
    return;
  }
  GdkRGBA fill = neo_color (NEO_PRIMARY), mark = neo_color (NEO_ON_PRIMARY);
  gtk_snapshot_push_rounded_clip (snapshot, &r);
  gtk_snapshot_append_color (snapshot, &fill, &r.bounds);
  gtk_snapshot_pop (snapshot);
  /* the check: M3's ic_mtrl_checked_circle path, as two strokes */
  GskPathBuilder *b = gsk_path_builder_new ();
  gsk_path_builder_move_to (b, x + 4, y + 9.5);
  gsk_path_builder_line_to (b, x + 7.5, y + 13);
  gsk_path_builder_line_to (b, x + 14, y + 5.5);
  GskPath *path = gsk_path_builder_free_to_path (b);
  GskStroke *stroke = gsk_stroke_new (2);
  gtk_snapshot_append_stroke (snapshot, path, stroke, &mark);
  gsk_stroke_free (stroke);
  gsk_path_unref (path);
}

static void
nb_mark_class_init (NbMarkClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  widget_class->measure = nb_mark_measure;
  widget_class->snapshot = nb_mark_snapshot;
  gtk_widget_class_set_css_name (widget_class, "nb-mark");
}

static void
nb_mark_init (NbMark *self)
{
  gtk_widget_set_can_target (GTK_WIDGET (self), FALSE);
}

GtkWidget *
nb_mark_new (NbMarkKind kind)
{
  NbMark *self = g_object_new (NB_TYPE_MARK, NULL);

  self->kind = kind;
  gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  kind == NB_MARK_RADIO ? "radio" : "check", -1);
  return GTK_WIDGET (self);
}

gboolean
nb_mark_get_active (NbMark *self)
{
  return self->active;
}

void
nb_mark_set_active (NbMark *self, gboolean active)
{
  self->active = !!active;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* ==== the toolbar ================================================================================================
 *
 * still:history: the up button [0,128]..[168,296] (56 dp square, 4 dp down), its arrow onSurfaceVariant; the
 * title from x 180 (60 dp), 22 sp; an action [936,140]..[1080,284] (48 dp at the end). */

GtkWidget *
nb_icon (const char *icon_name, int size)
{
  GtkWidget *image = gtk_image_new_from_icon_name (icon_name);

  gtk_image_set_pixel_size (GTK_IMAGE (image), size);
  return image;
}

GtkWidget *
nb_label (const char *text, double size, const char *css, gboolean ellipsize)
{
  GtkWidget *label = gtk_label_new (text);
  PangoAttrList *attrs = pango_attr_list_new ();

  pango_attr_list_insert (attrs, pango_attr_size_new_absolute ((int) round (size * PANGO_SCALE)));
  gtk_label_set_attributes (GTK_LABEL (label), attrs);
  pango_attr_list_unref (attrs);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  /* a TextView's includeFontPadding: Roboto's yMax / yMin above and below the line */
  neo_label_set_font_padding (label, size);
  /* centred across a row taller than it (a row's gravity center_vertical); a caller places it otherwise */
  gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
  if (ellipsize) {
    gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
    gtk_label_set_single_line_mode (GTK_LABEL (label), TRUE);
  }
  if (css)
    gtk_widget_add_css_class (label, css);
  return label;
}

GtkWidget *
nb_action_button (const char *icon_name, const char *tooltip)
{
  GtkWidget *button = neo_icon_button_new (icon_name);

  if (tooltip)
    gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, tooltip, -1);
  return button;
}

GtkWidget *
nb_toolbar_new (const char *title, double title_size, GCallback up, gpointer data, GtkWidget *actions)
{
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *up_button = neo_icon_button_new ("jelly-back-symbolic");
  GtkWidget *label;

  gtk_widget_add_css_class (bar, "nb-toolbar");
  gtk_widget_add_css_class (up_button, "nb-up");
  gtk_widget_set_size_request (up_button, 56, 56);
  gtk_widget_set_valign (up_button, GTK_ALIGN_CENTER);
  /* the shell's back gesture clicks the showing button named "Back" */
  gtk_accessible_update_property (GTK_ACCESSIBLE (up_button), GTK_ACCESSIBLE_PROPERTY_LABEL, "Back", -1);
  g_signal_connect_swapped (up_button, "clicked", up, data);
  gtk_box_append (GTK_BOX (bar), up_button);
  label = nb_label (title, title_size > 0 ? title_size : 22, "nb-toolbar-title", TRUE);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (label, 4);
  gtk_box_append (GTK_BOX (bar), label);
  g_object_set_data (G_OBJECT (bar), "nb-title", label);
  if (actions)
    gtk_box_append (GTK_BOX (bar), actions);
  return bar;
}

GtkWidget *
nb_toolbar_get_title (GtkWidget *toolbar)
{
  return g_object_get_data (G_OBJECT (toolbar), "nb-title");
}

/* ==== the outlined text field =====================================================================================
 *
 * TextInputLayout.OutlinedBox (Material 3's default textInputStyle): 56 dp, a 1 dp outline with 4 dp corners (2 dp
 * primary while focused), the hint inside until there is text or focus, then a 12 sp label on the outline. */

static void
sync_field (GtkWidget *field)
{
  GtkEntry *entry = nb_field_get_entry (field);
  GtkWidget *label = g_object_get_data (G_OBJECT (field), "nb-label");
  GtkEventController *focus = g_object_get_data (G_OBJECT (field), "nb-focus");
  gboolean focused = focus && gtk_event_controller_focus_contains_focus (GTK_EVENT_CONTROLLER_FOCUS (focus));
  gboolean has_text = gtk_entry_get_text_length (entry) > 0;

  gtk_widget_set_visible (label, focused || has_text);
  gtk_entry_set_placeholder_text (entry, focused || has_text ? NULL : g_object_get_data (G_OBJECT (field), "nb-hint"));
  if (focused)
    gtk_widget_add_css_class (field, "focused");
  else
    gtk_widget_remove_css_class (field, "focused");
}

GtkWidget *
nb_field_new (const char *label_text, gboolean password)
{
  GtkWidget *field = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *overlay = gtk_overlay_new ();
  GtkWidget *frame = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *entry = gtk_entry_new ();
  GtkWidget *label = nb_label (label_text, 12, "nb-field-label", TRUE);
  GtkWidget *error = nb_label (NULL, 12, "nb-field-error", FALSE);

  gtk_widget_add_css_class (field, "nb-field");
  gtk_widget_add_css_class (frame, "nb-field-box");
  gtk_widget_add_css_class (entry, "nb-field-entry");
  gtk_widget_set_hexpand (entry, TRUE);
  if (password) {
    gtk_entry_set_visibility (GTK_ENTRY (entry), FALSE);
    gtk_entry_set_input_purpose (GTK_ENTRY (entry), GTK_INPUT_PURPOSE_PASSWORD);
  }
  gtk_box_append (GTK_BOX (frame), entry);
  gtk_overlay_set_child (GTK_OVERLAY (overlay), frame);
  gtk_widget_set_halign (label, GTK_ALIGN_START);
  gtk_widget_set_valign (label, GTK_ALIGN_START);
  gtk_widget_set_can_target (label, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), label);
  gtk_box_append (GTK_BOX (field), overlay);
  gtk_widget_set_visible (error, FALSE);
  gtk_box_append (GTK_BOX (field), error);
  g_object_set_data (G_OBJECT (field), "nb-entry", entry);
  g_object_set_data (G_OBJECT (field), "nb-label", label);
  g_object_set_data (G_OBJECT (field), "nb-error", error);
  g_object_set_data_full (G_OBJECT (field), "nb-hint", g_strdup (label_text), g_free);
  GtkEventController *focus = gtk_event_controller_focus_new ();
  g_object_set_data (G_OBJECT (field), "nb-focus", focus);
  g_signal_connect_swapped (focus, "notify::contains-focus", G_CALLBACK (sync_field), field);
  gtk_widget_add_controller (entry, focus);
  g_signal_connect_swapped (entry, "changed", G_CALLBACK (sync_field), field);
  sync_field (field);
  return field;
}

GtkEntry *
nb_field_get_entry (GtkWidget *field)
{
  return g_object_get_data (G_OBJECT (field), "nb-entry");
}

void
nb_field_set_error (GtkWidget *field, const char *error)
{
  GtkWidget *label = g_object_get_data (G_OBJECT (field), "nb-error");

  gtk_label_set_text (GTK_LABEL (label), error ? error : "");
  gtk_widget_set_visible (label, error != NULL);
  if (error)
    gtk_widget_add_css_class (field, "error");
  else
    gtk_widget_remove_css_class (field, "error");
}

/* ==== the scroll indicator ========================================================================================
 *
 * A View's vertical scrollbar (still:menu x 1019..1031: 4 dp, #596163): the thumb is the visible fraction of the
 * content, it shows while the view scrolls and for 300 ms after (ViewConfiguration's scroll bar fade delay), then
 * fades over 250 ms; when the view appears it shows for 4 × 300 ms (awakenScrollBars on attach). */

#define IND_W         4
#define IND_DELAY     300
#define IND_FADE      250

typedef struct {
  GtkAdjustment *adj;
  gint64         until_us;    /* fully shown until then, frame time */
  guint          tick;
} Indicator;

static void
indicator_free (gpointer data)
{
  Indicator *ind = data;

  g_object_unref (ind->adj);
  g_free (ind);
}

static void
indicator_draw (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  Indicator *ind = data;
  double upper = gtk_adjustment_get_upper (ind->adj), page = gtk_adjustment_get_page_size (ind->adj);
  double value = gtk_adjustment_get_value (ind->adj);

  if (upper <= page + 0.5 || page <= 0)
    return;
  double len = MAX (height * page / upper, 2 * IND_W);
  double y = (height - len) * value / MAX (1, upper - page);
  cairo_set_source_rgb (cr, 0x59 / 255.0, 0x61 / 255.0, 0x63 / 255.0);
  cairo_rectangle (cr, width - IND_W, y, IND_W, len);
  cairo_fill (cr);
}

static gboolean
on_indicator_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  Indicator *ind = data;
  gint64 now = gdk_frame_clock_get_frame_time (clock);

  if (now <= ind->until_us) {
    gtk_widget_set_opacity (widget, 1);
    return G_SOURCE_CONTINUE;
  }
  double t = (now - ind->until_us) / 1000.0 / IND_FADE;
  gtk_widget_set_opacity (widget, 1 - CLAMP (t, 0, 1));
  if (t >= 1) {
    ind->tick = 0;
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static void
indicator_show (GtkWidget *widget, guint ms)
{
  Indicator *ind = g_object_get_data (G_OBJECT (widget), "nb-indicator");
  GdkFrameClock *clock = gtk_widget_get_frame_clock (widget);
  gint64 now = clock ? gdk_frame_clock_get_frame_time (clock) : g_get_monotonic_time ();

  ind->until_us = MAX (ind->until_us, now + ms * 1000);
  gtk_widget_set_opacity (widget, 1);
  gtk_widget_queue_draw (widget);
  if (!ind->tick)
    ind->tick = gtk_widget_add_tick_callback (widget, on_indicator_tick, ind, NULL);
}

static void
on_indicator_scrolled (GtkWidget *widget)
{
  indicator_show (widget, IND_DELAY);
}

void
nb_indicator_awaken (GtkWidget *indicator)
{
  indicator_show (indicator, 4 * IND_DELAY);
}

GtkWidget *
nb_indicator_new (GtkAdjustment *adjustment)
{
  GtkWidget *area = gtk_drawing_area_new ();
  Indicator *ind = g_new0 (Indicator, 1);

  ind->adj = g_object_ref (adjustment);
  g_object_set_data_full (G_OBJECT (area), "nb-indicator", ind, indicator_free);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (area), indicator_draw, ind, NULL);
  gtk_widget_set_size_request (area, IND_W, -1);
  gtk_widget_set_halign (area, GTK_ALIGN_END);
  gtk_widget_set_can_target (area, FALSE);
  gtk_widget_set_opacity (area, 0);
  g_signal_connect_object (adjustment, "value-changed", G_CALLBACK (on_indicator_scrolled), area,
                           G_CONNECT_SWAPPED);
  g_signal_connect_object (adjustment, "changed", G_CALLBACK (gtk_widget_queue_draw), area, G_CONNECT_SWAPPED);
  return area;
}
