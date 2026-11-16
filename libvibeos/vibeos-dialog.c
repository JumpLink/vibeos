/* VibeosDialog: see vibeos-dialog.h. The card is laid out by hand over the scrim, and moves on one VibeosAnim: in, 0 -> 1;
 * out, 1 -> 0. */
#include "vibeos-dialog.h"
#include "vibeos-anim.h"
#include "vibeos-pressable.h"
#include "vibeos-style.h"

#define SIDE_INSET   24       /* the card keeps this far from the window's edges */
#define MAX_WIDTH    560      /* and grows no wider on a wide window */
#define ENTER_MS     220
#define ENTER_RISE   20
#define EXIT_MS      150
#define EXIT_RISE    10

struct _VibeosDialog {
  GtkWidget  parent;
  GtkWidget *card, *title, *message, *content, *buttons;
  GtkWidget *anchor;          /* weak: the pane the card centres on, or NULL for the window */
  double     shown;           /* the window animation: 0 .. 1 */
  double     rise;            /* dp the card is below (positive) or above (negative) its place */
  VibeosAnim   *anim;
  gboolean   closing;
  gboolean   flush;
};

G_DEFINE_FINAL_TYPE (VibeosDialog, vibeos_dialog, GTK_TYPE_WIDGET)

enum { RESPONSE, N_SIGNALS };
static guint signals[N_SIGNALS];

/* ---- layout: the scrim fills the window, the card is centred in it ---------------------------------------------- */

static int
card_width (int width)
{
  return MIN (width - 2 * SIDE_INSET, MAX_WIDTH);
}

static void
vibeos_dialog_measure (GtkWidget *widget, GtkOrientation orientation, int for_size,
                    int *minimum, int *natural, int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 0;     /* it takes whatever the overlay gives it: the whole window */
}

static void
vibeos_dialog_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  VibeosDialog *self = VIBEOS_DIALOG (widget);
  int min_w, nat_w, h, min_h;
  /* the space the card is centred in: the window, or the pane it belongs to (the scrim still covers the window) */
  graphene_rect_t area = GRAPHENE_RECT_INIT (0, 0, width, height);
  if (self->anchor && !gtk_widget_compute_bounds (self->anchor, widget, &area))
    area = GRAPHENE_RECT_INIT (0, 0, width, height);
  int across = (int) area.size.width;
  gtk_widget_measure (self->card, GTK_ORIENTATION_HORIZONTAL, -1, &min_w, &nat_w, NULL, NULL);
  /* an alert dialog spans the window less its insets; one that holds a picker wraps it (the time picker's) */
  int w = self->flush ? MIN (nat_w, across) : MAX (card_width (across), min_w);
  /* its content's natural height (a list wraps its rows), up to the window
   * less its insets: a scrolled list then scrolls */
  gtk_widget_measure (self->card, GTK_ORIENTATION_VERTICAL, w, &min_h, &h, NULL, NULL);
  h = MAX (MIN (h, height - 2 * SIDE_INSET), MIN (min_h, height - 2 * SIDE_INSET));
  gtk_widget_allocate (self->card, w, h, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (area.origin.x + (across - w) / 2.0,
                                                                            (height - h) / 2.0)));
}

static void
vibeos_dialog_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  VibeosDialog *self = VIBEOS_DIALOG (widget);
  GdkRGBA scrim = { 0, 0, 0, 0.6f };
  /* the dim fades with the card */
  gtk_snapshot_push_opacity (snapshot, self->shown);
  gtk_snapshot_append_color (snapshot, &scrim,
                             &GRAPHENE_RECT_INIT (0, 0, gtk_widget_get_width (widget), gtk_widget_get_height (widget)));
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (0, self->rise));
  gtk_widget_snapshot_child (widget, self->card, snapshot);
  gtk_snapshot_restore (snapshot);
  gtk_snapshot_pop (snapshot);
}

/* ---- the window animation -------------------------------------------------------------------------------------- */

