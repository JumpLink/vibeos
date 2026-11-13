/* When a call was, as the call log words it. */
#pragma once

#include <glib.h>

typedef enum {
  DAY_TODAY,
  DAY_YESTERDAY,
  DAY_OLDER,
} DayGroup;

/* the call log's day groups, by calendar days in the local time zone */
DayGroup    call_dates_day_group (gint64 date_ms, gint64 now_ms);
/* "Today", "Yesterday", "Older" */
const char *call_dates_day_group_name (DayGroup group);
/* relative, abbreviated, to the minute: "0 min. ago", "5 min. ago", "3 hr. ago", "Yesterday", "3 days ago", then
 * "October 28", "October 28, 2025" */
char       *call_dates_relative (gint64 date_ms, gint64 now_ms);
/* Call details' date: the weekday, date, year and time ("Wednesday, November 4, 2026 at
 * 04:36"), the time as the system clock is set */
char       *call_dates_details (gint64 date_ms);
/* a call's length: "12s", "2m 15s" */
char       *call_dates_duration (gint64 seconds);
