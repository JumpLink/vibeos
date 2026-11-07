/* AlarmCard: see alarm-card.h. Sizes in dp, measured on still:clock-alarm and still:clock-alarm-expanded (a 328 dp
 * wide card on the 360 dp screen); the source line for each is in spec/clock.md §2. */
#include "alarm-card.h"
#include "alarm-model.h"
#include "clock-format.h"

#include <glib/gi18n.h>
#include <neo.h>

#define CARD_RADIUS     28        /* alarm_background */
#define CARD_TOP        8         /* MaterialCardViewStyle: the margin above every card */
#define ROW             48        /* a row of the card */
#define TIME_SIZE       56        /* display_time */
#define TIME_ROW        74.67     /* the time view, font padding included: 224 px */
#define START           16        /* start_guide_thin */
#define TEXT_PAD        4         /* the rows' paddingStart */
#define ICON_GAP        16        /* drawablePadding */
#define CARET_END       4         /* end_guide_extreme_thin */
#define SWITCH_END      22        /* end_guide_thin 16 + marginEnd 6 */
#define DAYS_INSET      12        /* start_guide_very_thin 8 + marginHorizontal 4 */
#define DAY_CIRCLE      32        /* bg_day_selected inset 8 dp in the 48 dp button */
#define DISABLED_ALPHA  0.69      /* AlarmItemViewHolder.CLOCK_DISABLED_ALPHA */
#define CHANGE_MS       150       /* the list's change duration */

/* --- a day button: a 32 dp circle with the day's letter (DayButtonStyle: no ripple, a tap swaps the circle) */

#define ALARM_TYPE_DAY (alarm_day_get_type ())
G_DECLARE_FINAL_TYPE (AlarmDay, alarm_day, ALARM, DAY, GtkWidget)

struct _AlarmDay
{
  GtkWidget parent;
  PangoLayout *letter;
  gboolean checked;
};

G_DEFINE_FINAL_TYPE (AlarmDay, alarm_day, GTK_TYPE_WIDGET)

static void
alarm_day_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  AlarmDay *self = ALARM_DAY (widget);
  double w = gtk_widget_get_width (widget), h = gtk_widget_get_height (widget);
  graphene_rect_t box = GRAPHENE_RECT_INIT ((w - DAY_CIRCLE) / 2, (h - DAY_CIRCLE) / 2, DAY_CIRCLE, DAY_CIRCLE);
  GskRoundedRect circle;
  PangoRectangle text;
  GdkRGBA ink;

  gsk_rounded_rect_init_from_rect (&circle, &box, DAY_CIRCLE / 2.0);
  if (self->checked)
    {
      /* bg_day_selected: a disc in colorPrimary, the letter in the window background colour */
      GdkRGBA fill = neo_color (NEO_PRIMARY);

      gtk_snapshot_push_rounded_clip (snapshot, &circle);
      gtk_snapshot_append_color (snapshot, &fill, &box);
      gtk_snapshot_pop (snapshot);
      ink = neo_color (NEO_SURFACE);
    }
  else
    {
      /* bg_day_unselected: a 1 dp ring in colorSecondary, the letter white */
      GdkRGBA ring = neo_color (NEO_SECONDARY);
      float widths[4] = { 1, 1, 1, 1 };
      GdkRGBA colors[4] = { ring, ring, ring, ring };

      gtk_snapshot_append_border (snapshot, &circle, widths, colors);
      ink = neo_color (NEO_WHITE);
    }
  pango_layout_get_pixel_extents (self->letter, NULL, &text);
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT ((w - text.width) / 2, (h - text.height) / 2));
  gtk_snapshot_append_layout (snapshot, self->letter, &ink);
  gtk_snapshot_restore (snapshot);
}

static void
alarm_day_dispose (GObject *object)
{
  g_clear_object (&ALARM_DAY (object)->letter);
  G_OBJECT_CLASS (alarm_day_parent_class)->dispose (object);
}