static void
on_anim (VibeosAnim *anim, double value, gpointer data)
{
  VibeosDialog *self = data;
  self->shown = value;
  self->rise = self->closing ? -EXIT_RISE * (1 - value) : ENTER_RISE * (1 - value);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  if (self->closing && !vibeos_anim_is_running (anim)) {
    GtkWidget *overlay = gtk_widget_get_parent (GTK_WIDGET (self));
    if (overlay)
      gtk_overlay_remove_overlay (GTK_OVERLAY (overlay), GTK_WIDGET (self));
  }
}

void
vibeos_dialog_respond (VibeosDialog *self, int response)
{
  if (self->closing)
    return;
  self->closing = TRUE;
  g_object_ref (self);
  g_signal_emit (self, signals[RESPONSE], 0, response);
  /* out: alpha 1 -> 0 and up 0 -> -10 px over 150 ms, standard-accelerate */
  vibeos_anim_start_cubic (self->anim, 1, 0, EXIT_MS, 0, 0.3f, 0, 1, 1);
  g_object_unref (self);
}

void
vibeos_dialog_present (VibeosDialog *self, GtkWidget *parent)
{
  GtkOverlay *overlay = vibeos_window_get_overlay (parent);
  g_return_if_fail (overlay != NULL);
  gtk_overlay_add_overlay (overlay, GTK_WIDGET (self));
  /* the scrim covers the whole window and takes every touch outside the card */
  gtk_overlay_set_measure_overlay (overlay, GTK_WIDGET (self), FALSE);
  gtk_overlay_set_clip_overlay (overlay, GTK_WIDGET (self), TRUE);
  /* in: alpha 0 -> 1 and up 20 px -> 0 over 220 ms, standard (0.2, 0, 0, 1) */
  vibeos_anim_start_cubic (self->anim, 0, 1, ENTER_MS, 0, 0.2f, 0, 0, 1);
  /* the first field takes the keyboard focus (and on the phone opens the keyboard), as the label dialog's does */
  gtk_widget_child_focus (self->card, GTK_DIR_TAB_FORWARD);
}

static void
on_scrim_pressed (GtkGestureClick *click, int n_press, double x, double y, VibeosDialog *self)
{
  graphene_rect_t card;
  if (gtk_widget_compute_bounds (self->card, GTK_WIDGET (self), &card) &&
      graphene_rect_contains_point (&card, &GRAPHENE_POINT_INIT (x, y)))
    return;
  /* setCanceledOnTouchOutside: a touch outside the card cancels */
  gtk_gesture_set_state (GTK_GESTURE (click), GTK_EVENT_SEQUENCE_CLAIMED);
  vibeos_dialog_respond (self, VIBEOS_RESPONSE_CANCEL);
}

static gboolean
on_key (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, VibeosDialog *self)
{
  if (keyval != GDK_KEY_Escape && keyval != GDK_KEY_Back)
    return FALSE;
  vibeos_dialog_respond (self, VIBEOS_RESPONSE_CANCEL);
  return TRUE;
}

static void
on_button (VibeosPressable *button, VibeosDialog *self)
{
  vibeos_dialog_respond (self, GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "vibeos-response")));
}

/* ---- GObject --------------------------------------------------------------------------------------------------- */

static void
vibeos_dialog_dispose (GObject *object)
{
  VibeosDialog *self = VIBEOS_DIALOG (object);
  vibeos_dialog_set_anchor (self, NULL);
  g_clear_pointer (&self->anim, vibeos_anim_free);
  g_clear_pointer (&self->card, gtk_widget_unparent);
  G_OBJECT_CLASS (vibeos_dialog_parent_class)->dispose (object);
}

static void
vibeos_dialog_class_init (VibeosDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  object_class->dispose = vibeos_dialog_dispose;
  widget_class->measure = vibeos_dialog_measure;
  widget_class->size_allocate = vibeos_dialog_size_allocate;
  widget_class->snapshot = vibeos_dialog_snapshot;
  gtk_widget_class_set_css_name (widget_class, "vibeos-dialog-scrim");
  /* a dialog to the shell's back gesture too (a11y.js), which then closes it with Escape instead of leaving the app */
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_DIALOG);

  signals[RESPONSE] = g_signal_new ("response", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                    NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
}

