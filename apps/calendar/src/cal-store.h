/* CalStore: the phone's calendars, from evolution-data-server.
 *
 * It keeps the ESourceRegistry's calendars and an ECalClient for each one shown, reads the events of a time range
 * as occurrences (CalEvent), tells when anything changed (an ECalClientView per calendar), and writes events:
 * create, change, delete, each with "only this event / this and future events / all events" for a recurring
 * one.
 *
 * Everything that waits on EDS runs off the main loop (GTask threads around libecal's sync calls); the results and
 * the signals come back on the main loop. GLib, GIO and libecal only: the reminder service uses it too. */
#pragma once

#include "cal-model.h"

#include <gio/gio.h>
#include <libecal/libecal.h>

G_BEGIN_DECLS

/* libical-glib 3.0 and libecal 3.56 (Debian 13, the phone's) declare no g_autoptr cleanups for their types */
G_DEFINE_AUTOPTR_CLEANUP_FUNC (ICalComponent, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (ICalDuration, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (ICalProperty, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (ICalRecurrence, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (ICalTime, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (ICalTimezone, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (ECalComponent, g_object_unref)

#define CAL_TYPE_STORE (cal_store_get_type ())
G_DECLARE_FINAL_TYPE (CalStore, cal_store, CAL, STORE, GObject)

/* which occurrences of a recurring event a change or a delete takes */
typedef enum {
  CAL_SCOPE_THIS,               /* "Only this event": E_CAL_OBJ_MOD_THIS with its RECURRENCE-ID */
  CAL_SCOPE_THIS_AND_FUTURE,    /* "This and future events" */
  CAL_SCOPE_ALL,                /* "All events": the series */
} CalScope;

/* @hidden_too: connect the calendars that are not shown as well (the reminder service: a calendar's reminders ring
 * whether or not the app shows it) */
CalStore *cal_store_new (gboolean hidden_too);

/* the calendars (CalCalendar), by account and name; "calendars-changed" when any comes, goes or changes */
GPtrArray         *cal_store_get_calendars (CalStore *self);
const CalCalendar *cal_store_find_calendar (CalStore *self, const char *uid);
ECalClient        *cal_store_get_client    (CalStore *self, const char *uid);   /* connected, or NULL */
/* the calendar a new event goes to: @last_used if it is writable and shown, else the first such */
const CalCalendar *cal_store_default_calendar (CalStore *self, const char *last_used);
/* "Display calendar" (a calendar's page): ESourceSelectable:selected, written to the
 * source as GNOME Calendar does; "calendars-changed" follows once the registry has it */
void               cal_store_set_visible   (CalStore *self, const char *uid, gboolean visible);
/* EDS's registry (adding, changing and removing calendars), NULL until it has answered */
ESourceRegistry   *cal_store_get_registry  (CalStore *self);
/* after cal_set_home_zone: floating times and all-day dates are read in the new zone, and "changed" says so */
void               cal_store_zone_changed  (CalStore *self);
/* the registry has answered and every calendar has connected or failed */
gboolean           cal_store_is_ready      (CalStore *self);

/* the shown occurrences between @start and @end (Unix seconds), sorted by cal_event_compare; NULL on cancel.
 * "changed" says the answer may be different now: ask again. */
typedef void (*CalQueryDone) (GPtrArray *events, gpointer data);
void cal_store_query (CalStore *self, gint64 start, gint64 end, GCancellable *cancellable, CalQueryDone done,
                      gpointer data);

/* one event again, after it may have changed (event info re-reads it): the occurrence with @uid and @rid starting at
 * or after @near, or NULL if it is gone */
void       cal_store_find_event        (CalStore *self, const char *calendar, const char *uid, const char *rid,
                                        gint64 near, GCancellable *cancellable, GAsyncReadyCallback callback,
                                        gpointer data);
CalEvent  *cal_store_find_event_finish (CalStore *self, GAsyncResult *result, GError **error);

/* Write @edited (title, place, notes, URL, times, all-day, repeat rule, reminders, colour) to its calendar: a new
 * event when @original is NULL, else a change of @original's occurrence(s) per @scope. */
void     cal_store_save        (CalStore *self, CalEvent *original, CalEvent *edited, CalScope scope,
                                GAsyncReadyCallback callback, gpointer data);
gboolean cal_store_save_finish (CalStore *self, GAsyncResult *result, GError **error);

void     cal_store_delete        (CalStore *self, CalEvent *event, CalScope scope, GAsyncReadyCallback callback,
                                  gpointer data);
gboolean cal_store_delete_finish (CalStore *self, GAsyncResult *result, GError **error);

/* the event as an iCalendar file (Export, Share): VCALENDAR with the series and its time zones */
char *cal_store_export_sync (CalStore *self, CalEvent *event, GError **error);

/* --- for the reminder service and the tests */

/* a CalEvent from one occurrence of @comp (in @calendar) from @start to @end */
CalEvent *cal_event_from_component (ECalComponent *comp, const CalCalendar *calendar, ICalTime *start,
                                    ICalTime *end);
/* an ICalTime as Unix seconds: a DATE is its local midnight, a floating time is local */
gint64    cal_ical_time_to_unix    (ICalTime *time);

G_END_DECLS
