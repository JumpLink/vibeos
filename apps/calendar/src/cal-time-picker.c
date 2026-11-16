/* The time picker (cal-time-picker.h).
 *
 * From the card's top: "Select time"; the hour and minute boxes with the colon, centred; the clock face, its numbers
 * 0–11 (00–55) on the outer ring and 12–23 on the inner; the selector a circle on a thin hand, a dot at the centre;
 * the buttons on the card's bottom edge, the keyboard button at its start, a little higher than Cancel and OK. The
 * box being chosen is filled in the primary container colour.
 *
 * Behaviour: a tap or a drag on the face moves the hand; letting go of the hour goes on to the minute; a tap on a box
 * chooses which. The hand turns in 200 ms.
 *
 * The 12-hour clock (when the phone's clock is 12-hour): one ring, 12 1 … 11, and the AM/PM toggle right of the boxes
 * (two outlined buttons, the checked one filled); the card is wider for it.
 *
 * The keyboard input mode: the keyboard button swaps the boxes and the face for two fields, "Hour" and "Minute" under
 * them; the card wraps them. The one being typed is a field, its outline in the primary colour while focused; the other
 * a box as above, its tap moves the typing there. At most two digits, at most 23 (12 for a 12-hour clock) and 59; an
 * empty field is 0. From a keyboard: a digit on a full hour goes on to the minute, Backspace on an empty minute back to
 * the hour, Enter on the hour to the minute. The clock button goes back to the face. */
#include "cal-time-picker.h"
#include "cal-colors.h"
#include "cal-format.h"
#include "cal-text.h"

#include <glib/gi18n.h>
#include <math.h>
#include <vibeos.h>
#include <stdlib.h>
#include <string.h>

#define FACE 256.0
#define OUTER 100.0
#define INNER 66.0
#define SELECTOR 48.0
#define HAND_WIDTH 2.0
#define DOT_RADIUS 4.0
#define NUMBER_SIZE 16.0
#define TURN_MS 200
#define SELECTED_NUMBER (cal_palette->selected_number)
#define BOX_WIDTH 96
#define BOX_HEIGHT 80
#define FIELD_CORNER 8.0
#define CARD_CLOCK_24H 304              /* the face and its sides */
#define CARD_12H 328                    /* the boxes, the AM/PM toggle and the sides */
#define CARD_KEYBOARD_24H 264           /* the two fields, the colon and the sides */

typedef struct {
  VibeosDialog *dialog;
  GtkWidget *content, *display, *hour_box, *minute_box, *hour_label, *minute_label, *face, *period;
  GtkWidget *mode, *input, *input_period, *hour_stack, *minute_stack, *hour_chip, *minute_chip;
  GtkWidget *hour_chip_label, *minute_chip_label, *hour_field, *minute_field;
  int hour, minute;
  gboolean minutes;                     /* the minute is being chosen */
  gboolean use_24h;
  gboolean keyboard;                    /* the keyboard input mode */
  gboolean setting_text;
  CalTimeChosen chosen;
  gpointer data;
} Picker;

static gboolean
is_pm (Picker *picker)
{
  return picker->hour >= 12;
}

/* the hour shown: 12 1 … 11 on a 12-hour clock */
static int
display_hour (Picker *picker)
{
  if (picker->use_24h)
    return picker->hour;
  return picker->hour % 12 == 0 ? 12 : picker->hour % 12;
}

/* --- the face */

#define CAL_TYPE_CLOCK_FACE (cal_clock_face_get_type ())
G_DECLARE_FINAL_TYPE (CalClockFace, cal_clock_face, CAL, CLOCK_FACE, GtkWidget)

struct _CalClockFace {
  GtkWidget parent;
  Picker *picker;
  double angle;                         /* the hand, in degrees clockwise from 12 o'clock */
  VibeosAnim *turn;
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
  return !picker->minutes && picker->use_24h && picker->hour >= 12 ? INNER : OUTER;
}

