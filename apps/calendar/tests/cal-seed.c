/* cal-seed: test events for the Calendar, in the test rig's own evolution-data-server (try-calendar.sh), never in
 * anyone's real calendars.
 *
 *   cal-seed            a second local calendar ("Work"), and events around today in it and in "Personal"
 *   cal-seed alarm M    one more event, starting M minutes from now with a reminder at its start (the reminder
 *                       service's test)
 *
 * It refuses to run outside the rig: CAL_RIG_DIR must be set, the session bus must be the rig's (CAL_RIG_DIR/bus),
 * and XDG_CONFIG_HOME and XDG_DATA_HOME must be inside CAL_RIG_DIR, where the rig's EDS keeps its sources and
 * events. */
#include <libecal/libecal.h>
#include <string.h>

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ICalComponent, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (ICalTime, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (ICalTimezone, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (ECalClient, g_object_unref)

static gboolean
inside (const char *dir, const char *rig)
{
  g_autofree char *prefix = g_strconcat (rig, "/", NULL);

  return dir && g_str_has_prefix (dir, prefix);
}

static void
refuse_outside_rig (void)
{
  const char *rig = g_getenv ("CAL_RIG_DIR");
  g_autofree char *bus_file = rig ? g_build_filename (rig, "bus", NULL) : NULL;
  g_autofree char *bus = NULL;

  if (!rig || !g_file_get_contents (bus_file, &bus, NULL, NULL)
      || g_strcmp0 (g_strstrip (bus), g_getenv ("DBUS_SESSION_BUS_ADDRESS")) != 0
      || !inside (g_getenv ("XDG_CONFIG_HOME"), rig) || !inside (g_getenv ("XDG_DATA_HOME"), rig))
    {
      g_printerr ("cal-seed: not in the test rig (try-calendar.sh): refusing to touch these calendars\n");
      exit (2);
    }
}

static ICalTime *
local_time (GDateTime *day, int days, int hour, int minute, ICalTimezone *zone)
{
  g_autoptr(GDateTime) date = g_date_time_add_days (day, days);
  ICalTime *time = i_cal_time_new_null_time ();

  i_cal_time_set_date (time, g_date_time_get_year (date), g_date_time_get_month (date),
                       g_date_time_get_day_of_month (date));
  i_cal_time_set_time (time, hour, minute, 0);
  i_cal_time_set_timezone (time, zone);
  return time;
}

static ICalTime *
date_of (GDateTime *day, int days)
{
  g_autoptr(GDateTime) date = g_date_time_add_days (day, days);
  ICalTime *time = i_cal_time_new_null_time ();

  i_cal_time_set_date (time, g_date_time_get_year (date), g_date_time_get_month (date),
                       g_date_time_get_day_of_month (date));
  i_cal_time_set_is_date (time, TRUE);
  return time;
}

static void
add_alarm (ICalComponent *event, int minutes)
{
  ICalComponent *alarm = i_cal_component_new_valarm ();
  ICalDuration *before = i_cal_duration_new_from_int (-minutes * 60);
  ICalTrigger *trigger = i_cal_trigger_new_from_int (0);

  i_cal_trigger_set_duration (trigger, before);

  i_cal_component_take_property (alarm, i_cal_property_new_action (I_CAL_ACTION_DISPLAY));
  i_cal_component_take_property (alarm, i_cal_property_new_trigger (trigger));
  i_cal_component_take_property (alarm, i_cal_property_new_description ("Reminder"));
  i_cal_component_take_component (event, alarm);
  g_object_unref (trigger);
  g_object_unref (before);
}

static void
create (ECalClient *client, const char *summary, ICalTime *start, ICalTime *end, const char *location,
        const char *description, const char *rrule, int alarm_minutes)
{
  g_autoptr(ICalComponent) event = i_cal_component_new_vevent ();
  g_autofree char *uid = NULL;
  g_autoptr(GError) error = NULL;

  i_cal_component_set_summary (event, summary);
  i_cal_component_set_dtstart (event, start);
  i_cal_component_set_dtend (event, end);
  if (location)
    i_cal_component_set_location (event, location);
  if (description)
    i_cal_component_set_description (event, description);
  if (rrule)
    i_cal_component_take_property (event, i_cal_property_new_from_string (rrule));
  if (alarm_minutes >= 0)
    add_alarm (event, alarm_minutes);
  if (!e_cal_client_create_object_sync (client, event, E_CAL_OPERATION_FLAG_NONE, &uid, NULL, &error))
    g_printerr ("cal-seed: %s: %s\n", summary, error->message);
  else
    g_print ("%s\n", summary);
  g_object_unref (start);
  g_object_unref (end);
}

static ECalClient *
connect_to (ESource *source)
{
  g_autoptr(GError) error = NULL;
  EClient *client = e_cal_client_connect_sync (source, E_CAL_CLIENT_SOURCE_TYPE_EVENTS, 30, NULL, &error);

  if (!client)
    {
      g_printerr ("cal-seed: %s: %s\n", e_source_get_display_name (source), error->message);
      exit (1);
    }
  return E_CAL_CLIENT (client);
}

static ESource *
work_calendar (ESourceRegistry *registry)
{
  GList *sources = e_source_registry_list_sources (registry, E_SOURCE_EXTENSION_CALENDAR);
  ESource *work = NULL;
  g_autoptr(GError) error = NULL;

  for (GList *l = sources; l && !work; l = l->next)
    if (g_strcmp0 (e_source_get_display_name (l->data), "Work") == 0)
      work = g_object_ref (l->data);
  g_list_free_full (sources, g_object_unref);
  if (work)
    return work;

  work = e_source_new (NULL, NULL, NULL);
  e_source_set_parent (work, "local-stub");
  e_source_set_display_name (work, "Work");
  e_source_backend_set_backend_name (e_source_get_extension (work, E_SOURCE_EXTENSION_CALENDAR), "local");
  e_source_selectable_set_color (e_source_get_extension (work, E_SOURCE_EXTENSION_CALENDAR), "#16a765");
  if (!e_source_registry_commit_source_sync (registry, work, NULL, &error))
    {
      g_printerr ("cal-seed: Work: %s\n", error->message);
      exit (1);
    }
  /* the registry hands back its own object for the new source */
  for (int i = 0; i < 50; i++)
    {
      ESource *added = e_source_registry_ref_source (registry, e_source_get_uid (work));

      if (added)
        {
          g_object_unref (work);
          return added;
        }
      g_main_context_iteration (NULL, FALSE);
      g_usleep (100000);
    }
  return work;
}

int
main (int argc, char **argv)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(ESourceRegistry) registry = NULL;
  g_autoptr(ESource) personal = NULL, work = NULL;
  g_autoptr(ECalClient) personal_client = NULL, work_client = NULL;
  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  g_autoptr(GDateTime) today = g_date_time_new_local (g_date_time_get_year (now), g_date_time_get_month (now),
                                                      g_date_time_get_day_of_month (now), 0, 0, 0);
  ICalTimezone *zone;

  refuse_outside_rig ();
  registry = e_source_registry_new_sync (NULL, &error);
  if (!registry)
    {
      g_printerr ("cal-seed: %s\n", error->message);
      return 1;
    }
  personal = e_source_registry_ref_builtin_calendar (registry);
  personal_client = connect_to (personal);
  zone = e_cal_util_get_system_timezone ();

  if (argc == 3 && g_str_equal (argv[1], "alarm"))
    {
      g_autoptr(GDateTime) start = g_date_time_add_minutes (now, atoi (argv[2]));
      ICalTime *begin = i_cal_time_new_from_timet_with_zone (g_date_time_to_unix (start), FALSE, zone);
      ICalTime *end = i_cal_time_new_from_timet_with_zone (g_date_time_to_unix (start) + 1800, FALSE, zone);

      i_cal_time_set_second (begin, 0);
      i_cal_time_set_second (end, 0);
      create (personal_client, "Reminder test", begin, end, "Room 4", NULL, NULL, 0);
      return 0;
    }

  work = work_calendar (registry);
  work_client = connect_to (work);

  create (personal_client, "Dentist", local_time (today, 0, 9, 0, zone), local_time (today, 0, 10, 0, zone),
          "Hauptstraße 5", NULL, NULL, 30);
  create (personal_client, "Lunch with Anna", local_time (today, 0, 12, 30, zone),
          local_time (today, 0, 13, 30, zone), "Café Mitte", NULL, NULL, 10);
  create (personal_client, "Birthday party", date_of (today, 2), date_of (today, 3), NULL, NULL, NULL, -1);
  create (personal_client, "Trip to the sea", date_of (today, 4), date_of (today, 7), "Rostock", NULL, NULL, -1);
  create (personal_client, "Yoga", local_time (today, -2, 18, 0, zone), local_time (today, -2, 19, 0, zone), NULL,
          NULL, "RRULE:FREQ=WEEKLY;INTERVAL=1", 15);
  create (personal_client, "Old appointment", local_time (today, -9, 15, 0, zone),
          local_time (today, -9, 16, 0, zone), NULL, NULL, NULL, -1);
  create (work_client, "Standup", local_time (today, 0, 9, 30, zone), local_time (today, 0, 9, 45, zone), NULL,
          NULL, "RRULE:FREQ=WEEKLY;BYDAY=MO,TU,WE,TH,FR", 5);
  create (work_client, "Design review", local_time (today, 0, 9, 0, zone), local_time (today, 0, 11, 0, zone),
          "Meeting room 2", "Bring the printouts.\nThe agenda is in the shared folder.", NULL, 10);
  create (work_client, "Release day", date_of (today, 1), date_of (today, 2), NULL, NULL, NULL, -1);
  create (work_client, "Planning", local_time (today, 1, 14, 0, zone), local_time (today, 1, 15, 30, zone),
          "https://meet.example.org/planning", NULL, NULL, 10);
  create (work_client, "Late call", local_time (today, 3, 21, 0, zone), local_time (today, 3, 22, 0, zone), NULL,
          NULL, NULL, -1);
  create (work_client, "Monthly report", local_time (today, -5, 10, 0, zone), local_time (today, -5, 11, 0, zone),
          NULL, NULL, "RRULE:FREQ=MONTHLY;COUNT=6", 60);
  return 0;
}
