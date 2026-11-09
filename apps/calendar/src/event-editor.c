/* The event editor (event-editor.h; spec/calendar.md §7). */
#include "event-editor.h"
#include "cal-colors.h"
#include "cal-date-picker.h"
#include "cal-dialogs.h"
#include "cal-format.h"
#include "cal-page.h"
#include "cal-popup.h"
#include "cal-reminders.h"
#include "cal-time-picker.h"
#include "cal-window.h"

#include <glib/gi18n.h>
#include <neo.h>
#include <spongy-gtk.h>
#include <string.h>

typedef struct {
  GtkWidget *page, *window;
  CalStore *store;
  GSettings *settings;
  CalEvent *original;           /* the event changed, or NULL for a new one */
  CalScope scope;
  CalEvent *source;             /* what the fields started from (a copy for Duplicate and Import) */

  char *calendar;
  int start_day, end_day;       /* the dates shown: an all-day event's end is its last day */
  int start_minute, end_minute; /* the times shown, minutes into the day */
  gboolean all_day;
  char *rrule;

  GtkWidget *title, *calendar_row, *calendar_name, *calendar_account, *calendar_color;
  GtkWidget *all_day_switch, *start_date, *start_time, *end_date, *end_time;
  GtkWidget *location, *description, *url, *repeat_row, *repeat, *reminders, *add_reminder;
  GtkWidget *scroll;
  gboolean saving;
} Editor;

static void
editor_free (gpointer data)
{
  Editor *editor = data;

  g_clear_pointer (&editor->original, cal_event_unref);
  g_clear_pointer (&editor->source, cal_event_unref);
  g_free (editor->calendar);
  g_free (editor->rrule);
  g_object_unref (editor->store);
  g_object_unref (editor->settings);
  g_free (editor);
}

/* --- times: day numbers and minutes into the day, the phone's zone */

static gint64
local_time (int day, int minute)
{
  GDate date;
  g_autoptr(GDateTime) time = NULL;

  g_date_clear (&date, 1);
  g_date_set_julian (&date, day);
  time = g_date_time_new_local (g_date_get_year (&date), g_date_get_month (&date), g_date_get_day (&date),
                                minute / 60, minute % 60, 0);
  return g_date_time_to_unix (time);
}

static void
split_time (gint64 time, int *day, int *minute)
{
  *day = cal_day_number (time);
  *minute = cal_minute_of_day (time);
}

static gint64
start_time (Editor *editor)
{
  return editor->all_day ? cal_day_start (editor->start_day) : local_time (editor->start_day, editor->start_minute);
}

static gint64
end_time (Editor *editor)
{
  return editor->all_day ? cal_day_start (editor->end_day + 1) : local_time (editor->end_day, editor->end_minute);
}

static char *
format_minute (int minute)
{
  return cal_format_time (local_time (cal_day_number (g_get_real_time () / G_USEC_PER_SEC), minute));
}

/* --- showing the values */

static void
set_button_text (GtkWidget *button, const char *text)
{
  gtk_label_set_label (GTK_LABEL (neo_pressable_get_child (NEO_PRESSABLE (button))), text);
}

static void
show_times (Editor *editor)
{
  g_autofree char *start_date = cal_format_editor_date (editor->start_day);
  g_autofree char *end_date = cal_format_editor_date (editor->end_day);
  g_autofree char *start = format_minute (editor->start_minute);
  g_autofree char *end = format_minute (editor->end_minute);

  set_button_text (editor->start_date, start_date);
  set_button_text (editor->end_date, end_date);
  set_button_text (editor->start_time, start);
  set_button_text (editor->end_time, end);
  gtk_widget_set_visible (editor->start_time, !editor->all_day);
  gtk_widget_set_visible (editor->end_time, !editor->all_day);
}

static void
show_repeat (Editor *editor)
{
  g_autofree char *text = editor->rrule ? cal_format_rrule (editor->rrule, start_time (editor)) : NULL;

  /* a rule without words (made elsewhere) still repeats: say so rather than "One-time event" */
  set_button_text (editor->repeat, text ? text : editor->rrule ? _("Custom repeat") : _("One-time event"));
}

static void
show_calendar (Editor *editor)
{
  const CalCalendar *calendar = cal_store_find_calendar (editor->store, editor->calendar);
  GdkRGBA color;

  gtk_label_set_label (GTK_LABEL (editor->calendar_name), calendar ? calendar->name : "");
  gtk_label_set_label (GTK_LABEL (editor->calendar_account), calendar ? calendar->account : "");
  gtk_widget_set_visible (editor->calendar_account, calendar && calendar->account && *calendar->account);
  color = cal_argb (0xff000000 | (calendar ? calendar->color : 0x3f51b5));
  g_object_set_data_full (G_OBJECT (editor->calendar_color), "cal-color", g_memdup2 (&color, sizeof color), g_free);
  gtk_widget_queue_draw (editor->calendar_color);
}

/* --- the values the user changed (EditEventFragment.isModelChanged) */