static void
vibeos_dialog_init (VibeosDialog *self)
{
  self->card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (self->card, "vibeos-dialog");
  gtk_widget_set_overflow (self->card, GTK_OVERFLOW_HIDDEN);
  gtk_widget_set_parent (self->card, GTK_WIDGET (self));

  self->title = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (self->title), 0);
  gtk_label_set_wrap (GTK_LABEL (self->title), TRUE);
  gtk_widget_add_css_class (self->title, "vibeos-dialog-title");
  gtk_widget_set_visible (self->title, FALSE);
  gtk_box_append (GTK_BOX (self->card), self->title);

  self->message = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (self->message), 0);
  gtk_label_set_wrap (GTK_LABEL (self->message), TRUE);
  gtk_widget_add_css_class (self->message, "vibeos-dialog-message");
  gtk_widget_set_visible (self->message, FALSE);
  gtk_box_append (GTK_BOX (self->card), self->message);

  self->buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (self->buttons, GTK_ALIGN_END);
  gtk_widget_add_css_class (self->buttons, "vibeos-dialog-buttons");
  /* shown with its first button: a dialog without any (a list to pick from) has no button row's space */
  gtk_widget_set_visible (self->buttons, FALSE);
  gtk_box_append (GTK_BOX (self->card), self->buttons);

  self->anim = vibeos_anim_new (GTK_WIDGET (self), on_anim, self);

  GtkGesture *click = gtk_gesture_click_new ();
  g_signal_connect (click, "pressed", G_CALLBACK (on_scrim_pressed), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
  GtkEventController *keys = gtk_event_controller_key_new ();
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key), self);
  gtk_widget_add_controller (GTK_WIDGET (self), keys);
}

VibeosDialog *
vibeos_dialog_new (const char *title)
{
  VibeosDialog *self = g_object_new (VIBEOS_TYPE_DIALOG, NULL);
  if (title) {
    gtk_label_set_text (GTK_LABEL (self->title), title);
    gtk_widget_set_visible (self->title, TRUE);
  }
  return self;
}

void
vibeos_dialog_set_message (VibeosDialog *self, const char *message)
{
  gtk_label_set_text (GTK_LABEL (self->message), message);
  gtk_widget_set_visible (self->message, message != NULL);
}

void
vibeos_dialog_set_content (VibeosDialog *self, GtkWidget *content)
{
  if (self->content)
    gtk_box_remove (GTK_BOX (self->card), self->content);
  self->content = content;
  if (content)
    gtk_box_insert_child_after (GTK_BOX (self->card), content, self->message);
}

GtkWidget *
vibeos_dialog_add_button (VibeosDialog *self, const char *label, int response)
{
  GtkWidget *button = vibeos_text_button_new (label);
  g_object_set_data (G_OBJECT (button), "vibeos-response", GINT_TO_POINTER (response));
  g_signal_connect (button, "clicked", G_CALLBACK (on_button), self);
  gtk_box_append (GTK_BOX (self->buttons), button);
  gtk_widget_set_visible (self->buttons, TRUE);
  return button;
}

void
vibeos_dialog_set_flush (VibeosDialog *self, gboolean flush)
{
  self->flush = flush;
  if (flush)
    gtk_widget_add_css_class (self->card, "flush");
  else
    gtk_widget_remove_css_class (self->card, "flush");
}

void
vibeos_dialog_set_anchor (VibeosDialog *self, GtkWidget *pane)
{
  if (self->anchor)
    g_object_remove_weak_pointer (G_OBJECT (self->anchor), (gpointer *) &self->anchor);
  self->anchor = pane;
  if (pane)
    g_object_add_weak_pointer (G_OBJECT (pane), (gpointer *) &self->anchor);
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}
