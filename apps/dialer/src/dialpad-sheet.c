/* The main screen's dialpad: see dialpad-sheet.h. */
#include "dialpad-sheet.h"
#include "call-log.h"
#include "call-types.h"
#include "calls-client.h"
#include "dialpad.h"
#include "phone.h"
#include "popup-menu.h"

#include <glib/gi18n.h>
#include <math.h>
#include <string.h>

#define DIAL_SIZE       56      /* the dial FAB */
#define DIAL_MARGIN     16      /* above the bottom */
#define SLIDE_MS        400     /* the sheet sliding in and out */
#define FAB_MS          250     /* the dial button growing in and shrinking out */
#define DIAL_GREEN      VIBEOS_CALL     /* libvibeos's fixed call green, light and dark */

struct _DialpadSheet
{
  GtkWidget parent;
  GtkWidget *dialpad, *dial;
  VibeosSnackbar *snackbar;
  gboolean shown;
  VibeosAnim *slide, *fab;
  double progress;              /* 0 down … 1 up */
  double fab_scale;
  GCancellable *recall;         /* looking up the last number called */
};

G_DEFINE_FINAL_TYPE (DialpadSheet, dialpad_sheet, GTK_TYPE_WIDGET)

enum { SHOWN_CHANGED, DIGITS_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* ---- the motion ----------------------------------------------------------------------------------------------- */

static void
on_fab (VibeosAnim *anim, double value, gpointer data)
{
  DialpadSheet *self = data;

  self->fab_scale = value;
  vibeos_pressable_set_scale (VIBEOS_PRESSABLE (self->dial), value);
}

static void
on_slide (VibeosAnim *anim, double value, gpointer data)
{
  DialpadSheet *self = data;

  self->progress = value;
  if (!vibeos_anim_is_running (anim))
    {
      /* the dial button scales in once the dialpad is up; gone, the sheet stops taking touches */
      if (value == 1)
        vibeos_anim_start (self->fab, self->fab_scale, 1, FAB_MS, 0, SPONGY_FAST_OUT_SLOW_IN);
      else if (value == 0)
        gtk_widget_set_visible (GTK_WIDGET (self), FALSE);
    }
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

/* slide up from its full height below to 0 over 400 ms (0, 0, 0.2, 1) */
void
dialpad_sheet_show (DialpadSheet *self, const char *number)
{
  if (number)
    dialer_dialpad_set_digits (DIALER_DIALPAD (self->dialpad), number);
  if (self->shown)
    return;
  self->shown = TRUE;
  calls_client_prepare_tones (calls_client_get ());
  gtk_widget_set_visible (GTK_WIDGET (self), TRUE);
  vibeos_anim_start_cubic (self->slide, self->progress, 1, SLIDE_MS, 0, 0, 0, 0.2, 1);
  g_signal_emit (self, signals[SHOWN_CHANGED], 0);
}

/* hiding: the dial button scales out as the dialpad slides down, 0 → 100 %p over 400 ms (0.4, 0, 1, 1) */
void
dialpad_sheet_hide (DialpadSheet *self)
{
  if (!self->shown)
    return;
  self->shown = FALSE;
  calls_client_release_tones (calls_client_get ());
  vibeos_anim_start (self->fab, self->fab_scale, 0, FAB_MS, 0, SPONGY_FAST_OUT_SLOW_IN);
  vibeos_anim_start_cubic (self->slide, self->progress, 0, SLIDE_MS, 0, 0.4, 0, 1, 1);
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
    vibeos_snackbar_show (self->snackbar, error->message, NULL, NULL, NULL, VIBEOS_SNACKBAR_LONG);
  else
    {
      /* the call is the service's now: its screen comes up, and the dialpad goes away empty */
      phone_start_app (INCALL_APP_ID);
      dialer_dialpad_set_digits (DIALER_DIALPAD (self->dialpad), "");
      dialpad_sheet_hide (self);
    }
  g_object_unref (self);
}

/* the dial button with no digits: the last number called comes back into the field */
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

/* the clear space over the dialpad: a touch there puts it away */
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

  /* the dialpad at its own height at the foot; the dial button DIAL_MARGIN above the foot, in front of it */
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
  g_clear_pointer (&self->slide, vibeos_anim_free);
  g_clear_pointer (&self->fab, vibeos_anim_free);
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

/* a key's tone sounds while it is held; the service knows whether "Keypad tones" is on */
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

/* ---- ⋮ --------------------------------------------------------------------------------------------------------- */

/* "," or ";" where the cursor is, which here is always the end of the digits, where one may go: never first, and no
 * wait next to a wait. The call service sends what follows them once the call is up (calls.c, post-dial) */
static void
add_special (DialpadSheet *self, char c)
{
  const char *digits = dialer_dialpad_get_digits (DIALER_DIALPAD (self->dialpad));
  size_t n = strlen (digits);

  if (n == 0 || (c == ';' && digits[n - 1] == ';'))
    return;
  dialer_dialpad_append (DIALER_DIALPAD (self->dialpad), c);
}

static void
add_pause (gpointer data)
{
  add_special (data, ',');
}

static void
add_wait (gpointer data)
{
  add_special (data, ';');
}

/* the dialpad's ⋮: "Call via" and "Call with a note" stay hidden (one account; no call subject over a modem) */
static void
on_overflow (DialerDialpad *dialpad, GtkWidget *anchor, DialpadSheet *self)
{
  DialerPopupMenu *menu = dialer_popup_menu_new ();
  gboolean enable = *dialer_dialpad_get_digits (dialpad) != '\0';

  dialer_popup_menu_append (menu, _("Add 2-sec pause"), enable ? add_pause : NULL, self);
  dialer_popup_menu_append (menu, _("Add wait"), enable ? add_wait : NULL, self);
  dialer_popup_menu_present (menu, anchor, POPUP_MENU_BELOW);
}

static void
dialpad_sheet_init (DialpadSheet *self)
{
  GtkWidget *icon = gtk_image_new_from_icon_name ("dialer-call-symbolic");
  GtkGesture *space = gtk_gesture_click_new ();

  self->dialpad = dialer_dialpad_new (DIALPAD_DIALER);
  gtk_widget_set_parent (self->dialpad, GTK_WIDGET (self));
  g_signal_connect_swapped (self->dialpad, "digits-changed", G_CALLBACK (on_digits_changed), self);
  g_signal_connect (self->dialpad, "key-down", G_CALLBACK (on_key_down), self);
  g_signal_connect (self->dialpad, "key-up", G_CALLBACK (on_key_up), self);
  g_signal_connect (self->dialpad, "overflow", G_CALLBACK (on_overflow), self);

  /* the dial button: green with a dark handset, its shadow under it */
  gtk_image_set_pixel_size (GTK_IMAGE (icon), 24);
  gtk_widget_add_css_class (icon, "dial-icon");
  self->dial = vibeos_pressable_new (icon);
  vibeos_pressable_set_fill_role (VIBEOS_PRESSABLE (self->dial), DIAL_GREEN, TRUE);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (self->dial), VIBEOS_RIPPLE_CIRCLE, DIAL_SIZE / 2.0);
  vibeos_pressable_set_scale (VIBEOS_PRESSABLE (self->dial), 0);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->dial), GTK_ACCESSIBLE_PROPERTY_LABEL, _("dial"), -1);
  gtk_widget_set_parent (self->dial, GTK_WIDGET (self));
  g_signal_connect_swapped (self->dial, "clicked", G_CALLBACK (on_dial), self);

  g_signal_connect (space, "pressed", G_CALLBACK (on_space_pressed), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (space));
  self->slide = vibeos_anim_new (GTK_WIDGET (self), on_slide, self);
  self->fab = vibeos_anim_new (GTK_WIDGET (self), on_fab, self);
  gtk_widget_set_visible (GTK_WIDGET (self), FALSE);
}

GtkWidget *
dialpad_sheet_new (VibeosSnackbar *snackbar)
{
  DialpadSheet *self = g_object_new (DIALPAD_TYPE_SHEET, NULL);

  self->snackbar = snackbar;
  return GTK_WIDGET (self);
}