static void
draw_numbers (CalClockFace *self, GtkSnapshot *snapshot, double cx, double cy, const GdkRGBA *color)
{
  Picker *picker = self->picker;
  double baseline, height;

  cal_text_line (NUMBER_SIZE, vibeos_device_scale (GTK_WIDGET (self)), &baseline, &height);
  for (int ring = 0; ring < (picker->minutes || !picker->use_24h ? 1 : 2); ring++)
    for (int i = 0; i < 12; i++)
      {
        double radius = ring ? INNER : OUTER, a = i * 30.0 * G_PI / 180;
        /* the numbers: 00–55; 0–11 and 12–23; 12 1 … 11 on a 12-hour clock */
        g_autofree char *text = picker->minutes ? g_strdup_printf ("%02d", i * 5)
                                : picker->use_24h ? g_strdup_printf ("%d", i + ring * 12)
                                                  : g_strdup_printf ("%d", i ? i : 12);
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

  /* the selector, then the numbers: under the selector in its colour */
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
on_turn (VibeosAnim *anim, double value, gpointer data)
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
    vibeos_anim_start (self->turn, from, target, TURN_MS, 0, SPONGY_FAST_OUT_SLOW_IN);
  else
    {
      vibeos_anim_stop (self->turn);
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

      /* the inner ring is 12–23; a 12-hour clock keeps AM or PM */
      if (picker->use_24h)
        picker->hour = hypot (dx, dy) < (OUTER + INNER) / 2 ? index + 12 : index;
      else
        picker->hour = index + (is_pm (picker) ? 12 : 0);
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
  /* let go: the hour chosen, the minute next */
  if (!self->picker->minutes)
    set_minutes (self->picker, TRUE);
}

static void
cal_clock_face_dispose (GObject *object)
{
  CalClockFace *self = CAL_CLOCK_FACE (object);

  g_clear_pointer (&self->turn, vibeos_anim_free);
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

  self->turn = vibeos_anim_new (GTK_WIDGET (self), on_turn, self);
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_drag_end), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));
}

/* --- a field of the keyboard mode: two digits, centred, in an outlined box (no fill, the outline in the primary
 * colour while focused) */

#define CAL_TYPE_TIME_FIELD (cal_time_field_get_type ())
G_DECLARE_FINAL_TYPE (CalTimeField, cal_time_field, CAL, TIME_FIELD, GtkWidget)

struct _CalTimeField {
  GtkWidget parent;
  GtkWidget *text;
};

G_DEFINE_FINAL_TYPE (CalTimeField, cal_time_field, GTK_TYPE_WIDGET)

static void
cal_time_field_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                        int *min_baseline, int *nat_baseline)
{
  *min = *nat = orientation == GTK_ORIENTATION_HORIZONTAL ? BOX_WIDTH : BOX_HEIGHT;
}

static void
cal_time_field_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  CalTimeField *self = CAL_TIME_FIELD (widget);
  int text_height, ignored;

  gtk_widget_measure (self->text, GTK_ORIENTATION_VERTICAL, -1, &ignored, &text_height, NULL, NULL);
  gtk_widget_size_allocate (self->text, &(GtkAllocation) { 0, (height - text_height) / 2, width, text_height }, -1);
}

static void
cal_time_field_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CalTimeField *self = CAL_TIME_FIELD (widget);
  gboolean focused = gtk_widget_has_focus (self->text);
  float stroke = focused ? 2 : 1, w = (float) gtk_widget_get_width (widget), h = (float) gtk_widget_get_height (widget);
  GdkRGBA color = cal_argb (focused ? CAL_PRIMARY : CAL_OUTLINE);
  GdkRGBA colors[4] = { color, color, color, color };
  float widths[4] = { stroke, stroke, stroke, stroke };
  GskRoundedRect round;

  gsk_rounded_rect_init_from_rect (&round, &GRAPHENE_RECT_INIT (0, 0, w, h), FIELD_CORNER);
  gtk_snapshot_append_border (snapshot, &round, widths, colors);
  gtk_widget_snapshot_child (widget, self->text, snapshot);
}