static void
alarm_day_class_init (AlarmDayClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = alarm_day_dispose;
  GTK_WIDGET_CLASS (klass)->snapshot = alarm_day_snapshot;
  g_signal_new ("toggled", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
on_day_released (GtkGestureClick *click, int n_press, double x, double y, AlarmDay *self)
{
  g_signal_emit_by_name (self, "toggled");
}

static void
alarm_day_init (AlarmDay *self)
{
  GtkGesture *click = gtk_gesture_click_new ();

  g_signal_connect (click, "released", G_CALLBACK (on_day_released), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
  g_object_set_data (G_OBJECT (self), "neo-tappable", GINT_TO_POINTER (TRUE));
}

static GtkWidget *
alarm_day_new (int index)
{
  AlarmDay *self = g_object_new (ALARM_TYPE_DAY, NULL);
  g_autofree char *letter = clock_weekday_letter (index);
  PangoFontDescription *font = pango_font_description_from_string ("Neo Roboto");

  /* day_button_font_size: 18 sp on this screen (values-h420dp), weight 400 + 300 */
  pango_font_description_set_weight (font, 700);
  pango_font_description_set_absolute_size (font, 18 * PANGO_SCALE);
  self->letter = gtk_widget_create_pango_layout (GTK_WIDGET (self), letter);
  pango_layout_set_font_description (self->letter, font);
  pango_font_description_free (font);
  return GTK_WIDGET (self);
}

/* --- the expand arrow: ic_caret_down / ic_caret_up, a 20 dp disc in colorOutline with a white chevron */

#define ALARM_TYPE_CARET (alarm_caret_get_type ())
G_DECLARE_FINAL_TYPE (AlarmCaret, alarm_caret, ALARM, CARET, GtkWidget)

struct _AlarmCaret
{
  GtkWidget parent;
  gboolean up;
};

G_DEFINE_FINAL_TYPE (AlarmCaret, alarm_caret, GTK_TYPE_WIDGET)

/* one bar of the chevron, through the drawable's groups in their order: a 2 x 8 rect, moved by caret_*_pivot and
 * caret_*_rect_position, rotated by caret_l / caret_r */
static void
caret_bar (GtkSnapshot *snapshot, float rotation, float pivot_y, float position_y, const GdkRGBA *white)
{
  gtk_snapshot_save (snapshot);
  gtk_snapshot_rotate (snapshot, rotation);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (0, pivot_y + position_y));
  gtk_snapshot_append_color (snapshot, white, &GRAPHENE_RECT_INIT (-1, -4, 2, 8));
  gtk_snapshot_restore (snapshot);
}

static void
alarm_caret_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  AlarmCaret *self = ALARM_CARET (widget);
  double cx = gtk_widget_get_width (widget) / 2.0, cy = gtk_widget_get_height (widget) / 2.0;
  GdkRGBA disc = neo_color (NEO_OUTLINE), white = neo_color (NEO_WHITE);
  GskRoundedRect circle;

  gsk_rounded_rect_init_from_rect (&circle, &GRAPHENE_RECT_INIT (cx - 10, cy - 10, 20, 20), 10);
  gtk_snapshot_push_rounded_clip (snapshot, &circle);
  gtk_snapshot_append_color (snapshot, &disc, &circle.bounds);
  gtk_snapshot_pop (snapshot);

  /* the vector is 32 viewport units over 32 dp (24 dp inset -6 dp on every side), centred on the disc */
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (cx - 16, cy - 16));
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (16, self->up ? 13 : 19));
  gtk_snapshot_rotate (snapshot, 90);
  caret_bar (snapshot, self->up ? -45 : 45, 4, -1, &white);
  caret_bar (snapshot, self->up ? 45 : -45, -4, 1, &white);
  gtk_snapshot_restore (snapshot);
}

static void
alarm_caret_class_init (AlarmCaretClass *klass)
{
  GTK_WIDGET_CLASS (klass)->snapshot = alarm_caret_snapshot;
}

static void
alarm_caret_init (AlarmCaret *self)
{
  gtk_widget_set_size_request (GTK_WIDGET (self), ROW, ROW);
}

/* --- the card */

struct _AlarmCard
{
  GtkWidget parent;
  ClockAlarm *alarm;
  GSettings *settings;

  gboolean expanded;            /* the state it is in, or animating to */
  double bounds;                /* 0 collapsed .. 1 expanded, eased */
  double elapsed;               /* ms into an expand or collapse; < 0 at rest */
  gboolean expanding;           /* which of the two `elapsed` is in */
  NeoAnim *change;
  double presence;              /* the list's add/remove: 0 no space .. 1 */
  NeoAnim *appear;
  gboolean leaving;
  GCallback gone;
  gpointer gone_data;

  GtkWidget *label_row, *label_icon, *label_text;
  GtkWidget *time, *time_text;
  GtkWidget *caret, *caret_icon;
  GtkWidget *repeat, *toggle, *dismiss;
  GtkWidget *days[7];
  GtkWidget *ringtone, *ringtone_icon, *ringtone_text;
  GtkWidget *vibrate, *vibrate_check;
  GtkWidget *delete;
  gboolean syncing;             /* the widgets are being set from the alarm: their handlers stay quiet */
};

G_DEFINE_FINAL_TYPE (AlarmCard, alarm_card, GTK_TYPE_WIDGET)

enum { TOGGLE_EXPANDED, MESSAGE, DELETED, N_SIGNALS };
static guint signals[N_SIGNALS];

static gboolean
has_label (AlarmCard *self)
{
  return self->alarm->label && *self->alarm->label;
}

/* Alarm.canPreemptivelyDismiss: an instance that is coming soon or snoozed can be dismissed from the list */
static gboolean
can_dismiss (AlarmCard *self)
{
  int state = self->alarm->instance ? self->alarm->instance_state : -1;

  return state == ALARM_STATE_SNOOZE || state == ALARM_STATE_HIGH_NOTIFICATION ||
         state == ALARM_STATE_LOW_NOTIFICATION || state == ALARM_STATE_HIDE_NOTIFICATION;
}