static char *
entry_text (GtkWidget *entry)
{
  return g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (entry))));
}

static char *
description_text (Editor *editor)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (editor->description));
  GtkTextIter a, b;

  gtk_text_buffer_get_bounds (buffer, &a, &b);
  return g_strstrip (gtk_text_buffer_get_text (buffer, &a, &b, FALSE));
}

static CalEvent *
edited_event (Editor *editor)
{
  CalEvent *event = cal_event_new ();
  CalEvent *source = editor->source;

  event->calendar = g_strdup (editor->calendar);
  event->uid = g_strdup (editor->original ? editor->original->uid : NULL);
  event->rid = g_strdup (editor->original ? editor->original->rid : NULL);
  event->title = entry_text (editor->title);
  event->location = entry_text (editor->location);
  event->description = description_text (editor);
  event->url = entry_text (editor->url);
  event->all_day = editor->all_day;
  event->start = start_time (editor);
  event->end = MAX (end_time (editor), event->start);
  event->rrule = g_strdup (editor->rrule);
  event->recurring = editor->rrule != NULL;
  event->color = source ? source->color : 0;
  event->tzid = g_strdup (source ? source->tzid : NULL);
  g_array_unref (event->reminders);
  event->reminders = cal_reminders_get (CAL_REMINDERS (editor->reminders));
  return event;
}

static gboolean
text_equal (const char *a, const char *b)
{
  return g_strcmp0 (a && *a ? a : NULL, b && *b ? b : NULL) == 0;
}

static gboolean
changed (Editor *editor)
{
  g_autoptr(CalEvent) now = edited_event (editor);
  CalEvent *before = editor->source;

  if (!before)
    {
      /* a new event: anything typed counts, the times and the default reminder do not */
      return (now->title && *now->title) || (now->location && *now->location)
             || (now->description && *now->description) || (now->url && *now->url);
    }
  if (!text_equal (now->title, before->title) || !text_equal (now->location, before->location)
      || !text_equal (now->description, before->description) || !text_equal (now->url, before->url)
      || !text_equal (now->rrule, before->rrule) || now->all_day != before->all_day || now->start != before->start
      || now->end != before->end || g_strcmp0 (now->calendar, before->calendar) != 0)
    return TRUE;
  if (now->reminders->len != before->reminders->len)
    return TRUE;
  for (guint i = 0; i < now->reminders->len; i++)
    if (g_array_index (now->reminders, CalReminder, i).minutes
        != g_array_index (before->reminders, CalReminder, i).minutes)
      return TRUE;
  return FALSE;
}

/* --- Done and Cancel */

static void
on_saved (GObject *source, GAsyncResult *result, gpointer data)
{
  GtkWidget *window = data;
  g_autoptr(GError) error = NULL;

  if (!cal_store_save_finish (CAL_STORE (source), result, &error))
    {
      g_warning ("save: %s", error->message);
      cal_window_toast (window, error->message);
    }
  g_object_unref (window);
}

static void
done (Editor *editor)
{
  g_autoptr(CalEvent) edited = edited_event (editor);
  gboolean is_new = editor->original == NULL;

  /* EditEventFragment.onActionBarItemSelected: a new event with nothing in it is not made */
  if (is_new && !(edited->title && *edited->title) && !(edited->location && *edited->location)
      && !(edited->description && *edited->description))
    {
      cal_window_toast (editor->window, _("Empty event not created."));
      cal_window_pop (editor->window);
      return;
    }
  if (!is_new && !changed (editor))
    {
      cal_window_pop (editor->window);
      return;
    }
  if (is_new)
    g_settings_set_string (editor->settings, "last-calendar", editor->calendar);
  cal_store_save (editor->store, editor->original, edited, editor->scope, on_saved, g_object_ref (editor->window));
  cal_window_toast (editor->window, is_new ? _("Event created.") : _("Event saved."));
  cal_window_pop (editor->window);
}

static void
on_done (NeoPressable *button, Editor *editor)
{
  done (editor);
}

static void
on_discard_answer (int index, gpointer data)
{
  Editor *editor = data;

  if (index == 0)
    cal_window_pop (editor->window);
  g_object_unref (editor->page);
}

/* Back and Cancel: "Do you want to discard changes?" when something changed */
static gboolean
on_back (CalPage *page, gpointer data)
{
  Editor *editor = data;

  if (!changed (editor))
    return FALSE;
  cal_confirm_dialog (editor->page, _("Do you want to discard changes?"), _("Discard"), on_discard_answer,
                      g_object_ref (editor->page));
  return TRUE;
}

static void
on_cancel (NeoPressable *button, Editor *editor)
{
  cal_page_back (CAL_PAGE (editor->page));
}

/* --- the calendar */