static void
cal_time_field_dispose (GObject *object)
{
  CalTimeField *self = CAL_TIME_FIELD (object);

  g_clear_pointer (&self->text, gtk_widget_unparent);
  G_OBJECT_CLASS (cal_time_field_parent_class)->dispose (object);
}

static void
cal_time_field_class_init (CalTimeFieldClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = cal_time_field_dispose;
  widget_class->measure = cal_time_field_measure;
  widget_class->size_allocate = cal_time_field_allocate;
  widget_class->snapshot = cal_time_field_snapshot;
}

static void
on_time_field_focus (GtkEventControllerFocus *focus, CalTimeField *self)
{
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
cal_time_field_init (CalTimeField *self)
{
  GtkEventController *focus = gtk_event_controller_focus_new ();

  gtk_widget_add_css_class (GTK_WIDGET (self), "cal-time-field");
  self->text = gtk_text_new ();
  gtk_text_set_max_length (GTK_TEXT (self->text), 2);
  gtk_text_set_input_purpose (GTK_TEXT (self->text), GTK_INPUT_PURPOSE_DIGITS);
  gtk_editable_set_alignment (GTK_EDITABLE (self->text), 0.5);
  gtk_widget_set_parent (self->text, GTK_WIDGET (self));
  g_signal_connect (focus, "enter", G_CALLBACK (on_time_field_focus), self);
  g_signal_connect (focus, "leave", G_CALLBACK (on_time_field_focus), self);
  gtk_widget_add_controller (self->text, focus);
}

static GtkWidget *
field_text (GtkWidget *field)
{
  return CAL_TIME_FIELD (field)->text;
}

/* --- the dialog */

static void
show_value (Picker *picker)
{
  g_autofree char *hour = g_strdup_printf ("%02d", display_hour (picker));
  g_autofree char *minute = g_strdup_printf ("%02d", picker->minute);

  gtk_label_set_label (GTK_LABEL (picker->hour_label), hour);
  gtk_label_set_label (GTK_LABEL (picker->minute_label), minute);
  gtk_widget_queue_draw (picker->face);
  if (!picker->use_24h)
    for (GtkWidget *toggle = picker->period; toggle; toggle = toggle == picker->period ? picker->input_period : NULL)
      {
        GtkWidget *am = gtk_widget_get_first_child (toggle), *pm = gtk_widget_get_last_child (toggle);

        if (is_pm (picker))
          {
            gtk_widget_remove_css_class (am, "checked");
            gtk_widget_add_css_class (pm, "checked");
          }
        else
          {
            gtk_widget_add_css_class (am, "checked");
            gtk_widget_remove_css_class (pm, "checked");
          }
      }
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
on_hour_box (VibeosPressable *box, Picker *picker)
{
  set_minutes (picker, FALSE);
}

static void
on_minute_box (VibeosPressable *box, Picker *picker)
{
  set_minutes (picker, TRUE);
}

static GtkWidget *
time_box (Picker *picker, GtkWidget **label, const char *name, GCallback callback)
{
  GtkWidget *box;

  *label = gtk_label_new (NULL);
  gtk_widget_add_css_class (*label, "cal-time-text");
  box = vibeos_pressable_new (*label);
  gtk_widget_add_css_class (box, "cal-time-box");
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (box), VIBEOS_RIPPLE_BOUNDED, 8);
  gtk_widget_set_size_request (box, BOX_WIDTH, BOX_HEIGHT);
  gtk_accessible_update_property (GTK_ACCESSIBLE (box), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, name, -1);
  g_signal_connect (box, "clicked", callback, picker);
  return box;
}

/* --- AM and PM (a toggle group, one always checked) */

static void sync_fields (Picker *picker);

static void
on_period (VibeosPressable *button, Picker *picker)
{
  gboolean pm = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "cal-pm"));

  /* AM or PM: the hour moved by 12 */
  if (pm && picker->hour < 12)
    picker->hour += 12;
  else if (!pm && picker->hour >= 12)
    picker->hour -= 12;
  show_value (picker);
  turn_to_value (CAL_CLOCK_FACE (picker->face), FALSE);
}