static double
collapsed_height (AlarmCard *self)
{
  return (has_label (self) ? ROW : 0) + TIME_ROW + ROW + (can_dismiss (self) ? ROW : 0);
}

static double
expanded_height (AlarmCard *self)
{
  /* label, time, repeat, days, sound, vibrate, (dismiss), Delete and its 8 dp bottom margin */
  return ROW + TIME_ROW + 4 * ROW + (can_dismiss (self) ? ROW : 0) + ROW + 8;
}

/* --- the expand and collapse timeline (spec/clock.md §2.7) */

/* the rows that fade, in the expand's order; the collapse runs them backwards */
enum { FADE_DAYS, FADE_SOUND, FADE_LABEL, FADE_DISMISS, FADE_DELETE, N_FADES };

static double
clamp01 (double v)
{
  return v < 0 ? 0 : v > 1 ? 1 : v;
}

/* how far row `which` has faded in (1) or out (0) at `elapsed` ms (ExpandedAlarmViewHolder.getExpansionAnimator,
 * getCollapseAnimator): expanding, each fades in over 100 ms (2/3 of the change) from 25 ms (1/6) on, a twelfth of
 * the change spread over the rows; collapsing, each fades out over 37.5 ms (1/4), Delete first, 7/12 spread */
static double
fade (AlarmCard *self, int which)
{
  int n = can_dismiss (self) ? 5 : 4, k = which;
  double inc;

  if (!can_dismiss (self) && which == FADE_DELETE)
    k = FADE_DISMISS;
  if (self->elapsed < 0)
    return self->expanded;
  if (self->expanding)
    {
      inc = CHANGE_MS * (1.0 / 12) / (n - 1);
      return clamp01 ((self->elapsed - (CHANGE_MS / 6.0 + k * inc)) / (CHANGE_MS * 2.0 / 3));
    }
  inc = CHANGE_MS * (7.0 / 12) / (n - 1);
  return 1 - clamp01 ((self->elapsed - (n - 1 - k) * inc) / (CHANGE_MS / 4.0));
}

static void
set_alphas (AlarmCard *self)
{
  double state = self->alarm->enabled ? 1 : DISABLED_ALPHA;
  double label = fade (self, FADE_LABEL);

  /* a labelled card shows its label collapsed too: the expanded label fades out with the others, then the
   * collapsed one fades in over the last 25 ms (CollapsedAlarmViewHolder.getCollapseAnimator) */
  if (!self->expanded && has_label (self))
    label = self->elapsed < 0 ? 1 : self->elapsed < 125 ? label : clamp01 ((self->elapsed - 125) / 25);
  gtk_widget_set_opacity (self->label_row, state * label);
  gtk_widget_set_opacity (self->time, state);
  gtk_widget_set_opacity (self->repeat, state);
  gtk_widget_set_opacity (self->dismiss, state);
  for (int i = 0; i < 7; i++)
    gtk_widget_set_opacity (self->days[i], state * fade (self, FADE_DAYS));
  gtk_widget_set_opacity (self->ringtone, fade (self, FADE_SOUND));
  gtk_widget_set_opacity (self->vibrate, fade (self, FADE_SOUND));
  gtk_widget_set_opacity (self->delete, fade (self, FADE_DELETE));
}

/* the label row looks expanded (icon, "Add label") from the start of an expand to the 125 ms point of a collapse */
static gboolean
label_looks_expanded (AlarmCard *self)
{
  return self->expanded || (self->elapsed >= 0 && self->elapsed < 125);
}

