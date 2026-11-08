/* The main screen's dialpad: see dialpad-sheet.h. The numbers are dialer/dialpadview's DialpadFragment.java and
 * res/layout/dialpad_fragment.xml, and the motion measured on the reference phone (spec §3.1, §3.5, §3.7). */
#include "dialpad-sheet.h"
#include "call-log.h"
#include "call-types.h"
#include "calls-client.h"
#include "dialpad.h"
#include "phone.h"

#include <glib/gi18n.h>
#include <math.h>

#define DIAL_SIZE       56      /* the dial FAB */
#define DIAL_MARGIN     16      /* above the bottom */
#define SLIDE_MS        400     /* dialpad_slide_in_bottom / _out_bottom */
#define FAB_MS          250     /* FloatingActionButtonController */
#define DIAL_GREEN      0x3ee0a0        /* dialer_call_green: the dialpad opts out of force dark */

struct _DialpadSheet
{
  GtkWidget parent;
  GtkWidget *dialpad, *dial;
  NeoSnackbar *snackbar;
  gboolean shown;
  NeoAnim *slide, *fab;
  double progress;              /* 0 down … 1 up */
  double fab_scale;
  GCancellable *recall;         /* looking up the last number called */
};

G_DEFINE_FINAL_TYPE (DialpadSheet, dialpad_sheet, GTK_TYPE_WIDGET)