static void
on_calendar_chosen (int index, gpointer data)
{
  Editor *editor = data;
  g_autoptr(GPtrArray) writable = g_ptr_array_new ();
  GPtrArray *calendars = cal_store_get_calendars (editor->store);

  for (guint i = 0; i < calendars->len; i++)
    if (!((CalCalendar *) calendars->pdata[i])->readonly)
      g_ptr_array_add (writable, calendars->pdata[i]);
  if (index < 0 || index >= (int) writable->len)
    return;
  g_free (editor->calendar);
  editor->calendar = g_strdup (((CalCalendar *) writable->pdata[index])->uid);
  show_calendar (editor);
}

static void
on_calendar (NeoPressable *row, Editor *editor)
{
  GPtrArray *calendars = cal_store_get_calendars (editor->store);
  g_autoptr(GPtrArray) labels = g_ptr_array_new_with_free_func (g_free);
  int selected = -1;

  /* the writable calendars, as Etar's calendars spinner lists them (name, then account) */
  for (guint i = 0; i < calendars->len; i++)
    {
      CalCalendar *calendar = calendars->pdata[i];

      if (calendar->readonly)
        continue;
      if (g_strcmp0 (calendar->uid, editor->calendar) == 0)
        selected = (int) labels->len;
      g_ptr_array_add (labels, calendar->account && *calendar->account
                               ? g_strdup_printf ("%s (%s)", calendar->name, calendar->account)
                               : g_strdup (calendar->name));
    }
  cal_popup_show (GTK_WIDGET (row), (const char *const *) labels->pdata, (int) labels->len, selected, 0, FALSE,
                  on_calendar_chosen, editor);
}

static void
draw_calendar_color (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  GdkRGBA *color = g_object_get_data (G_OBJECT (area), "cal-color");

  if (!color)
    return;
  gdk_cairo_set_source_rgba (cr, color);
  cairo_rectangle (cr, 0, 0, width, height);
  cairo_fill (cr);
}

/* --- dates and times (EditEventView.DateListener, TimeListener) */

static void
on_all_day (GObject *object, GParamSpec *pspec, Editor *editor)
{
  gboolean all_day = neo_switch_get_active (NEO_SWITCH (object));

  if (all_day == editor->all_day)
    return;
  editor->all_day = all_day;
  if (all_day)
    {
      /* an end at midnight is the day before's end */
      if (editor->end_minute == 0 && editor->end_day > editor->start_day)
        editor->end_day--;
    }
  else if (editor->end_minute == 0 && editor->end_day >= editor->start_day
           && !(editor->end_day == editor->start_day && editor->end_minute > editor->start_minute))
    {
      /* back to times: the end at midnight after the last day */
      if (editor->start_minute == 0)
        editor->end_day++;
    }
  if (editor->end_day < editor->start_day)
    editor->end_day = editor->start_day;
  show_times (editor);
}

static void
on_start_date (int day, gpointer data)
{
  Editor *editor = data;
  int shift = day - editor->start_day;

  /* the end keeps the duration */
  editor->start_day = day;
  editor->end_day += shift;
  show_times (editor);
  show_repeat (editor);
}

static void
on_end_date (int day, gpointer data)
{
  Editor *editor = data;

  /* an end before the start is the start's date */
  editor->end_day = MAX (day, editor->start_day);
  if (editor->end_day == editor->start_day && !editor->all_day && editor->end_minute < editor->start_minute)
    editor->end_minute = editor->start_minute;
  show_times (editor);
}

static void
on_start_minute (int hour, int minute, gpointer data)
{
  Editor *editor = data;
  gint64 duration = local_time (editor->end_day, editor->end_minute)
                    - local_time (editor->start_day, editor->start_minute);
  gint64 end;

  editor->start_minute = hour * 60 + minute;
  end = local_time (editor->start_day, editor->start_minute) + duration;
  split_time (end, &editor->end_day, &editor->end_minute);
  show_times (editor);
}

static void
on_end_minute (int hour, int minute, gpointer data)
{
  Editor *editor = data;

  editor->end_minute = hour * 60 + minute;
  /* an end time before the start on the same day: the next day */
  if (editor->end_day == editor->start_day && editor->end_minute < editor->start_minute)
    editor->end_day++;
  show_times (editor);
}

static int
week_start (Editor *editor)
{
  /* MaterialDatePicker starts the week on the locale's first day */
  return cal_week_start (-1);
}

/* EditEventView titles its date pickers "Go to…" as well (R.string.goto_date, :1681) */
static void
on_start_date_clicked (NeoPressable *button, Editor *editor)
{
  cal_date_picker_show (editor->page, _("Go to…"), editor->start_day, week_start (editor), on_start_date,
                        editor);
}

static void
on_end_date_clicked (NeoPressable *button, Editor *editor)
{
  cal_date_picker_show (editor->page, _("Go to…"), editor->end_day, week_start (editor), on_end_date, editor);
}

static void
on_start_time_clicked (NeoPressable *button, Editor *editor)
{
  cal_time_picker_show (editor->page, editor->start_minute / 60, editor->start_minute % 60, on_start_minute, editor);
}

static void
on_end_time_clicked (NeoPressable *button, Editor *editor)
{
  cal_time_picker_show (editor->page, editor->end_minute / 60, editor->end_minute % 60, on_end_minute, editor);
}