static void
on_change (NeoAnim *anim, double value, gpointer data)
{
  AlarmCard *self = data;
  double eased = spongy_curve (SPONGY_FAST_OUT_SLOW_IN, value / CHANGE_MS);

  self->elapsed = neo_anim_is_running (anim) ? value : -1;
  self->bounds = self->expanding ? eased : 1 - eased;
  /* the arrow is the holder's: it turns when the new holder takes over, at the end */
  ALARM_CARET (self->caret_icon)->up = self->elapsed < 0 ? self->expanded : !self->expanded;
  gtk_widget_queue_draw (self->caret_icon);
  gtk_widget_set_visible (self->label_icon, label_looks_expanded (self));
  set_alphas (self);
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

void
alarm_card_set_expanded (AlarmCard *self, gboolean expanded, gboolean animate)
{
  if (self->expanded == expanded)
    return;
  self->expanded = expanded;
  self->expanding = expanded;
  if (animate)
    neo_anim_start (self->change, 0, CHANGE_MS, CHANGE_MS, 0, SPONGY_LINEAR);
  else
    {
      neo_anim_stop (self->change);
      on_change (self->change, CHANGE_MS, self);
    }
}

gboolean
alarm_card_get_expanded (AlarmCard *self)
{
  return self->expanded;
}

/* --- the list's item animations */

static void
on_appear (NeoAnim *anim, double value, gpointer data)
{
  AlarmCard *self = data;
  gboolean appearing = !self->leaving;
  /* `value` is the time: 0..150 ms the space, 150..270 ms the fade (appearing); 0..120 the fade, 120..270 the
   * space (leaving) */
  double space = appearing ? spongy_curve (SPONGY_FAST_OUT_SLOW_IN, clamp01 (value / 150))
                           : 1 - spongy_curve (SPONGY_FAST_OUT_SLOW_IN, clamp01 ((value - 120) / 150));
  double alpha = appearing ? clamp01 ((value - 150) / 120) : 1 - clamp01 (value / 120);

  self->presence = space;
  gtk_widget_set_opacity (GTK_WIDGET (self), alpha);
  gtk_widget_queue_resize (GTK_WIDGET (self));
  if (!neo_anim_is_running (anim) && !appearing && self->gone)
    {
      GCallback gone = self->gone;

      self->gone = NULL;
      ((void (*) (AlarmCard *, gpointer)) gone) (self, self->gone_data);
    }
}

void
alarm_card_appear (AlarmCard *self)
{
  self->leaving = FALSE;
  neo_anim_start (self->appear, 0, 270, 270, 0, SPONGY_LINEAR);
}

void
alarm_card_disappear (AlarmCard *self, GCallback gone, gpointer data)
{
  self->gone = gone;
  self->gone_data = data;
  self->leaving = TRUE;
  gtk_widget_set_sensitive (GTK_WIDGET (self), FALSE);
  neo_anim_start (self->appear, 0, 270, 270, 0, SPONGY_LINEAR);
}

/* --- layout */

static void
alarm_card_measure (GtkWidget *widget, GtkOrientation orientation, int for_size,
                    int *minimum, int *natural, int *minimum_baseline, int *natural_baseline)
{
  AlarmCard *self = ALARM_CARD (widget);

  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    {
      *minimum = 0;
      *natural = 328;
      return;
    }
  double h = collapsed_height (self) + (expanded_height (self) - collapsed_height (self)) * self->bounds;
  *minimum = *natural = (int) ((CARD_TOP + h) * self->presence + 0.5);
}

/* puts `child` at (x, y) in the card, `width` wide (-1: its natural width) and `height` tall; a row that is
 * neither shown nor fading is taken out of the picking too */
static void
place (GtkWidget *child, double x, double y, double width, double height, gboolean shown)
{
  int min, nat;

  gtk_widget_set_child_visible (child, shown);
  if (!shown)
    return;
  if (width < 0)
    {
      gtk_widget_measure (child, GTK_ORIENTATION_HORIZONTAL, -1, &min, &nat, NULL, NULL);
      width = nat;
    }
  gtk_widget_allocate (child, (int) width, (int) height, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (x, y)));
}

static double
lerp (double a, double b, double t)
{
  return a + (b - a) * t;
}

static void
alarm_card_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  AlarmCard *self = ALARM_CARD (widget);
  double t = self->bounds, top = CARD_TOP;
  gboolean animating = self->elapsed >= 0;
  gboolean extra = self->expanded || animating;       /* the expanded-only rows are on screen */
  gboolean dismiss = can_dismiss (self);
  double label_c = has_label (self) ? ROW : 0;
  /* where the shared rows sit collapsed (c) and expanded (e) */
  double time_c = label_c, time_e = ROW;
  double repeat_c = time_c + TIME_ROW, repeat_e = time_e + TIME_ROW;
  double days_e = repeat_e + ROW, sound_e = days_e + ROW, vibrate_e = sound_e + ROW;
  double dismiss_c = repeat_c + ROW, dismiss_e = vibrate_e + ROW;
  double delete_e = dismiss_e + (dismiss ? ROW : 0);
  int min, nat;

  place (self->label_row, START, top, width - START - CARET_END - ROW, ROW,
         extra || has_label (self));
  gtk_widget_measure (self->time, GTK_ORIENTATION_HORIZONTAL, -1, &min, &nat, NULL, NULL);
  place (self->time, START, top + lerp (time_c, time_e, t), nat, TIME_ROW, TRUE);
  place (self->caret, width - CARET_END - ROW, top, ROW, ROW, TRUE);
  place (self->repeat, START, top + lerp (repeat_c, repeat_e, t), width - START - SWITCH_END - 52, ROW, TRUE);
  place (self->toggle, width - SWITCH_END - 52, top + lerp (repeat_c, repeat_e, t), 52, ROW, TRUE);
  place (self->dismiss, START, top + lerp (dismiss_c, dismiss_e, t), -1, ROW, dismiss);

  /* the seven buttons' left edges are at sevenths of the row (layout_constraintGuide_percent 0, 0.143, ...) */
  for (int i = 0; i < 7; i++)
    {
      static const double guide[7] = { 0, 0.143, 0.286, 0.429, 0.571, 0.714, 0.857 };
      double x = DAYS_INSET + guide[i] * (width - 2 * DAYS_INSET);

      place (self->days[i], x, top + days_e, ROW, ROW, extra);
    }
  place (self->ringtone, START, top + sound_e, width - 2 * START, ROW, extra);
  place (self->vibrate, START, top + vibrate_e, width - 2 * START, ROW, extra);
  place (self->delete, START, top + delete_e, -1, ROW, extra);
}

