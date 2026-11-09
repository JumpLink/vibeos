/* The calendar's data and the rules the views share (cal-model.h). */
#include "cal-model.h"

#include <math.h>
#include <string.h>

void
cal_calendar_free (CalCalendar *calendar)
{
  g_free (calendar->uid);
  g_free (calendar->name);
  g_free (calendar->account);
  g_free (calendar);
}

CalEvent *
cal_event_new (void)
{
  CalEvent *event = g_new0 (CalEvent, 1);

  event->ref = 1;
  event->reminders = g_array_new (FALSE, FALSE, sizeof (CalReminder));
  event->max_columns = 1;
  return event;
}

CalEvent *
cal_event_ref (CalEvent *event)
{
  event->ref++;
  return event;
}

void
cal_event_unref (CalEvent *event)
{
  if (--event->ref > 0)
    return;
  g_free (event->calendar);
  g_free (event->uid);
  g_free (event->rid);
  g_free (event->title);
  g_free (event->location);
  g_free (event->description);
  g_free (event->url);
  g_free (event->rrule);
  g_free (event->tzid);
  g_array_unref (event->reminders);
  g_free (event);
}

gboolean
cal_event_draws_as_all_day (const CalEvent *event)
{
  /* Event.drawAsAllday: allDay || duration >= DateUtils.DAY_IN_MILLIS */
  return event->all_day || event->end - event->start >= 24 * 3600;
}

int
cal_event_first_day (const CalEvent *event)
{
  return cal_day_number (event->start);
}

int
cal_event_last_day (const CalEvent *event)
{
  /* the end is exclusive: an event to midnight, as every all-day one, ends on the day before */
  if (event->end <= event->start)
    return cal_day_number (event->start);
  return cal_day_number (event->end - 1);
}

int
cal_event_compare (gconstpointer a, gconstpointer b)
{
  const CalEvent *x = *(CalEvent * const *) a, *y = *(CalEvent * const *) b;

  if (x->start != y->start)
    return x->start < y->start ? -1 : 1;
  if (x->end != y->end)
    return x->end > y->end ? -1 : 1;
  return g_strcmp0 (x->title, y->title);
}

/* --- colours */

static gboolean real_event_colors;

void
cal_set_real_event_colors (gboolean real)
{
  real_event_colors = real;
}

guint32
cal_event_display_color (const CalEvent *event)
{
  return cal_display_color (event->color, real_event_colors);
}

guint32
cal_display_color (guint32 rgb, gboolean real_colors)
{
  double r = ((rgb >> 16) & 0xff) / 255.0, g = ((rgb >> 8) & 0xff) / 255.0, b = (rgb & 0xff) / 255.0;
  double max = MAX (r, MAX (g, b)), min = MIN (r, MIN (g, b));
  double h = 0, s = max > 0 ? (max - min) / max : 0, v = max;

  if (real_colors)
    return rgb & 0xffffff;
  /* Utils.getDisplayColorFromColor: Color.colorToHSV, s × 1.3 (max 1), v × 0.8, Color.HSVToColor */
  if (max > min)
    {
      if (max == r)
        h = fmod ((g - b) / (max - min), 6);
      else if (max == g)
        h = (b - r) / (max - min) + 2;
      else
        h = (r - g) / (max - min) + 4;
      h *= 60;
      if (h < 0)
        h += 360;
    }
  s = MIN (1.0, s * 1.3);
  v = v * 0.8;

  /* HSV back to RGB, as android.graphics.Color does (rounding each channel) */
  {
    double c = v * s, x = c * (1 - fabs (fmod (h / 60, 2) - 1)), m = v - c, rr, gg, bb;
    int sector = (int) (h / 60) % 6;

    switch (sector)
      {
      case 0: rr = c; gg = x; bb = 0; break;
      case 1: rr = x; gg = c; bb = 0; break;
      case 2: rr = 0; gg = c; bb = x; break;
      case 3: rr = 0; gg = x; bb = c; break;
      case 4: rr = x; gg = 0; bb = c; break;
      default: rr = c; gg = 0; bb = x; break;
      }
    return ((guint32) lround ((rr + m) * 255) << 16) | ((guint32) lround ((gg + m) * 255) << 8)
           | (guint32) lround ((bb + m) * 255);
  }
}