/* --- repeat */

static const char *byday[] = { "SU", "MO", "TU", "WE", "TH", "FR", "SA" };

typedef struct {
  Editor *editor;
  GPtrArray *rules;             /* the rule each choice sets (NULL: one-time) */
} RepeatChoice;

static void
on_repeat_chosen (int index, gpointer data)
{
  RepeatChoice *choice = data;
  Editor *editor = choice->editor;

  if (index >= 0)
    {
      g_free (editor->rrule);
      editor->rrule = g_strdup (choice->rules->pdata[index]);
      show_repeat (editor);
    }
  g_ptr_array_unref (choice->rules);
  g_free (choice);
}

static void
on_repeat (NeoPressable *button, Editor *editor)
{
  RepeatChoice *choice = g_new0 (RepeatChoice, 1);
  g_autoptr(GPtrArray) labels = g_ptr_array_new_with_free_func (g_free);
  gint64 start = start_time (editor);
  const char *rules[] = { NULL, "FREQ=DAILY", NULL, "FREQ=MONTHLY", "FREQ=YEARLY" };
  g_autofree char *weekly = g_strdup_printf ("FREQ=WEEKLY;BYDAY=%s", byday[cal_day_of_week (editor->start_day)]);
  gboolean common = editor->rrule == NULL;

  choice->editor = editor;
  choice->rules = g_ptr_array_new_with_free_func (g_free);
  rules[2] = weekly;
  for (guint i = 0; i < G_N_ELEMENTS (rules); i++)
    {
      g_ptr_array_add (choice->rules, g_strdup (rules[i]));
      g_ptr_array_add (labels, rules[i] ? cal_format_rrule (rules[i], start) : g_strdup (_("One-time event")));
      common = common || g_strcmp0 (editor->rrule, rules[i]) == 0;
    }
  /* a rule made elsewhere: offered first, as it is */
  if (!common)
    {
      g_autofree char *text = cal_format_rrule (editor->rrule, start);

      g_ptr_array_insert (choice->rules, 0, g_strdup (editor->rrule));
      g_ptr_array_insert (labels, 0, g_strdup (text ? text : _("Custom repeat")));
    }
  cal_choice_dialog (editor->page, _("Repeat"), (const char *const *) labels->pdata, (int) labels->len,
                     on_repeat_chosen, choice);
}

/* --- reminders */

static void
on_reminders_changed (CalReminders *reminders, Editor *editor)
{
  gtk_widget_set_visible (editor->add_reminder, cal_reminders_count (reminders) < CAL_MAX_REMINDERS);
}

static void
on_add_reminder (NeoPressable *button, Editor *editor)
{
  int minutes = g_settings_get_int (editor->settings, "default-reminder");

  cal_reminders_add (CAL_REMINDERS (editor->reminders), minutes >= 0 ? minutes : 10);
}

/* --- building it */

/* the description's hint shows while it is empty */
static void
on_description_changed (GtkTextBuffer *buffer, GtkWidget *hint)
{
  gtk_widget_set_visible (hint, gtk_text_buffer_get_char_count (buffer) == 0);
}

static GtkWidget *
separator (void)
{
  GtkWidget *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

  gtk_widget_add_css_class (line, "cal-edit-separator");
  return line;
}

/* a row: the 24 dp icon 12 dp in, its content from 48 dp */
static GtkWidget *
row (const char *icon_name, GtkWidget *content)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *icon = gtk_image_new_from_icon_name (icon_name);

  gtk_image_set_pixel_size (GTK_IMAGE (icon), 24);
  gtk_widget_add_css_class (icon, "cal-edit-icon");
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), icon);
  gtk_widget_set_hexpand (content, TRUE);
  gtk_box_append (GTK_BOX (box), content);
  gtk_widget_add_css_class (box, "cal-edit-row");
  return box;
}

/* an M3 outlined button: a pill, its text in colorPrimary */
static GtkWidget *
outlined_button (const char *name, GCallback callback, gpointer data)
{
  GtkWidget *label = gtk_label_new (NULL);
  GtkWidget *button = neo_pressable_new (label);

  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (button, "cal-outlined");
  neo_pressable_set_ripple (NEO_PRESSABLE (button), NEO_RIPPLE_BOUNDED, 20);
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, name, -1);
  gtk_widget_set_halign (button, GTK_ALIGN_START);
  g_signal_connect (button, "clicked", callback, data);
  return button;
}

/* an M3 outlined text field with its hint */
static GtkWidget *
text_field (const char *hint, const char *text)
{
  GtkWidget *entry = gtk_entry_new ();

  gtk_entry_set_placeholder_text (GTK_ENTRY (entry), hint);
  gtk_editable_set_text (GTK_EDITABLE (entry), text ? text : "");
  gtk_widget_add_css_class (entry, "cal-text-field");
  return entry;
}