static void
alarm_card_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  AlarmCard *self = ALARM_CARD (widget);
  double h = collapsed_height (self) + (expanded_height (self) - collapsed_height (self)) * self->bounds;
  GskRoundedRect card;
  GdkRGBA background = neo_color (NEO_SURFACE_CONTAINER_HIGHEST);

  gsk_rounded_rect_init_from_rect (&card, &GRAPHENE_RECT_INIT (0, CARD_TOP, gtk_widget_get_width (widget), h),
                                   CARD_RADIUS);
  /* everything is clipped to the card, so the expanded rows come out from under its edge as it grows */
  gtk_snapshot_push_rounded_clip (snapshot, &card);
  gtk_snapshot_append_color (snapshot, &background, &card.bounds);
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child; child = gtk_widget_get_next_sibling (child))
    gtk_widget_snapshot_child (widget, child, snapshot);
  gtk_snapshot_pop (snapshot);
}

/* --- showing the alarm */

static void
show_alarm (AlarmCard *self)
{
  ClockAlarm *alarm = self->alarm;
  gboolean use_24h = neo_clock_uses_24h ();
  g_autofree char *time = clock_time_markup (alarm->hour, alarm->minute, use_24h, 0.3);
  g_autofree char *repeat = NULL, *sound = NULL;
  GDateWeekday first = clock_week_start (self->settings);

  self->syncing = TRUE;
  gtk_label_set_text (GTK_LABEL (self->label_text), has_label (self) ? alarm->label : _("Add label"));
  if (has_label (self))
    gtk_widget_remove_css_class (self->label_text, "hint");
  else
    gtk_widget_add_css_class (self->label_text, "hint");

  /* an enabled alarm's time is Typeface.BOLD (900 here), a disabled one's NORMAL (700): .enabled in the CSS */
  gtk_label_set_markup (GTK_LABEL (self->time_text), time);
  if (alarm->enabled)
    gtk_widget_add_css_class (self->time_text, "enabled");
  else
    gtk_widget_remove_css_class (self->time_text, "enabled");

  if (alarm->days)
    repeat = alarm_days_text (alarm->days, first);
  else
    {
      /* Alarm.isTomorrow: a one-time alarm whose time of day has passed rings tomorrow */
      g_autoptr(GDateTime) now = g_date_time_new_now_local ();
      int now_minutes = g_date_time_get_hour (now) * 60 + g_date_time_get_minute (now);

      repeat = g_strdup (alarm->hour * 60 + alarm->minute <= now_minutes &&
                         alarm->instance_state != ALARM_STATE_SNOOZE ? _("Tomorrow") : _("Today"));
    }
  gtk_label_set_text (GTK_LABEL (self->repeat), repeat);
  neo_switch_set_active (NEO_SWITCH (self->toggle), alarm->enabled);

  if (can_dismiss (self))
    {
      g_autofree char *text = NULL;

      if (alarm->instance_state == ALARM_STATE_SNOOZE)
        {
          g_autoptr(GDateTime) until = g_date_time_new_from_unix_local (alarm->instance_time);
          g_autofree char *when = clock_time_text (g_date_time_get_hour (until), g_date_time_get_minute (until),
                                                   use_24h);

          text = g_strdup_printf (_("Snoozing until %s"), when);
        }
      gtk_label_set_text (GTK_LABEL (neo_pressable_get_child (NEO_PRESSABLE (self->dismiss))),
                          text ? text : _("Dismiss"));
    }

  /* the days in the week's order: button i is the week's i-th day (day: Monday 0 .. Sunday 6) */
  for (int i = 0; i < 7; i++)
    {
      int day = (first - G_DATE_MONDAY + i) % 7;
      AlarmDay *button = ALARM_DAY (self->days[i]);
      g_autofree char *letter = clock_weekday_letter (day);

      g_object_set_data (G_OBJECT (button), "day", GINT_TO_POINTER (day));
      pango_layout_set_text (button->letter, letter, -1);
      button->checked = (alarm->days >> day) & 1;
      gtk_widget_queue_draw (GTK_WIDGET (button));
    }

  sound = clock_ringtone_title (alarm->ringtone, self->settings);
  gtk_label_set_text (GTK_LABEL (self->ringtone_text), sound);
  gtk_image_set_from_icon_name (GTK_IMAGE (self->ringtone_icon),
                                alarm->ringtone && !*alarm->ringtone ? "clock-ringtone-silent-symbolic"
                                                                     : "clock-ringtone-symbolic");
  gtk_image_set_from_icon_name (GTK_IMAGE (self->vibrate_check),
                                alarm->vibrate ? "clock-selector-checkbox-checked-symbolic"
                                               : "clock-selector-checkbox-unchecked-symbolic");
  if (alarm->vibrate)
    gtk_widget_add_css_class (self->vibrate_check, "checked");
  else
    gtk_widget_remove_css_class (self->vibrate_check, "checked");

  gtk_widget_set_visible (self->label_icon, label_looks_expanded (self));
  set_alphas (self);
  gtk_widget_queue_resize (GTK_WIDGET (self));
  self->syncing = FALSE;
}