static GtkWidget *
period_toggle (Picker *picker, GtkWidget *row, const char *place_class)
{
  GtkWidget *group = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *place = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  gtk_widget_add_css_class (group, "cal-time-period");
  gtk_widget_set_overflow (group, GTK_OVERFLOW_HIDDEN);
  gtk_box_set_homogeneous (GTK_BOX (group), TRUE);
  gtk_widget_set_size_request (group, 52, BOX_HEIGHT);
  gtk_widget_set_valign (group, GTK_ALIGN_START);
  gtk_accessible_update_property (GTK_ACCESSIBLE (group), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Select AM or PM"), -1);
  for (int pm = 0; pm < 2; pm++)
    {
      GtkWidget *button = vibeos_pressable_new (gtk_label_new (pm ? _("PM") : _("AM")));

      gtk_widget_add_css_class (button, "cal-time-period-button");
      vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (button), VIBEOS_RIPPLE_BOUNDED, 0);
      gtk_widget_set_vexpand (button, TRUE);
      g_object_set_data (G_OBJECT (button), "cal-pm", GINT_TO_POINTER (pm));
      g_signal_connect (button, "clicked", G_CALLBACK (on_period), picker);
      gtk_box_append (GTK_BOX (group), button);
    }
  /* the margins on a box round it: a size request counts the widget's own CSS margins */
  gtk_widget_add_css_class (place, place_class);
  gtk_widget_set_valign (place, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (place), group);
  gtk_box_append (GTK_BOX (row), place);
  return group;
}

/* --- the keyboard input mode */

/* the field being typed and the box for the other */
static void
check_field (Picker *picker, gboolean minutes)
{
  picker->minutes = minutes;
  gtk_stack_set_visible_child (GTK_STACK (picker->hour_stack), minutes ? picker->hour_chip : picker->hour_field);
  gtk_stack_set_visible_child (GTK_STACK (picker->minute_stack), minutes ? picker->minute_field : picker->minute_chip);
  /* focused: the field keeps its cursor, nothing selected */
  gtk_text_grab_focus_without_selecting (GTK_TEXT (field_text (minutes ? picker->minute_field : picker->hour_field)));
}

/* both fields and both boxes from the time */
static void
sync_fields (Picker *picker)
{
  g_autofree char *hour = g_strdup_printf ("%02d", display_hour (picker));
  g_autofree char *minute = g_strdup_printf ("%02d", picker->minute);

  picker->setting_text = TRUE;
  gtk_editable_set_text (GTK_EDITABLE (field_text (picker->hour_field)), hour);
  gtk_editable_set_text (GTK_EDITABLE (field_text (picker->minute_field)), minute);
  /* the cursor at the start, where the first digit typed clears the prefilled value */
  gtk_editable_set_position (GTK_EDITABLE (field_text (picker->hour_field)), 0);
  gtk_editable_set_position (GTK_EDITABLE (field_text (picker->minute_field)), 0);
  picker->setting_text = FALSE;
  gtk_label_set_label (GTK_LABEL (picker->hour_chip_label), hour);
  gtk_label_set_label (GTK_LABEL (picker->minute_chip_label), minute);
}