static GtkWidget *
action_button (const char *icon_name, const char *label, GCallback callback, Editor *editor)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *icon = gtk_image_new_from_icon_name (icon_name);
  GtkWidget *text = gtk_label_new (label);
  GtkWidget *button;

  gtk_image_set_pixel_size (GTK_IMAGE (icon), 36);     /* ic_menu_cancel, ic_menu_done: 36 dp */
  gtk_widget_add_css_class (icon, "cal-edit-action-icon");
  gtk_widget_add_css_class (text, "cal-edit-action-text");
  gtk_box_append (GTK_BOX (box), icon);
  gtk_box_append (GTK_BOX (box), text);
  gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
  button = neo_pressable_new (box);
  gtk_widget_set_hexpand (button, TRUE);
  neo_pressable_set_ripple (NEO_PRESSABLE (button), NEO_RIPPLE_BOUNDED, 0);
  g_signal_connect (button, "clicked", callback, editor);
  return button;
}

static GtkWidget *
title_bar (Editor *editor)
{
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *divider = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *overlay = gtk_overlay_new ();
  GtkWidget *back = neo_icon_button_new ("cal-back-symbolic");

  /* edit_event_custom_actionbar.xml: "✕ CANCEL" and "✓ DONE" in two halves, a divider between */
  gtk_box_append (GTK_BOX (bar), action_button ("cal-cancel-symbolic", _("CANCEL"), G_CALLBACK (on_cancel), editor));
  gtk_widget_add_css_class (divider, "cal-edit-action-divider");
  gtk_box_append (GTK_BOX (bar), divider);
  gtk_box_append (GTK_BOX (bar), action_button ("cal-done-symbolic", _("DONE"), G_CALLBACK (on_done), editor));
  gtk_widget_add_css_class (bar, "cal-edit-bar");
  gtk_overlay_set_child (GTK_OVERLAY (overlay), bar);
  /* An invisible "Back" for the shell's back gesture, which presses the button of that name in the top band (Etar's
   * Back here is Cancel's: ask before throwing changes away). Not drawn: the bar has no arrow. */
  gtk_accessible_update_property (GTK_ACCESSIBLE (back), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Back"), -1);
  gtk_widget_set_opacity (back, 0);
  gtk_widget_set_halign (back, GTK_ALIGN_START);
  gtk_widget_set_valign (back, GTK_ALIGN_START);
  gtk_widget_set_can_target (back, FALSE);
  g_signal_connect (back, "clicked", G_CALLBACK (on_cancel), editor);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), back);
  return overlay;
}