/* --- what the user does */

static void
update (AlarmCard *self, GVariant *changes)
{
  alarms_client_update (alarms_client_get (), self->alarm->id, changes);
}

/* the "Alarm set for ..." snackbar, for the alarm as it now is (AlarmUtils.popAlarmSetSnackbar) */
static void
tell_alarm_set (AlarmCard *self, int hour, int minute, guint days)
{
  g_autoptr(Alarm) alarm = alarm_new ();
  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  g_autoptr(GDateTime) next = NULL;
  g_autofree char *text = NULL;

  alarm->hour = hour;
  alarm->minute = minute;
  alarm->days = days;
  next = alarm_next_time (alarm, now);
  text = clock_alarm_set_text (g_date_time_difference (next, now) / G_TIME_SPAN_SECOND);
  g_signal_emit (self, signals[MESSAGE], 0, text);
}

static void
on_switch (NeoSwitch *toggle, GParamSpec *pspec, AlarmCard *self)
{
  gboolean on = neo_switch_get_active (toggle);

  if (self->syncing || on == self->alarm->enabled)
    return;
  update (self, g_variant_new_parsed ("{'enabled': <%b>}", on));
  if (on)
    tell_alarm_set (self, self->alarm->hour, self->alarm->minute, self->alarm->days);
}

static void
on_time_chosen (int hour, int minute, gpointer data)
{
  AlarmCard *self = data;

  /* AlarmTimeClickHandler.onTimeSet: the new time, and the alarm switched on */
  update (self, g_variant_new_parsed ("{'hour': <%i>, 'minute': <%i>, 'enabled': <true>}", hour, minute));
  tell_alarm_set (self, hour, minute, self->alarm->days);
}

static void
on_time (NeoPressable *time, AlarmCard *self)
{
  /* a tap on the time of a collapsed card opens it as well as the picker */
  if (!self->expanded)
    g_signal_emit (self, signals[TOGGLE_EXPANDED], 0);
  neo_time_picker_dialog (GTK_WIDGET (self), self->alarm->hour, self->alarm->minute, on_time_chosen,
                          g_object_ref (self), g_object_unref);
}

static void
on_day (AlarmDay *day, AlarmCard *self)
{
  int index = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (day), "day"));
  guint days = self->alarm->days ^ (1u << index);
  g_autoptr(Alarm) before = alarm_new (), after = alarm_new ();
  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  g_autoptr(GDateTime) was = NULL, will = NULL;

  day->checked = !day->checked;
  gtk_widget_queue_draw (GTK_WIDGET (day));
  update (self, g_variant_new_parsed ("{'days': <%u>}", days));
  /* the snackbar only when the next ring moved (AlarmTimeClickHandler.setDayOfWeekEnabled) */
  before->hour = after->hour = self->alarm->hour;
  before->minute = after->minute = self->alarm->minute;
  before->days = self->alarm->days;
  after->days = days;
  was = alarm_next_time (before, now);
  will = alarm_next_time (after, now);
  if (self->alarm->enabled && !g_date_time_equal (was, will))
    tell_alarm_set (self, self->alarm->hour, self->alarm->minute, days);
}

static void
on_label_response (NeoDialog *dialog, int response, AlarmCard *self)
{
  GtkWidget *entry = g_object_get_data (G_OBJECT (dialog), "entry");

  if (response == NEO_RESPONSE_OK)
    update (self, g_variant_new_parsed ("{'label': <%s>}", gtk_editable_get_text (GTK_EDITABLE (entry))));
}

static void
on_label_activate (GtkEntry *entry, NeoDialog *dialog)
{
  /* the keyboard's Done is OK */
  neo_dialog_respond (dialog, NEO_RESPONSE_OK);
}

static void
on_label (NeoPressable *row, AlarmCard *self)
{
  /* LabelDialogFragment: "Label" over one line of text, Cancel and OK; the text starts selected */
  NeoDialog *dialog = neo_dialog_new (NULL);
  GtkWidget *entry = gtk_entry_new ();

  neo_dialog_set_message (dialog, _("Label"));
  gtk_editable_set_text (GTK_EDITABLE (entry), self->alarm->label);
  gtk_entry_set_input_hints (GTK_ENTRY (entry), GTK_INPUT_HINT_UPPERCASE_SENTENCES);
  gtk_widget_add_css_class (entry, "neo-entry");
  gtk_widget_add_css_class (entry, "label-entry");
  g_signal_connect (entry, "activate", G_CALLBACK (on_label_activate), dialog);
  neo_dialog_set_content (dialog, entry);
  neo_dialog_add_button (dialog, _("Cancel"), NEO_RESPONSE_CANCEL);
  neo_dialog_add_button (dialog, _("OK"), NEO_RESPONSE_OK);
  g_object_set_data (G_OBJECT (dialog), "entry", entry);
  g_signal_connect_object (dialog, "response", G_CALLBACK (on_label_response), self, 0);
  neo_dialog_present (dialog, GTK_WIDGET (self));
  gtk_widget_grab_focus (entry);
  gtk_editable_select_region (GTK_EDITABLE (entry), 0, -1);
}