/* the hour or the minute typed: an empty field is 0; the other box shows it as two digits */
static void
on_field_changed (GtkEditable *editable, Picker *picker)
{
  gboolean minutes = editable == GTK_EDITABLE (field_text (picker->minute_field));
  const char *text = gtk_editable_get_text (editable);
  int value = *text ? atoi (text) : 0;
  g_autofree char *chip = g_strdup_printf ("%02d", value);

  if (picker->setting_text)
    return;
  gtk_label_set_label (GTK_LABEL (minutes ? picker->minute_chip_label : picker->hour_chip_label), chip);
  if (minutes)
    picker->minute = value;
  else
    /* a 12-hour clock keeps AM or PM */
    picker->hour = picker->use_24h ? value : value % 12 + (is_pm (picker) ? 12 : 0);
}

/* digits only, and what they make no more than the most */
static void
on_field_insert (GtkEditable *editable, const char *text, int length, int *position, Picker *picker)
{
  gboolean minutes = editable == GTK_EDITABLE (field_text (picker->minute_field));
  const char *old = gtk_editable_get_text (editable);
  int most = minutes ? 59 : picker->use_24h ? 23 : 12;
  GString *result = g_string_new (NULL);
  gsize at = (gsize) MIN ((int) strlen (old), *position);
  gboolean ok = TRUE;

  for (const char *p = text; length < 0 ? *p : p < text + length; p++)
    ok = ok && g_ascii_isdigit (*p);
  g_string_append_len (result, old, (gssize) at);
  g_string_append_len (result, text, length);
  g_string_append (result, old + at);
  if (!ok || (result->len > 0 && atoi (result->str) > most))
    g_signal_stop_emission_by_name (editable, "insert-text");
  g_string_free (result, TRUE);
}

/* a key on a hardware keyboard: typing over a field that is full starts
 * it again, a digit after a full hour goes to the minute, Backspace in an empty minute back to the hour */
static gboolean
on_field_key (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, Picker *picker)
{
  GtkWidget *text = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (keys));
  gboolean minutes = text == field_text (picker->minute_field);
  const char *value = gtk_editable_get_text (GTK_EDITABLE (text));
  int position = gtk_editable_get_position (GTK_EDITABLE (text)), length = (int) strlen (value);
  gboolean digit = (keyval >= GDK_KEY_0 && keyval <= GDK_KEY_9) || (keyval >= GDK_KEY_KP_0 && keyval <= GDK_KEY_KP_9);

  if (minutes && keyval == GDK_KEY_BackSpace && length == 0)
    {
      check_field (picker, FALSE);
      return TRUE;
    }
  if (!minutes && digit && position == 2 && length == 2)
    {
      check_field (picker, TRUE);
      return TRUE;
    }
  /* the first digit typed over a full field clears it */
  if (position == 0 && length == 2)
    gtk_editable_set_text (GTK_EDITABLE (text), "");
  return FALSE;
}

/* Enter on the hour: on to the minute */
static void
on_field_activate (GtkText *text, Picker *picker)
{
  if (GTK_WIDGET (text) == field_text (picker->hour_field))
    check_field (picker, TRUE);
}

static void
on_hour_chip (VibeosPressable *box, Picker *picker)
{
  check_field (picker, FALSE);
}

static void
on_minute_chip (VibeosPressable *box, Picker *picker)
{
  check_field (picker, TRUE);
}