/* --- days */

int
cal_day_number (gint64 time)
{
  g_autoptr(GDateTime) local = g_date_time_new_from_unix_local (time);
  GDate date;

  g_date_clear (&date, 1);
  g_date_set_dmy (&date, g_date_time_get_day_of_month (local), g_date_time_get_month (local),
                  g_date_time_get_year (local));
  return (int) g_date_get_julian (&date);
}

gint64
cal_day_start (int day)
{
  GDate date;
  g_autoptr(GDateTime) midnight = NULL;

  g_date_clear (&date, 1);
  g_date_set_julian (&date, day);
  midnight = g_date_time_new_local (g_date_get_year (&date), g_date_get_month (&date), g_date_get_day (&date), 0, 0,
                                    0);
  return g_date_time_to_unix (midnight);
}

int
cal_day_of_week (int day)
{
  GDate date;

  g_date_clear (&date, 1);
  g_date_set_julian (&date, day);
  /* GDateWeekday is Monday 1 … Sunday 7; Android's Time.weekDay is Sunday 0 … Saturday 6 */
  return g_date_get_weekday (&date) % 7;
}

int
cal_minute_of_day (gint64 time)
{
  g_autoptr(GDateTime) local = g_date_time_new_from_unix_local (time);

  return g_date_time_get_hour (local) * 60 + g_date_time_get_minute (local);
}

/* --- layout */

/* removeNonAlldayActiveEvents / removeAlldayActiveEvents: the events of the active list that ended before @event
 * starts free their columns */
static guint64
remove_inactive (CalEvent *event, GPtrArray *active, gboolean all_day, gint64 min_seconds, guint64 mask)
{
  for (guint i = 0; i < active->len;)
    {
      CalEvent *other = active->pdata[i];
      gboolean over;

      if (all_day)
        over = cal_event_last_day (other) < cal_event_first_day (event);
      else
        over = other->start + MAX (other->end - other->start, min_seconds) <= event->start;
      if (over)
        {
          mask &= ~(G_GUINT64_CONSTANT (1) << other->column);
          g_ptr_array_remove_index (active, i);
        }
      else
        i++;
    }
  return mask;
}

/* computeGroupLayout, non-staggered: each event takes the lowest free column; the group shares the widest count */
static void
layout_group (GPtrArray *group, gboolean all_day, gint64 min_seconds)
{
  g_autoptr(GPtrArray) active = g_ptr_array_new ();
  guint64 mask = 0;
  int max_columns = 0;

  for (guint i = 0; i < group->len; i++)
    {
      CalEvent *event = group->pdata[i];
      int column;

      mask = remove_inactive (event, active, all_day, min_seconds, mask);
      g_ptr_array_add (active, event);
      for (column = 0; column < 64 && (mask & (G_GUINT64_CONSTANT (1) << column)); column++)
        ;
      /* "No column available. What do we do now? Use last column" (Event.java:572-576) */
      if (column == 64)
        column = 63;
      else
        mask |= G_GUINT64_CONSTANT (1) << column;
      event->column = column;
      max_columns = MAX (max_columns, column + 1);
    }
  for (guint i = 0; i < group->len; i++)
    ((CalEvent *) group->pdata[i])->max_columns = max_columns;
}

void
cal_layout_events (GPtrArray *events, gboolean all_day, gint64 min_seconds)
{
  g_autoptr(GPtrArray) group = g_ptr_array_new ();
  gint64 group_end = G_MININT64;
  int group_last_day = G_MININT;

  /* doComputePositions: events are grouped while they overlap the group so far */
  for (guint i = 0; i < events->len; i++)
    {
      CalEvent *event = events->pdata[i];
      gboolean separate;

      if (all_day)
        separate = group->len > 0 && cal_event_first_day (event) > group_last_day;
      else
        separate = group->len > 0 && event->start >= group_end;
      if (separate)
        {
          layout_group (group, all_day, min_seconds);
          g_ptr_array_set_size (group, 0);
          group_end = G_MININT64;
          group_last_day = G_MININT;
        }
      g_ptr_array_add (group, event);
      group_end = MAX (group_end, event->end);
      group_last_day = MAX (group_last_day, cal_event_last_day (event));
    }
  layout_group (group, all_day, min_seconds);
}