/* a short buzz as a preview of the vibration (AlarmTimeClickHandler.setAlarmVibrationEnabled: 300 ms) */
static void
buzz (void)
{
  g_autoptr(GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  GVariantBuilder pattern;

  if (!bus)
    return;
  g_variant_builder_init (&pattern, G_VARIANT_TYPE ("a(du)"));
  g_variant_builder_add (&pattern, "(du)", 1.0, 300);
  g_dbus_connection_call (bus, "org.sigxcpu.Feedback", "/org/sigxcpu/Feedback", "org.sigxcpu.Feedback.Haptic",
                          "Vibrate", g_variant_new ("(sa(du))", "de.yesman.Clock", &pattern), NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void
on_vibrate (NeoPressable *row, AlarmCard *self)
{
  gboolean on = !self->alarm->vibrate;

  update (self, g_variant_new_parsed ("{'vibrate': <%b>}", on));
  if (on)
    buzz ();
}

static void
on_dismiss (NeoPressable *button, AlarmCard *self)
{
  g_autofree char *time = clock_time_text (self->alarm->hour, self->alarm->minute, neo_clock_uses_24h ());
  g_autofree char *text = g_strdup_printf (_("%s alarm dismissed"), time);

  alarms_client_dismiss (alarms_client_get (), self->alarm->instance);
  g_signal_emit (self, signals[MESSAGE], 0, text);
}

static void
on_delete (NeoPressable *button, AlarmCard *self)
{
  g_signal_emit (self, signals[DELETED], 0, self->alarm);
}

static void
on_caret (NeoPressable *caret, AlarmCard *self)
{
  g_signal_emit (self, signals[TOGGLE_EXPANDED], 0);
}

static void
on_body_released (GtkGestureClick *click, int n_press, double x, double y, AlarmCard *self)
{
  /* the card itself has no ripple: a tap anywhere else on it opens or closes it */
  if (!neo_pick_is_tappable (GTK_WIDGET (self), x, y))
    g_signal_emit (self, signals[TOGGLE_EXPANDED], 0);
}

/* --- building it */

static GtkWidget *
body_label (const char *css)
{
  GtkWidget *label = gtk_label_new (NULL);

  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode (GTK_LABEL (label), TRUE);
  /* a 14 sp text view centred in its 48 dp row */
  neo_label_set_font_padding (label, 14);
  gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (label, "body");
  if (css)
    gtk_widget_add_css_class (label, css);
  return label;
}

/* a 48 dp row: an icon at 4 dp, its text 16 dp after it, in a pressable with the plain ripple */
static GtkWidget *
icon_row (const char *icon_name, GtkWidget **icon, GtkWidget **text)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, ICON_GAP);
  GtkWidget *row;

  gtk_widget_set_margin_start (box, TEXT_PAD);
  gtk_widget_set_margin_end (box, TEXT_PAD);
  *icon = gtk_image_new_from_icon_name (icon_name);
  gtk_image_set_pixel_size (GTK_IMAGE (*icon), 24);
  *text = body_label (NULL);
  gtk_widget_set_hexpand (*text, TRUE);
  gtk_box_append (GTK_BOX (box), *icon);
  gtk_box_append (GTK_BOX (box), *text);
  row = neo_pressable_new (box);
  neo_pressable_set_ripple (NEO_PRESSABLE (row), NEO_RIPPLE_BOUNDED, 0);
  return row;
}

static void
alarm_card_dispose (GObject *object)
{
  AlarmCard *self = ALARM_CARD (object);
  GtkWidget *child;

  g_clear_pointer (&self->change, neo_anim_free);
  g_clear_pointer (&self->appear, neo_anim_free);
  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self))))
    gtk_widget_unparent (child);
  g_clear_pointer (&self->alarm, clock_alarm_free);
  g_clear_object (&self->settings);
  G_OBJECT_CLASS (alarm_card_parent_class)->dispose (object);
}

