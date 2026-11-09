/* The time picker (cal-time-picker.h).
 *
 * Measured on still:cal-dlg-time (3 px a dp), from the card's top left: "Select time" 12 sp at (24, 16); the two
 * 96 × 80 dp boxes (8 dp corners) and the 24 dp colon at y 52, centred; the 256 dp face at (24, 168), #36343B; the
 * numbers 16 sp, their centres 100 dp from the face's centre (0–11, 00–55) and 66 dp (12–23); the selector a 48 dp
 * colorPrimary circle on a 2 dp hand, a dot at the centre; the buttons on the card's bottom edge, 8 dp from its end.
 * The chosen box is primaryContainer #4F378B under onPrimaryContainer, the other #36343B under #E6E0E9; the number
 * under the selector is #141218.
 *
 * Behaviour (TimePickerClockPresenter): a tap or a drag on the face moves the hand; letting go of the hour goes on
 * to the minute; a tap on a box chooses which. The hand turns in 200 ms (ClockHandView's animator). */
#include "cal-time-picker.h"
#include "cal-colors.h"
#include "cal-text.h"

#include <glib/gi18n.h>
#include <math.h>
#include <neo.h>

#define FACE 256.0
#define OUTER 100.0
#define INNER 66.0
#define SELECTOR 48.0
#define HAND_WIDTH 2.0
#define DOT_RADIUS 4.0
#define NUMBER_SIZE 16.0
#define TURN_MS 200
#define SELECTED_NUMBER 0xff141218

typedef struct {
  NeoDialog *dialog;
  GtkWidget *hour_box, *minute_box, *hour_label, *minute_label, *face;
  int hour, minute;
  gboolean minutes;                     /* the minute is being chosen */
  CalTimeChosen chosen;
  gpointer data;
} Picker;

/* --- the face */

#define CAL_TYPE_CLOCK_FACE (cal_clock_face_get_type ())
G_DECLARE_FINAL_TYPE (CalClockFace, cal_clock_face, CAL, CLOCK_FACE, GtkWidget)

struct _CalClockFace {
  GtkWidget parent;
  Picker *picker;
  double angle;                         /* the hand, in degrees clockwise from 12 o'clock */
  NeoAnim *turn;
  gboolean dragged;
};

G_DEFINE_FINAL_TYPE (CalClockFace, cal_clock_face, GTK_TYPE_WIDGET)

static double
value_angle (Picker *picker)
{
  return picker->minutes ? picker->minute * 6.0 : (picker->hour % 12) * 30.0;
}

static double
value_radius (Picker *picker)
{
  return !picker->minutes && picker->hour >= 12 ? INNER : OUTER;
}

static void
draw_numbers (CalClockFace *self, GtkSnapshot *snapshot, double cx, double cy, const GdkRGBA *color)
{
  Picker *picker = self->picker;
  double baseline, height;

  cal_text_line (NUMBER_SIZE, neo_device_scale (GTK_WIDGET (self)), &baseline, &height);
  for (int ring = 0; ring < (picker->minutes ? 1 : 2); ring++)
    for (int i = 0; i < 12; i++)
      {
        double radius = ring ? INNER : OUTER, a = i * 30.0 * G_PI / 180;
        g_autofree char *text = picker->minutes ? g_strdup_printf ("%02d", i * 5)
                                                : g_strdup_printf ("%d", i + ring * 12);
        g_autoptr(PangoLayout) layout = cal_text_layout (GTK_WIDGET (self), text, NUMBER_SIZE, 700);

        cal_text_draw (snapshot, layout, cx + radius * sin (a), cy - radius * cos (a) - height / 2 + baseline, 0.5,
                       color);
      }
}