enum { SHOWN_CHANGED, DIGITS_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static GdkRGBA
rgb (guint32 value)
{
  return (GdkRGBA) { ((value >> 16) & 0xff) / 255.0, ((value >> 8) & 0xff) / 255.0, (value & 0xff) / 255.0, 1 };
}

/* ---- the motion ----------------------------------------------------------------------------------------------- */

static void
on_fab (NeoAnim *anim, double value, gpointer data)
{
  DialpadSheet *self = data;

  self->fab_scale = value;
  neo_pressable_set_scale (NEO_PRESSABLE (self->dial), value);
}

static void
on_slide (NeoAnim *anim, double value, gpointer data)
{
  DialpadSheet *self = data;

  self->progress = value;
  if (!neo_anim_is_running (anim))
    {
      /* the dial button scales in once the dialpad is up; gone, the sheet stops taking touches */
      if (value == 1)
        neo_anim_start (self->fab, self->fab_scale, 1, FAB_MS, 0, SPONGY_FAST_OUT_SLOW_IN);
      else if (value == 0)
        gtk_widget_set_visible (GTK_WIDGET (self), FALSE);
    }
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

/* DialpadFragment.onHiddenChanged: slide up 100 %p → 0 over 400 ms (0, 0, 0.2, 1) */
void
dialpad_sheet_show (DialpadSheet *self, const char *number)
{
  if (number)
    dialer_dialpad_set_digits (DIALER_DIALPAD (self->dialpad), number);
  if (self->shown)
    return;
  self->shown = TRUE;
  gtk_widget_set_visible (GTK_WIDGET (self), TRUE);
  neo_anim_start_cubic (self->slide, self->progress, 1, SLIDE_MS, 0, 0, 0, 0.2, 1);
  g_signal_emit (self, signals[SHOWN_CHANGED], 0);
}

/* hideDialpad: the dial button scales out as the dialpad slides down, 0 → 100 %p over 400 ms (0.4, 0, 1, 1) */
void
dialpad_sheet_hide (DialpadSheet *self)
{
  if (!self->shown)
    return;
  self->shown = FALSE;
  neo_anim_start (self->fab, self->fab_scale, 0, FAB_MS, 0, SPONGY_FAST_OUT_SLOW_IN);
  neo_anim_start_cubic (self->slide, self->progress, 0, SLIDE_MS, 0, 0.4, 0, 1, 1);
  g_signal_emit (self, signals[SHOWN_CHANGED], 0);
}

gboolean
dialpad_sheet_get_shown (DialpadSheet *self)
{
  return self->shown;
}

const char *
dialpad_sheet_get_digits (DialpadSheet *self)
{
  return dialer_dialpad_get_digits (DIALER_DIALPAD (self->dialpad));
}

void
dialpad_sheet_clear (DialpadSheet *self)
{
  dialer_dialpad_set_digits (DIALER_DIALPAD (self->dialpad), "");
}

/* ---- dialling ------------------------------------------------------------------------------------------------- */

static void
on_dialed (guint id, const GError *error, gpointer data)
{
  DialpadSheet *self = data;

  if (!self->dialpad)
    ;                           /* the window closed meanwhile */
  else if (error)
    neo_snackbar_show (self->snackbar, error->message, NULL, NULL, NULL, NEO_SNACKBAR_LONG);
  else
    {
      /* the call is the service's now: its screen comes up, and the dialpad goes away empty (hideAndClearDialpad) */
      phone_start_app (INCALL_APP_ID);
      dialer_dialpad_set_digits (DIALER_DIALPAD (self->dialpad), "");
      dialpad_sheet_hide (self);
    }
  g_object_unref (self);
}

/* handleDialButtonPressed with no digits: the last number called comes back into the field */
static void
on_call_log (GObject *source, GAsyncResult *result, gpointer data)
{
  DialpadSheet *self = data;
  g_autoptr(GPtrArray) entries = calls_client_get_call_log_finish (CALLS_CLIENT (source), result, NULL);

  if (entries && !g_cancellable_is_cancelled (self->recall))
    for (guint i = 0; i < entries->len; i++)
      {
        CallLogEntry *entry = entries->pdata[i];

        if (entry->type == CALL_TYPE_OUTGOING && *entry->number)
          {
            dialer_dialpad_set_digits (DIALER_DIALPAD (self->dialpad), entry->number);
            break;
          }
      }
  g_clear_object (&self->recall);
  g_object_unref (self);
}

static void
on_dial (DialpadSheet *self)
{
  const char *digits = dialer_dialpad_get_digits (DIALER_DIALPAD (self->dialpad));

  if (!*digits)
    {
      if (self->recall)
        return;
      self->recall = g_cancellable_new ();
      calls_client_get_call_log (calls_client_get (), self->recall, on_call_log, g_object_ref (self));
      return;
    }
  calls_client_dial (calls_client_get (), digits, on_dialed, g_object_ref (self));
}

static void
on_digits_changed (DialpadSheet *self)
{
  g_signal_emit (self, signals[DIGITS_CHANGED], 0);
}

/* the clear space over the dialpad: a touch there puts it away (onSearchListTouch) */
static void
on_space_pressed (GtkGestureClick *gesture, int n_press, double x, double y, DialpadSheet *self)
{
  graphene_rect_t bounds;

  if (gtk_widget_compute_bounds (self->dialpad, GTK_WIDGET (self), &bounds) && y < bounds.origin.y)
    {
      gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
      dialpad_sheet_hide (self);
    }
}

/* ---- layout --------------------------------------------------------------------------------------------------- */

static void
dialpad_sheet_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  DialpadSheet *self = DIALPAD_SHEET (widget);
  double slide = round ((1 - self->progress) * height);
  int min, nat;

  /* the dialpad at its own height at the foot; the dial button 16 dp above the foot, in front of it */
  gtk_widget_measure (self->dialpad, GTK_ORIENTATION_VERTICAL, width, &min, &nat, NULL, NULL);
  gtk_widget_allocate (self->dialpad, width, nat, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (0, height - nat + slide)));
  gtk_widget_allocate (self->dial, DIAL_SIZE, DIAL_SIZE, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT ((width - DIAL_SIZE) / 2,
                                                                            height - DIAL_MARGIN - DIAL_SIZE
                                                                            + slide)));
}

static void
dialpad_sheet_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                       int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 0;
}

/* ---- GObject -------------------------------------------------------------------------------------------------- */