static void
alarm_card_class_init (AlarmCardClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = alarm_card_dispose;
  widget_class->measure = alarm_card_measure;
  widget_class->size_allocate = alarm_card_size_allocate;
  widget_class->snapshot = alarm_card_snapshot;
  gtk_widget_class_set_css_name (widget_class, "alarm-card");

  signals[TOGGLE_EXPANDED] = g_signal_new ("toggle-expanded", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                           NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[MESSAGE] = g_signal_new ("message", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                   NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[DELETED] = g_signal_new ("deleted", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                   NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_POINTER);
}

static void
alarm_card_init (AlarmCard *self)
{
  GtkWidget *widget = GTK_WIDGET (self);
  GtkWidget *icon, *text;
  GtkGesture *click;

  self->elapsed = -1;
  self->presence = 1;
  self->change = neo_anim_new (widget, on_change, self);
  self->appear = neo_anim_new (widget, on_appear, self);

  /* the label row: ic_label and the label (or the "Add label" hint) */
  self->label_row = icon_row ("clock-label-symbolic", &self->label_icon, &self->label_text);
  gtk_widget_add_css_class (self->label_icon, "label-icon");
  g_signal_connect (self->label_row, "clicked", G_CALLBACK (on_label), self);
  gtk_widget_set_parent (self->label_row, widget);

  /* the time: display_time, a plain ripple over it */
  self->time_text = gtk_label_new (NULL);
  gtk_widget_add_css_class (self->time_text, "alarm-time");
  neo_label_set_font_padding (self->time_text, TIME_SIZE);
  self->time = neo_pressable_new (self->time_text);
  neo_pressable_set_ripple (NEO_PRESSABLE (self->time), NEO_RIPPLE_BOUNDED, 0);
  g_signal_connect (self->time, "clicked", G_CALLBACK (on_time), self);
  gtk_widget_set_parent (self->time, widget);

  self->caret_icon = g_object_new (ALARM_TYPE_CARET, NULL);
  self->caret = neo_pressable_new (self->caret_icon);
  neo_pressable_set_ripple (NEO_PRESSABLE (self->caret), NEO_RIPPLE_CIRCLE, 20);
  g_signal_connect (self->caret, "clicked", G_CALLBACK (on_caret), self);
  gtk_widget_set_parent (self->caret, widget);

  self->repeat = body_label (NULL);
  gtk_widget_set_margin_start (self->repeat, TEXT_PAD);
  gtk_widget_set_margin_end (self->repeat, TEXT_PAD);
  gtk_widget_set_parent (self->repeat, widget);

  self->toggle = neo_switch_new ();
  g_signal_connect (self->toggle, "notify::active", G_CALLBACK (on_switch), self);
  gtk_widget_set_parent (self->toggle, widget);

  self->dismiss = neo_text_button_new (NULL);
  gtk_widget_add_css_class (self->dismiss, "alarm-text-button");
  g_signal_connect (self->dismiss, "clicked", G_CALLBACK (on_dismiss), self);
  gtk_widget_set_parent (self->dismiss, widget);

  for (int i = 0; i < 7; i++)
    {
      self->days[i] = alarm_day_new (i);
      g_signal_connect (self->days[i], "toggled", G_CALLBACK (on_day), self);
      gtk_widget_set_parent (self->days[i], widget);
    }

  self->ringtone = icon_row ("clock-ringtone-symbolic", &self->ringtone_icon, &self->ringtone_text);
  gtk_widget_add_css_class (self->ringtone, "ringtone");
  gtk_widget_set_parent (self->ringtone, widget);

  /* Vibrate: ic_vibrate, the word, and the round check at the end (a CheckBox with drawableEnd) */
  self->vibrate = icon_row ("clock-vibrate-symbolic", &icon, &text);
  gtk_label_set_text (GTK_LABEL (text), _("Vibrate"));
  gtk_widget_add_css_class (text, "vibrate-text");
  self->vibrate_check = gtk_image_new ();
  gtk_image_set_pixel_size (GTK_IMAGE (self->vibrate_check), 24);
  gtk_widget_add_css_class (self->vibrate_check, "vibrate-check");
  gtk_box_append (GTK_BOX (gtk_widget_get_parent (text)), self->vibrate_check);
  g_signal_connect (self->vibrate, "clicked", G_CALLBACK (on_vibrate), self);
  gtk_widget_set_parent (self->vibrate, widget);

  self->delete = neo_text_button_new (_("Delete"));
  gtk_widget_add_css_class (self->delete, "alarm-text-button");
  g_signal_connect (self->delete, "clicked", G_CALLBACK (on_delete), self);
  gtk_widget_set_parent (self->delete, widget);

  click = gtk_gesture_click_new ();
  g_signal_connect (click, "released", G_CALLBACK (on_body_released), self);
  gtk_widget_add_controller (widget, GTK_EVENT_CONTROLLER (click));
}

static ClockAlarm *
copy_alarm (const ClockAlarm *alarm)
{
  ClockAlarm *copy = g_memdup2 (alarm, sizeof *alarm);

  copy->label = g_strdup (alarm->label);
  copy->ringtone = g_strdup (alarm->ringtone);
  return copy;
}

GtkWidget *
alarm_card_new (const ClockAlarm *alarm, GSettings *settings)
{
  AlarmCard *self = g_object_new (ALARM_TYPE_CARD, NULL);

  self->settings = g_object_ref (settings);
  self->alarm = copy_alarm (alarm);
  show_alarm (self);
  return GTK_WIDGET (self);
}

void
alarm_card_update (AlarmCard *self, const ClockAlarm *alarm)
{
  g_clear_pointer (&self->alarm, clock_alarm_free);
  self->alarm = copy_alarm (alarm);
  show_alarm (self);
}

guint
alarm_card_get_id (AlarmCard *self)
{
  return self->alarm->id;
}

const ClockAlarm *
alarm_card_get_alarm (AlarmCard *self)
{
  return self->alarm;
}