static void
cal_clock_face_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CalClockFace *self = CAL_CLOCK_FACE (widget);
  double cx = gtk_widget_get_width (widget) / 2, cy = gtk_widget_get_height (widget) / 2;
  double radius = value_radius (self->picker), a = self->angle * G_PI / 180;
  double sx = cx + radius * sin (a), sy = cy - radius * cos (a);
  GdkRGBA face = cal_argb (CAL_SURFACE_CONTAINER_HIGHEST), primary = cal_argb (CAL_PRIMARY);
  GdkRGBA number = cal_argb (CAL_ON_SURFACE), selected = cal_argb (SELECTED_NUMBER);
  GskRoundedRect round;

  gsk_rounded_rect_init_from_rect (&round, &GRAPHENE_RECT_INIT (cx - FACE / 2, cy - FACE / 2, FACE, FACE), FACE / 2);
  gtk_snapshot_push_rounded_clip (snapshot, &round);
  gtk_snapshot_append_color (snapshot, &face, &round.bounds);
  gtk_snapshot_pop (snapshot);

  /* the hand, from the centre to the selector's, and the dot */
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (cx, cy));
  gtk_snapshot_rotate (snapshot, (float) self->angle);
  gtk_snapshot_append_color (snapshot, &primary,
                             &GRAPHENE_RECT_INIT (-HAND_WIDTH / 2, -radius, HAND_WIDTH, radius));
  gtk_snapshot_restore (snapshot);
  gsk_rounded_rect_init_from_rect (&round, &GRAPHENE_RECT_INIT (cx - DOT_RADIUS, cy - DOT_RADIUS, 2 * DOT_RADIUS,
                                                                2 * DOT_RADIUS), DOT_RADIUS);
  gtk_snapshot_push_rounded_clip (snapshot, &round);
  gtk_snapshot_append_color (snapshot, &primary, &round.bounds);
  gtk_snapshot_pop (snapshot);

  /* the selector, then the numbers: under the selector in its colour, as ClockFaceView clips them */
  gsk_rounded_rect_init_from_rect (&round, &GRAPHENE_RECT_INIT (sx - SELECTOR / 2, sy - SELECTOR / 2, SELECTOR,
                                                                SELECTOR), SELECTOR / 2);
  gtk_snapshot_push_rounded_clip (snapshot, &round);
  gtk_snapshot_append_color (snapshot, &primary, &round.bounds);
  gtk_snapshot_pop (snapshot);
  draw_numbers (self, snapshot, cx, cy, &number);
  gtk_snapshot_push_rounded_clip (snapshot, &round);
  draw_numbers (self, snapshot, cx, cy, &selected);
  gtk_snapshot_pop (snapshot);
}

static void
cal_clock_face_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                        int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = (int) FACE;
}

static void
on_turn (NeoAnim *anim, double value, gpointer data)
{
  CalClockFace *self = data;

  self->angle = value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* the hand to the value, the short way round */
static void
turn_to_value (CalClockFace *self, gboolean animate)
{
  double target = value_angle (self->picker), from = fmod (self->angle, 360);

  if (from < 0)
    from += 360;
  if (target - from > 180)
    from += 360;
  else if (from - target > 180)
    from -= 360;
  if (animate)
    neo_anim_start (self->turn, from, target, TURN_MS, 0, SPONGY_FAST_OUT_SLOW_IN);
  else
    {
      neo_anim_stop (self->turn);
      self->angle = target;
      gtk_widget_queue_draw (GTK_WIDGET (self));
    }
}

static void show_value (Picker *picker);

/* a point on the face: the value under it */
static void
pick (CalClockFace *self, double x, double y)
{
  Picker *picker = self->picker;
  double dx = x - gtk_widget_get_width (GTK_WIDGET (self)) / 2, dy = y - gtk_widget_get_height (GTK_WIDGET (self)) / 2;
  double degrees = atan2 (dx, -dy) * 180 / G_PI;

  if (degrees < 0)
    degrees += 360;
  if (picker->minutes)
    picker->minute = ((int) round (degrees / 6)) % 60;
  else
    {
      int index = ((int) round (degrees / 30)) % 12;

      /* the inner ring is 12–23 */
      picker->hour = hypot (dx, dy) < (OUTER + INNER) / 2 ? index + 12 : index;
    }
  show_value (picker);
}

static void set_minutes (Picker *picker, gboolean minutes);

static void
on_drag_begin (GtkGestureDrag *drag, double x, double y, CalClockFace *self)
{
  self->dragged = FALSE;
  pick (self, x, y);
  turn_to_value (self, TRUE);
}

static void
on_drag_update (GtkGestureDrag *drag, double dx, double dy, CalClockFace *self)
{
  double x, y;

  if (!self->dragged && hypot (dx, dy) < 8)
    return;
  self->dragged = TRUE;
  gtk_gesture_drag_get_start_point (drag, &x, &y);
  pick (self, x + dx, y + dy);
  turn_to_value (self, FALSE);
}

static void
on_drag_end (GtkGestureDrag *drag, double dx, double dy, CalClockFace *self)
{
  /* onActionUp: the hour chosen, the minute next */
  if (!self->picker->minutes)
    set_minutes (self->picker, TRUE);
}

static void
cal_clock_face_dispose (GObject *object)
{
  CalClockFace *self = CAL_CLOCK_FACE (object);

  g_clear_pointer (&self->turn, neo_anim_free);
  G_OBJECT_CLASS (cal_clock_face_parent_class)->dispose (object);
}

static void
cal_clock_face_class_init (CalClockFaceClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = cal_clock_face_dispose;
  GTK_WIDGET_CLASS (klass)->snapshot = cal_clock_face_snapshot;
  GTK_WIDGET_CLASS (klass)->measure = cal_clock_face_measure;
}

static void
cal_clock_face_init (CalClockFace *self)
{
  GtkGesture *drag = gtk_gesture_drag_new ();

  self->turn = neo_anim_new (GTK_WIDGET (self), on_turn, self);
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_drag_end), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));
}

/* --- the dialog */