/* one input: the field or the box over the label "Hour" / "Minute" */
static GtkWidget *
combo (Picker *picker, gboolean minutes)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *stack = gtk_stack_new ();
  GtkWidget *field = g_object_new (CAL_TYPE_TIME_FIELD, NULL);
  GtkWidget *text = field_text (field);
  GtkWidget *label = gtk_label_new (minutes ? _("Minute") : _("Hour"));
  GtkWidget *chip_label;
  GtkWidget *chip = time_box (picker, &chip_label, minutes ? _("Minute") : _("Hour"),
                              minutes ? G_CALLBACK (on_minute_chip) : G_CALLBACK (on_hour_chip));
  GtkEventController *keys = gtk_event_controller_key_new ();

  gtk_stack_add_child (GTK_STACK (stack), chip);
  gtk_stack_add_child (GTK_STACK (stack), field);
  gtk_box_append (GTK_BOX (box), stack);
  gtk_accessible_update_property (GTK_ACCESSIBLE (text), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  minutes ? _("Minute") : _("Hour"), -1);
  g_signal_connect (text, "changed", G_CALLBACK (on_field_changed), picker);
  g_signal_connect (text, "insert-text", G_CALLBACK (on_field_insert), picker);
  g_signal_connect (text, "activate", G_CALLBACK (on_field_activate), picker);
  gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_field_key), picker);
  gtk_widget_add_controller (text, keys);

  gtk_label_set_xalign (GTK_LABEL (label), 0);
  vibeos_label_set_font_padding (label, 12);
  gtk_widget_add_css_class (label, "cal-time-input-label");
  gtk_box_append (GTK_BOX (box), label);
  if (minutes)
    {
      picker->minute_stack = stack;
      picker->minute_field = field;
      picker->minute_chip = chip;
      picker->minute_chip_label = chip_label;
    }
  else
    {
      picker->hour_stack = stack;
      picker->hour_field = field;
      picker->hour_chip = chip;
      picker->hour_chip_label = chip_label;
    }
  return box;
}

/* the keyboard mode: the two fields with the colon, AM/PM on a 12-hour clock */
static GtkWidget *
keyboard_input (Picker *picker)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *display = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *colon = gtk_label_new (":");

  gtk_widget_add_css_class (row, "cal-time-input");
  gtk_widget_set_halign (row, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (display, "cal-time-input-display");
  gtk_box_append (GTK_BOX (display), combo (picker, FALSE));
  gtk_widget_add_css_class (colon, "cal-time-colon");
  gtk_widget_set_size_request (colon, 24, BOX_HEIGHT);
  gtk_widget_set_valign (colon, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (display), colon);
  gtk_box_append (GTK_BOX (display), combo (picker, TRUE));
  gtk_box_append (GTK_BOX (row), display);
  if (!picker->use_24h)
    {
      picker->input_period = period_toggle (picker, row, "cal-time-input-period");
    }
  gtk_widget_set_visible (row, FALSE);
  return row;
}

/* the keyboard and the clock button */
static void
on_mode (VibeosPressable *button, Picker *picker)
{
  GtkWidget *image = vibeos_pressable_get_child (button);

  picker->keyboard = !picker->keyboard;
  gtk_widget_set_visible (picker->display, !picker->keyboard);
  gtk_widget_set_visible (picker->face, !picker->keyboard);
  gtk_widget_set_visible (picker->input, picker->keyboard);
  gtk_image_set_from_icon_name (GTK_IMAGE (image), picker->keyboard ? "cal-schedule-symbolic" : "cal-keyboard-symbolic");
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  picker->keyboard ? _("Switch to clock mode for the time input.")
                                                   : _("Switch to text input mode for the time input."), -1);
  /* the window wraps the content: the fields are narrower than the face */
  gtk_widget_set_size_request (picker->content, !picker->use_24h ? CARD_12H
                                                : picker->keyboard ? CARD_KEYBOARD_24H : CARD_CLOCK_24H, -1);
  if (picker->keyboard)
    {
      sync_fields (picker);
      check_field (picker, picker->minutes);
    }
  else
    {
      /* the keyboard down, the face at the time typed */
      gtk_widget_grab_focus (GTK_WIDGET (button));
      show_value (picker);
      set_minutes (picker, picker->minutes);
      turn_to_value (CAL_CLOCK_FACE (picker->face), FALSE);
    }
}