static void
build (Editor *editor)
{
  GtkWidget *page = editor->page, *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *box, *label, *calendar_text, *dates, *line, *description_frame;
  CalEvent *source = editor->source;
  gboolean new_event = editor->original == NULL;

  gtk_box_append (GTK_BOX (page), title_bar (editor));

  /* the title: "Event name", 20 sp, from 48 dp */
  editor->title = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (editor->title), _("Event name"));
  gtk_editable_set_text (GTK_EDITABLE (editor->title), source && source->title ? source->title : "");
  gtk_widget_add_css_class (editor->title, "cal-edit-title");
  gtk_box_append (GTK_BOX (content), editor->title);
  gtk_box_append (GTK_BOX (content), separator ());

  /* the calendar: chosen for a new event; an existing one stays in its calendar (Etar shows it as text) */
  calendar_text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  editor->calendar_name = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (editor->calendar_name), 0);
  gtk_label_set_ellipsize (GTK_LABEL (editor->calendar_name), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (editor->calendar_name, "cal-edit-calendar-name");
  editor->calendar_account = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (editor->calendar_account), 0);
  gtk_label_set_ellipsize (GTK_LABEL (editor->calendar_account), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (editor->calendar_account, "cal-edit-calendar-account");
  /* textAppearanceMedium over textAppearanceSmall: 24.3 dp and 19 dp with the font padding, 43 dp in all */
  neo_label_set_font_padding (editor->calendar_name, 18);
  neo_label_set_font_padding (editor->calendar_account, 14);
  gtk_box_append (GTK_BOX (calendar_text), editor->calendar_name);
  gtk_box_append (GTK_BOX (calendar_text), editor->calendar_account);
  {
    GtkWidget *icon_box = gtk_overlay_new ();
    GtkWidget *icon = gtk_image_new_from_icon_name ("cal-calendar-symbolic");

    gtk_image_set_pixel_size (GTK_IMAGE (icon), 24);
    gtk_widget_add_css_class (icon, "cal-edit-icon");
    gtk_overlay_set_child (GTK_OVERLAY (icon_box), icon);
    /* calendar_selector_group_background: the calendar's colour in a 12 dp square at the icon's corner */
    editor->calendar_color = gtk_drawing_area_new ();
    gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (editor->calendar_color), draw_calendar_color, NULL, NULL);
    gtk_widget_set_size_request (editor->calendar_color, 12, 12);
    gtk_widget_set_halign (editor->calendar_color, GTK_ALIGN_END);
    gtk_widget_set_valign (editor->calendar_color, GTK_ALIGN_END);
    gtk_widget_add_css_class (editor->calendar_color, "cal-edit-calendar-color");
    gtk_overlay_add_overlay (GTK_OVERLAY (icon_box), editor->calendar_color);
    gtk_widget_set_valign (icon_box, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class (icon_box, "cal-edit-calendar-icon");
    box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append (GTK_BOX (box), icon_box);
    gtk_widget_set_hexpand (calendar_text, TRUE);
    gtk_box_append (GTK_BOX (box), calendar_text);
    gtk_widget_add_css_class (box, "cal-edit-row");
    gtk_widget_add_css_class (box, "cal-edit-calendar");
  }
  if (new_event)
    {
      editor->calendar_row = neo_pressable_new (box);
      neo_pressable_set_ripple (NEO_PRESSABLE (editor->calendar_row), NEO_RIPPLE_BOUNDED, 0);
      gtk_accessible_update_property (GTK_ACCESSIBLE (editor->calendar_row), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      _("Calendar"), -1);
      g_signal_connect (editor->calendar_row, "clicked", G_CALLBACK (on_calendar), editor);
    }
  else
    editor->calendar_row = box;
  gtk_box_append (GTK_BOX (content), editor->calendar_row);
  gtk_box_append (GTK_BOX (content), separator ());

  /* All day, and the dates and times */
  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  label = gtk_label_new (_("All day"));
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_add_css_class (label, "cal-edit-label");
  gtk_box_append (GTK_BOX (box), label);
  editor->all_day_switch = neo_switch_new ();
  neo_switch_set_thumb_icons (NEO_SWITCH (editor->all_day_switch), FALSE);
  neo_switch_set_active (NEO_SWITCH (editor->all_day_switch), editor->all_day);
  gtk_accessible_update_property (GTK_ACCESSIBLE (editor->all_day_switch), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _("All day event"), -1);
  g_signal_connect (editor->all_day_switch, "notify::active", G_CALLBACK (on_all_day), editor);
  {
    /* The row is the icon's 24 dp line (16 dp under the separator, the buttons 8 dp under it); the 32 dp switch
     * overhangs it, centred a dp under the line's middle, 24 dp from the end. An overlay, so it adds no height. */
    GtkWidget *overlay = gtk_overlay_new ();

    neo_label_set_font_padding (label, 18);
    line = row ("cal-schedule-symbolic", box);
    gtk_widget_add_css_class (line, "cal-edit-all-day");
    gtk_overlay_set_child (GTK_OVERLAY (overlay), line);
    gtk_widget_set_halign (editor->all_day_switch, GTK_ALIGN_END);
    gtk_widget_set_valign (editor->all_day_switch, GTK_ALIGN_START);
    gtk_widget_add_css_class (editor->all_day_switch, "cal-edit-all-day-switch");
    gtk_overlay_add_overlay (GTK_OVERLAY (overlay), editor->all_day_switch);
    gtk_box_append (GTK_BOX (content), overlay);
  }

  dates = gtk_grid_new ();
  gtk_widget_add_css_class (dates, "cal-edit-dates");
  editor->start_date = outlined_button (_("Start date"), G_CALLBACK (on_start_date_clicked), editor);
  editor->start_time = outlined_button (_("Start time"), G_CALLBACK (on_start_time_clicked), editor);
  editor->end_date = outlined_button (_("End date"), G_CALLBACK (on_end_date_clicked), editor);
  editor->end_time = outlined_button (_("End time"), G_CALLBACK (on_end_time_clicked), editor);
  gtk_grid_attach (GTK_GRID (dates), editor->start_date, 0, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (dates), editor->start_time, 1, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (dates), editor->end_date, 0, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (dates), editor->end_time, 1, 1, 1, 1);
  gtk_box_append (GTK_BOX (content), dates);
  gtk_box_append (GTK_BOX (content), separator ());

  /* location, description, URL */
  editor->location = text_field (_("Location"), source ? source->location : NULL);
  gtk_box_append (GTK_BOX (content), row ("cal-place-symbolic", editor->location));
  editor->description = gtk_text_view_new ();
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (editor->description), GTK_WRAP_WORD_CHAR);
  gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (editor->description)),
                            source && source->description ? source->description : "", -1);
  gtk_widget_add_css_class (editor->description, "cal-text-area");
  description_frame = gtk_overlay_new ();
  gtk_overlay_set_child (GTK_OVERLAY (description_frame), editor->description);
  {
    /* GtkTextView has no hint: the label shows while it is empty */
    GtkWidget *hint = gtk_label_new (_("Description"));
    GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (editor->description));

    gtk_label_set_xalign (GTK_LABEL (hint), 0);
    gtk_widget_set_can_target (hint, FALSE);
    gtk_widget_set_valign (hint, GTK_ALIGN_START);
    gtk_widget_add_css_class (hint, "cal-text-hint");
    g_signal_connect_object (buffer, "changed", G_CALLBACK (on_description_changed), hint, 0);
    gtk_overlay_add_overlay (GTK_OVERLAY (description_frame), hint);
    on_description_changed (buffer, hint);
  }
  gtk_widget_add_css_class (description_frame, "cal-text-field");
  gtk_box_append (GTK_BOX (content), row ("cal-notes-symbolic", description_frame));
  editor->url = text_field (_("URL"), source ? source->url : NULL);
  gtk_box_append (GTK_BOX (content), row ("cal-link-symbolic", editor->url));
  gtk_box_append (GTK_BOX (content), separator ());

  /* repeat: not for one occurrence of a series (it is an exception, it repeats nothing) */
  editor->repeat = outlined_button (_("Repeat event"), G_CALLBACK (on_repeat), editor);
  gtk_widget_set_hexpand (editor->repeat, TRUE);
  gtk_widget_set_halign (editor->repeat, GTK_ALIGN_FILL);
  editor->repeat_row = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (editor->repeat_row), row ("cal-repeat-symbolic", editor->repeat));
  gtk_box_append (GTK_BOX (editor->repeat_row), separator ());
  gtk_widget_set_visible (editor->repeat_row, !(editor->original && editor->scope == CAL_SCOPE_THIS
                                                && editor->original->recurring));
  gtk_box_append (GTK_BOX (content), editor->repeat_row);

  /* reminders */
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  editor->reminders = cal_reminders_new ();
  g_signal_connect (editor->reminders, "changed", G_CALLBACK (on_reminders_changed), editor);
  gtk_box_append (GTK_BOX (box), editor->reminders);
  label = gtk_label_new (_("Add reminder"));
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  editor->add_reminder = neo_pressable_new (label);
  gtk_widget_add_css_class (editor->add_reminder, "cal-add-reminder");
  neo_pressable_set_ripple (NEO_PRESSABLE (editor->add_reminder), NEO_RIPPLE_BOUNDED, 4);
  g_signal_connect (editor->add_reminder, "clicked", G_CALLBACK (on_add_reminder), editor);
  gtk_box_append (GTK_BOX (box), editor->add_reminder);
  line = row ("cal-notifications-symbolic", box);
  gtk_widget_add_css_class (line, "cal-edit-reminders");
  gtk_box_append (GTK_BOX (content), line);

  editor->scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (editor->scroll), content);
  gtk_widget_set_vexpand (editor->scroll, TRUE);
  gtk_box_append (GTK_BOX (page), editor->scroll);
}