static void
dialpad_sheet_dispose (GObject *object)
{
  DialpadSheet *self = DIALPAD_SHEET (object);
  GtkWidget *child;

  if (self->recall)
    g_cancellable_cancel (self->recall);
  g_clear_pointer (&self->slide, neo_anim_free);
  g_clear_pointer (&self->fab, neo_anim_free);
  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self))))
    gtk_widget_unparent (child);
  self->dialpad = self->dial = NULL;
  G_OBJECT_CLASS (dialpad_sheet_parent_class)->dispose (object);
}

static void
dialpad_sheet_class_init (DialpadSheetClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = dialpad_sheet_dispose;
  widget_class->measure = dialpad_sheet_measure;
  widget_class->size_allocate = dialpad_sheet_size_allocate;
  gtk_widget_class_set_css_name (widget_class, "dialpad-sheet");
  signals[SHOWN_CHANGED] = g_signal_new ("shown-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                         NULL, NULL, G_TYPE_NONE, 0);
  signals[DIGITS_CHANGED] = g_signal_new ("digits-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                          NULL, NULL, G_TYPE_NONE, 0);
}

/* DialpadFragment.onPressed: a key's tone sounds while it is held; the service knows whether "Keypad tones" is on */
static void
on_key_down (DialerDialpad *dialpad, int c, DialpadSheet *self)
{
  calls_client_play_tone (calls_client_get (), (char) c);
}

static void
on_key_up (DialerDialpad *dialpad, int c, DialpadSheet *self)
{
  calls_client_stop_tone (calls_client_get ());
}

static void
dialpad_sheet_init (DialpadSheet *self)
{
  GtkWidget *icon = gtk_image_new_from_icon_name ("dialer-call-symbolic");
  GtkGesture *space = gtk_gesture_click_new ();
  GdkRGBA green = rgb (DIAL_GREEN);

  self->dialpad = dialer_dialpad_new (DIALPAD_DIALER);
  gtk_widget_set_parent (self->dialpad, GTK_WIDGET (self));
  g_signal_connect_swapped (self->dialpad, "digits-changed", G_CALLBACK (on_digits_changed), self);
  g_signal_connect (self->dialpad, "key-down", G_CALLBACK (on_key_down), self);
  g_signal_connect (self->dialpad, "key-up", G_CALLBACK (on_key_up), self);

  /* dialpad_floating_action_button: #3ee0a0 with the black handset, elevation 8 dp */
  gtk_image_set_pixel_size (GTK_IMAGE (icon), 24);
  gtk_widget_add_css_class (icon, "dial-icon");
  self->dial = neo_pressable_new (icon);
  neo_pressable_set_fill (NEO_PRESSABLE (self->dial), &green, TRUE);
  neo_pressable_set_ripple (NEO_PRESSABLE (self->dial), NEO_RIPPLE_CIRCLE, DIAL_SIZE / 2.0);
  neo_pressable_set_scale (NEO_PRESSABLE (self->dial), 0);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->dial), GTK_ACCESSIBLE_PROPERTY_LABEL, _("dial"), -1);
  gtk_widget_set_parent (self->dial, GTK_WIDGET (self));
  g_signal_connect_swapped (self->dial, "clicked", G_CALLBACK (on_dial), self);

  g_signal_connect (space, "pressed", G_CALLBACK (on_space_pressed), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (space));
  self->slide = neo_anim_new (GTK_WIDGET (self), on_slide, self);
  self->fab = neo_anim_new (GTK_WIDGET (self), on_fab, self);
  gtk_widget_set_visible (GTK_WIDGET (self), FALSE);
}

GtkWidget *
dialpad_sheet_new (NeoSnackbar *snackbar)
{
  DialpadSheet *self = g_object_new (DIALPAD_TYPE_SHEET, NULL);

  self->snackbar = snackbar;
  return GTK_WIDGET (self);
}
