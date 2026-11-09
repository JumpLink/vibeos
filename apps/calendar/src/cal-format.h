/* The calendar's dates and times as text: Android's DateUtils.formatDateRange and formatDateTime with the flags
 * Etar passes, and Etar's own phrases ("Today at …", "Tomorrow, …"). English patterns, translatable: the order of
 * day, month and year is the translation's to choose. GLib only (the reminder service uses it too). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* the system clock's 24-hour setting (org.gnome.desktop.interface clock-format; DateFormat.is24HourFormat) */
gboolean cal_format_uses_24h (void);

/* "09:00" or "9:00 AM" */
char *cal_format_time (gint64 time);
/* "10:00 – 11:00" (FORMAT_SHOW_TIME): the en dash between thin spaces, as ICU's interval format */
char *cal_format_time_range (gint64 start, gint64 end);

char *cal_format_month_year (int day);      /* "November 2026" */
char *cal_format_full_date (int day);       /* "November 7, 2026" */
char *cal_format_month_day (int day);       /* "November 7" */
char *cal_format_weekday (int day);         /* "Saturday" */
char *cal_format_weekday_short (int day);   /* "Sat" */
char *cal_format_editor_date (int day);     /* "Sun, Nov 1, 2026" (EditEventView.setDate's flags) */
char *cal_format_picker_date (int day);     /* "Nov 1, 2026" (MaterialDatePicker's header) */

/* "Today, Saturday" / "Yesterday, Friday" / "Tomorrow, Sunday" / "Monday" (agenda and the toolbar's subtitle) */
char *cal_format_relative_weekday (int day, int today);

/* Utils.getDisplayedDatetime: an event's time in its info screen */
char *cal_format_event_when (gint64 start, gint64 end, gboolean all_day, gint64 now);

/* the time an agenda row shows (AgendaAdapter): "November 9" for an all-day one, "10:00 – 11:00" else; an
 * all-day event over several days "November 9 – 11" */
char *cal_format_agenda_when (gint64 start, gint64 end, gboolean all_day);

/* AlertUtils.formatTimeLocation: a reminder's text ("10:00", "Nov 9, 10:00", "10:00, Tomorrow", + ", place") */
char *cal_format_reminder_text (gint64 start, gboolean all_day, const char *location, gint64 now);

/* "10 minutes", "1 hour", "2 days", "1 week" (Etar's Nminutes … Nweeks plurals) */
char *cal_format_reminder_minutes (int minutes);

/* EventRecurrenceFormatter.getRepeatString: an RRULE in words ("Weekly on Monday", "Every 2 days; until 12/31/2026",
 * "Monthly (on every first Monday)"), or NULL for a rule it has no words for. @start is the first occurrence. */
char *cal_format_rrule (const char *rrule, gint64 start);

/* the first day of the week, 0 Sunday … 6 Saturday: @setting ("Week starts on": -1 the locale's, 7 Saturday,
 * 1 Sunday, 2 Monday, as Etar's preferences_week_start_day values), else the locale's (glibc's LC_TIME) */
int cal_week_start (int setting);
G_END_DECLS