static void
on_response (VibeosDialog *dialog, int response, Picker *picker)
{
  if (response == VIBEOS_RESPONSE_OK)
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
  VibeosDialog *dialog = vibeos_dialog_new (NULL);
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *title = gtk_label_new (_("Select time"));
  GtkWidget *display = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *colon = gtk_label_new (":");
  GtkWidget *buttons, *space;
  CalClockFace *face;

  picker->dialog = dialog;
  picker->content = content;
  picker->hour = CLAMP (hour, 0, 23);
  picker->minute = CLAMP (minute, 0, 59);
  picker->use_24h = cal_format_uses_24h ();
  picker->chosen = chosen;
  picker->data = data;

  gtk_label_set_xalign (GTK_LABEL (title), 0);
  vibeos_label_set_font_padding (title, 12);
  gtk_widget_add_css_class (title, "cal-time-title");
  gtk_box_append (GTK_BOX (content), title);

  picker->hour_box = time_box (picker, &picker->hour_label, _("Hour"), G_CALLBACK (on_hour_box));
  picker->minute_box = time_box (picker, &picker->minute_label, _("Minute"), G_CALLBACK (on_minute_box));
  gtk_widget_add_css_class (colon, "cal-time-colon");
  gtk_widget_set_size_request (colon, 24, -1);
  gtk_box_append (GTK_BOX (display), picker->hour_box);
  gtk_box_append (GTK_BOX (display), colon);
  gtk_box_append (GTK_BOX (display), picker->minute_box);
  if (!picker->use_24h)
    {
      picker->period = period_toggle (picker, display, "cal-time-display-period");
    }
  gtk_widget_set_halign (display, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (display, "cal-time-display");
  picker->display = display;
  gtk_box_append (GTK_BOX (content), display);

  face = g_object_new (CAL_TYPE_CLOCK_FACE, NULL);
  face->picker = picker;
  picker->face = GTK_WIDGET (face);
  gtk_widget_set_halign (picker->face, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (picker->face, "cal-time-face");
  gtk_accessible_update_property (GTK_ACCESSIBLE (picker->face), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Clock face"), -1);
  gtk_box_append (GTK_BOX (content), picker->face);

  picker->input = keyboard_input (picker);
  gtk_box_append (GTK_BOX (content), picker->input);

  gtk_widget_set_size_request (content, picker->use_24h ? CARD_CLOCK_24H : CARD_12H, -1);
  gtk_widget_add_css_class (content, "cal-time-picker");
  vibeos_dialog_set_flush (dialog, TRUE);
  vibeos_dialog_set_content (dialog, content);
  gtk_widget_add_css_class (gtk_widget_get_parent (content), "cal-time-card");
  vibeos_dialog_add_button (dialog, _("Cancel"), VIBEOS_RESPONSE_CANCEL);
  vibeos_dialog_add_button (dialog, _("OK"), VIBEOS_RESPONSE_OK);

  /* the keyboard button at the start of the buttons' row (the card's last child), a little higher than Cancel
   * and OK */
  picker->mode = vibeos_icon_button_new ("cal-keyboard-symbolic");
  gtk_widget_add_css_class (picker->mode, "cal-time-mode");
  gtk_widget_set_valign (picker->mode, GTK_ALIGN_START);
  gtk_accessible_update_property (GTK_ACCESSIBLE (picker->mode), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _("Switch to text input mode for the time input."), -1);
  g_signal_connect (picker->mode, "clicked", G_CALLBACK (on_mode), picker);
  buttons = gtk_widget_get_last_child (gtk_widget_get_parent (content));
  if (buttons && gtk_widget_has_css_class (buttons, "vibeos-dialog-buttons"))
    {
      space = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_widget_set_hexpand (space, TRUE);
      gtk_widget_set_halign (buttons, GTK_ALIGN_FILL);
      gtk_box_prepend (GTK_BOX (buttons), space);
      gtk_box_prepend (GTK_BOX (buttons), picker->mode);
    }

  g_signal_connect_data (dialog, "response", G_CALLBACK (on_response), picker, picker_free, 0);
  show_value (picker);
  set_minutes (picker, FALSE);
  turn_to_value (face, FALSE);
  vibeos_dialog_present (dialog, parent);
}