/* the cursor at the title's end, nothing selected (an EditText's focus selects nothing; GtkText's selects all) */
static void
focus_title (GtkWidget *title)
{
  gtk_widget_grab_focus (title);
  gtk_editable_set_position (GTK_EDITABLE (title), -1);
}

static GtkWidget *
editor_page (GtkWidget *window, CalEvent *original, CalEvent *source, gint64 start, gboolean all_day,
             CalScope scope)
{
  Editor *editor = g_new0 (Editor, 1);
  GtkWidget *page = cal_page_new (window);
  GSettings *settings = cal_window_get_settings (window);

  editor->page = page;
  editor->window = window;
  editor->store = g_object_ref (cal_window_get_store (window));
  editor->settings = g_object_ref (settings);
  editor->original = original ? cal_event_ref (original) : NULL;
  editor->source = source ? cal_event_ref (source) : NULL;
  editor->scope = scope;
  g_object_set_data_full (G_OBJECT (page), "cal-editor", editor, editor_free);
  cal_page_set_back_handler (CAL_PAGE (page), on_back, editor);
  gtk_widget_add_css_class (page, "cal-editor");

  if (source)
    {
      const CalCalendar *calendar = cal_store_find_calendar (editor->store, source->calendar);

      editor->all_day = source->all_day;
      split_time (source->start, &editor->start_day, &editor->start_minute);
      split_time (source->end, &editor->end_day, &editor->end_minute);
      if (source->all_day)
        editor->end_day = MAX (editor->start_day, cal_event_last_day (source));
      editor->rrule = g_strdup (source->rrule);
      /* a copy goes to a calendar it can be written to */
      editor->calendar = g_strdup (calendar && (!calendar->readonly || original) ? calendar->uid : NULL);
    }
  else
    {
      /* resetToDefaultDuration: the default length from the start */
      gint64 end = start + (all_day ? 24 * 3600 : g_settings_get_int (settings, "default-event-duration") * 60);

      editor->all_day = all_day;
      split_time (start, &editor->start_day, &editor->start_minute);
      split_time (end, &editor->end_day, &editor->end_minute);
      if (all_day)
        editor->end_day = editor->start_day;
    }
  if (!editor->calendar)
    {
      g_autofree char *last = g_settings_get_string (settings, "last-calendar");
      const CalCalendar *calendar = cal_store_default_calendar (editor->store, last);

      editor->calendar = g_strdup (calendar ? calendar->uid : NULL);
    }

  build (editor);
  if (source)
    cal_reminders_set (CAL_REMINDERS (editor->reminders), source->reminders);
  else
    {
      /* a new event: one reminder at the default time ("None" is −1) */
      int minutes = g_settings_get_int (settings, "default-reminder");

      if (minutes >= 0)
        {
          g_autoptr(GArray) reminders = g_array_new (FALSE, FALSE, sizeof (CalReminder));
          CalReminder reminder = { minutes, CAL_METHOD_NOTIFICATION };

          g_array_append_val (reminders, reminder);
          cal_reminders_set (CAL_REMINDERS (editor->reminders), reminders);
        }
    }
  on_reminders_changed (CAL_REMINDERS (editor->reminders), editor);
  show_calendar (editor);
  show_times (editor);
  show_repeat (editor);
  /* Etar focuses the title of a new event only (an existing one opens with nothing focused: cal_window_push) */
  if (!original)
    g_signal_connect_object (page, "map", G_CALLBACK (focus_title), editor->title, G_CONNECT_SWAPPED);
  return page;
}