static void
show_value (Picker *picker)
{
  g_autofree char *hour = g_strdup_printf ("%02d", picker->hour);
  g_autofree char *minute = g_strdup_printf ("%02d", picker->minute);

  gtk_label_set_label (GTK_LABEL (picker->hour_label), hour);
  gtk_label_set_label (GTK_LABEL (picker->minute_label), minute);
  gtk_widget_queue_draw (picker->face);
}

static void
set_minutes (Picker *picker, gboolean minutes)
{
  picker->minutes = minutes;
  if (minutes)
    {
      gtk_widget_remove_css_class (picker->hour_box, "selected");
      gtk_widget_add_css_class (picker->minute_box, "selected");
    }
  else
    {
      gtk_widget_add_css_class (picker->hour_box, "selected");
      gtk_widget_remove_css_class (picker->minute_box, "selected");
    }
  turn_to_value (CAL_CLOCK_FACE (picker->face), TRUE);
  gtk_widget_queue_draw (picker->face);
}

static void
on_hour_box (NeoPressable *box, Picker *picker)
{
  set_minutes (picker, FALSE);
}

static void
on_minute_box (NeoPressable *box, Picker *picker)
{
  set_minutes (picker, TRUE);
}

static GtkWidget *
time_box (Picker *picker, GtkWidget **label, const char *name, GCallback callback)
{
  GtkWidget *box;

  *label = gtk_label_new (NULL);
  gtk_widget_add_css_class (*label, "cal-time-text");
  box = neo_pressable_new (*label);
  gtk_widget_add_css_class (box, "cal-time-box");
  neo_pressable_set_ripple (NEO_PRESSABLE (box), NEO_RIPPLE_BOUNDED, 8);
  gtk_widget_set_size_request (box, 96, 80);
  gtk_accessible_update_property (GTK_ACCESSIBLE (box), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, name, -1);
  g_signal_connect (box, "clicked", callback, picker);
  return box;
}

static void
on_response (NeoDialog *dialog, int response, Picker *picker)
{
  if (response == NEO_RESPONSE_OK)
    picker->chosen (picker->hour, picker->minute, picker->data);
}

static void
picker_free (gpointer data, GClosure *closure)
{
  g_free (data);
}

void
cal_time_picker_show (GtkWidget *parent, int hour, int minute, CalTimeChosen chosen, gpointer data)
{
  Picker *picker = g_new0 (Picker, 1);
  NeoDialog *dialog = neo_dialog_new (NULL);
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *title = gtk_label_new (_("Select time"));
  GtkWidget *display = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *colon = gtk_label_new (":");
  CalClockFace *face;

  picker->dialog = dialog;
  picker->hour = CLAMP (hour, 0, 23);
  picker->minute = CLAMP (minute, 0, 59);
  picker->chosen = chosen;
  picker->data = data;

  gtk_label_set_xalign (GTK_LABEL (title), 0);
  neo_label_set_font_padding (title, 12);
  gtk_widget_add_css_class (title, "cal-time-title");
  gtk_box_append (GTK_BOX (content), title);

  picker->hour_box = time_box (picker, &picker->hour_label, _("Hour"), G_CALLBACK (on_hour_box));
  picker->minute_box = time_box (picker, &picker->minute_label, _("Minute"), G_CALLBACK (on_minute_box));
  gtk_widget_add_css_class (colon, "cal-time-colon");
  gtk_widget_set_size_request (colon, 24, -1);
  gtk_box_append (GTK_BOX (display), picker->hour_box);
  gtk_box_append (GTK_BOX (display), colon);
  gtk_box_append (GTK_BOX (display), picker->minute_box);
  gtk_widget_set_halign (display, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (display, "cal-time-display");
  gtk_box_append (GTK_BOX (content), display);

  face = g_object_new (CAL_TYPE_CLOCK_FACE, NULL);
  face->picker = picker;
  picker->face = GTK_WIDGET (face);
  gtk_widget_set_halign (picker->face, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (picker->face, "cal-time-face");
  gtk_accessible_update_property (GTK_ACCESSIBLE (picker->face), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Clock face"), -1);
  gtk_box_append (GTK_BOX (content), picker->face);

  gtk_widget_set_size_request (content, 304, -1);
  gtk_widget_add_css_class (content, "cal-time-picker");
  neo_dialog_set_flush (dialog, TRUE);
  neo_dialog_set_content (dialog, content);
  gtk_widget_add_css_class (gtk_widget_get_parent (content), "cal-time-card");
  neo_dialog_add_button (dialog, _("Cancel"), NEO_RESPONSE_CANCEL);
  neo_dialog_add_button (dialog, _("OK"), NEO_RESPONSE_OK);
  g_signal_connect_data (dialog, "response", G_CALLBACK (on_response), picker, picker_free, 0);
  show_value (picker);
  set_minutes (picker, FALSE);
  turn_to_value (face, FALSE);
  neo_dialog_present (dialog, parent);
}