/* --- entry points */

typedef struct {
  GtkWidget *window, *placeholder;
  CalEvent *event;
} Which;

static void
on_which (int index, gpointer data)
{
  Which *which = data;
  /* EditEventFragment: modify_event, modify_all_following, modify_all */
  CalScope scopes[] = { CAL_SCOPE_THIS, CAL_SCOPE_THIS_AND_FUTURE, CAL_SCOPE_ALL };
  GtkWidget *window = which->window;

  /* the editor stood behind the question: Cancel closes it, a choice fills it in */
  cal_window_pop (window);
  if (index >= 0)
    cal_window_push (window, editor_page (window, which->event, which->event, which->event->start,
                                          which->event->all_day, scopes[index]));
  cal_event_unref (which->event);
  g_object_unref (which->placeholder);
  g_free (which);
}

static GtkWidget *
ask_which (GtkWidget *window, CalEvent *event)
{
  static const char *labels[] = { N_("Change only this event"), N_("Change this and all future events"),
                                  N_("Change all events in the series") };
  const char *translated[] = { _(labels[0]), _(labels[1]), _(labels[2]) };
  Which *which = g_new0 (Which, 1);
  GtkWidget *placeholder = cal_page_new (window);

  /* displayEditWhichDialog: asked before the editor shows anything */
  which->window = window;
  which->placeholder = g_object_ref (placeholder);
  which->event = cal_event_ref (event);
  cal_choice_dialog (window, _("Details"), translated, 3, on_which, which);
  return placeholder;
}

GtkWidget *
event_editor_new (GtkWidget *window, CalEvent *event, gint64 start, gboolean all_day, gboolean duplicate)
{
  if (!event)
    return editor_page (window, NULL, NULL, start, all_day, CAL_SCOPE_ALL);
  if (duplicate)
    return editor_page (window, NULL, event, event->start, event->all_day, CAL_SCOPE_ALL);
  if (event->recurring)
    return ask_which (window, event);
  return editor_page (window, event, event, event->start, event->all_day, CAL_SCOPE_ALL);
}

void
event_editor_import (GtkWidget *window, GFile *file)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *contents = NULL;
  g_autoptr(ICalComponent) root = NULL;
  ICalComponent *vevent = NULL;
  CalCalendar dummy = { NULL, NULL, NULL, 0x3f51b5, FALSE, TRUE, TRUE };

  if (!g_file_load_contents (file, NULL, &contents, NULL, NULL, &error))
    {
      cal_window_toast (window, error->message);
      return;
    }
  root = i_cal_parser_parse_string (contents);
  if (root && i_cal_component_isa (root) == I_CAL_VEVENT_COMPONENT)
    vevent = g_object_ref (root);
  else if (root)
    vevent = i_cal_component_get_first_component (root, I_CAL_VEVENT_COMPONENT);
  if (!vevent)
    {
      /* ImportActivity's "no events" */
      cal_window_toast (window, _("Invalid iCalendar file"));
      return;
    }
  {
    g_autoptr(ECalComponent) comp = e_cal_component_new_from_icalcomponent (vevent);
    ECalComponentDateTime *dtstart = comp ? e_cal_component_get_dtstart (comp) : NULL;
    ECalComponentDateTime *dtend = comp ? e_cal_component_get_dtend (comp) : NULL;
    g_autoptr(CalEvent) event = NULL;

    if (!dtstart)
      {
        g_clear_pointer (&dtend, e_cal_component_datetime_free);
        cal_window_toast (window, _("Invalid iCalendar file"));
        return;
      }
    event = cal_event_from_component (comp, &dummy, e_cal_component_datetime_get_value (dtstart),
                                      dtend ? e_cal_component_datetime_get_value (dtend)
                                            : e_cal_component_datetime_get_value (dtstart));
    e_cal_component_datetime_free (dtstart);
    g_clear_pointer (&dtend, e_cal_component_datetime_free);
    /* a new event in the default calendar, with the file's contents */
    g_clear_pointer (&event->calendar, g_free);
    cal_window_push (window, editor_page (window, NULL, event, event->start, event->all_day, CAL_SCOPE_ALL));
  }
}
