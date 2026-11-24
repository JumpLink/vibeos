/* navi-plan: see navi-plan.h. */
#include "navi-plan.h"
#include "navi-settings.h"

#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <vibeos-locale.h>

#define UNIT        "vibeos-navi-leave" /* the timer's and its service's name */
#define WAKEUP_NAME "navi-leave"        /* our wake-up's key in vibeos-wakeup */
#define APP_ID      "vibeos.Navi"
#define GROUP       "planned"
#define LATE_S      3600                /* a reminder this late is no use any more; nor a plan this long gone */

static char *
plan_path (void)
{
  return g_build_filename (g_get_user_config_dir (), "vibeos-navi", "planned.ini", NULL);
}

NaviPlan *
navi_plan_load (void)
{
  g_autofree char *file = plan_path ();
  g_autoptr (GKeyFile) kf = g_key_file_new ();
  if (!g_key_file_load_from_file (kf, file, G_KEY_FILE_NONE, NULL))
    return NULL;
  gint64 time = g_key_file_get_int64 (kf, GROUP, "time", NULL);
  NaviPlace *place = navi_settings_read_place (kf, GROUP);
  if (!place || time <= 0) {
    g_clear_pointer (&place, navi_place_free);
    return NULL;
  }
  NaviPlan *plan = g_new0 (NaviPlan, 1);
  plan->place = place;
  plan->time = time;
  return plan;
}

void
navi_plan_free (NaviPlan *plan)
{
  if (!plan)
    return;
  navi_place_free (plan->place);
  g_free (plan);
}

static gint64
now_s (void)
{
  return g_get_real_time () / G_USEC_PER_SEC;
}

/* ---- the reminder: a systemd user timer, and the phone woken for it ---- */

/* vibeos-wakeup wakes a sleeping phone at `time` (0: not any more). Without it (not VibeOS, or not running) the
 * reminder comes when the phone wakes anyway. */
static void
set_wakeup (gint64 time)
{
  g_autoptr (GDBusConnection) system = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
  if (!system)
    return;
  GVariantBuilder options;
  g_variant_builder_init (&options, G_VARIANT_TYPE ("a{sv}"));
  if (time > 0)
    g_variant_builder_add (&options, "{sv}", "app", g_variant_new_string (APP_ID));
  g_dbus_connection_call (system, "vibeos.Wakeup", "/vibeos/Wakeup", "vibeos.Wakeup", "Set",
                          g_variant_new ("(sxa{sv})", WAKEUP_NAME, time, &options), NULL,
                          G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, NULL, NULL, NULL);
}

static GSubprocess *
spawn (const char *const *argv, GError **error)
{
  return g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_PIPE, error);
}

/* stops the timer (and with it the transient units), whether or not there is one */
static GSubprocess *
stop_timer (void)
{
  const char *argv[] = { "systemctl", "--user", "--quiet", "stop", UNIT ".timer", NULL };
  return spawn (argv, NULL);
}

typedef struct {
  gint64 time;
  NaviPlanDone done;
  gpointer data;
} Arm;

static void
arm_finish (Arm *arm, gboolean ok)
{
  if (ok)
    set_wakeup (arm->time);
  if (arm->done)
    arm->done (ok, arm->data);
  g_free (arm);
}

static void
run_done (GObject *src, GAsyncResult *res, gpointer data)
{
  Arm *arm = data;
  g_autofree char *err = NULL;
  g_autoptr (GError) error = NULL;
  g_subprocess_communicate_utf8_finish (G_SUBPROCESS (src), res, NULL, &err, &error);
  gboolean ok = !error && g_subprocess_get_if_exited (G_SUBPROCESS (src)) &&
                g_subprocess_get_exit_status (G_SUBPROCESS (src)) == 0;
  if (!ok)
    g_message ("plan: no reminder: %s", error ? error->message : err ? g_strstrip (err) : "systemd-run failed");
  g_object_unref (src);
  arm_finish (arm, ok);
}

/* the timer: `vibeos-navi --leave-reminder` at the plan's time, in the environment that finds this plan and speaks
 * this language */
static void
stopped_then_run (GObject *src, GAsyncResult *res, gpointer data)
{
  Arm *arm = data;
  if (src) {
    g_subprocess_wait_finish (G_SUBPROCESS (src), res, NULL);
    g_object_unref (src);
  }
  g_autofree char *exe = g_file_read_link ("/proc/self/exe", NULL);
  g_autoptr (GDateTime) at = g_date_time_new_from_unix_utc (arm->time);
  g_autofree char *when = g_date_time_format (at, "--on-calendar=%Y-%m-%d %H:%M:%S UTC");
  static const char *const command[] = { "systemd-run", "--user", "--quiet", "--collect", ("--unit=" UNIT),
                                         "--description=Navi: time to leave", "--timer-property=AccuracySec=1s" };
  g_autoptr (GPtrArray) argv = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < G_N_ELEMENTS (command); i++)
    g_ptr_array_add (argv, g_strdup (command[i]));
  g_ptr_array_add (argv, g_steal_pointer (&when));
  static const char *const env[] = { "XDG_CONFIG_HOME", "LANG", "LANGUAGE", "LC_ALL", "LC_MESSAGES" };
  for (guint i = 0; i < G_N_ELEMENTS (env); i++)
    if (g_getenv (env[i]))
      g_ptr_array_add (argv, g_strdup_printf ("--setenv=%s=%s", env[i], g_getenv (env[i])));
  g_ptr_array_add (argv, g_strdup ("--"));
  g_ptr_array_add (argv, g_strdup (exe ? exe : "vibeos-navi"));
  g_ptr_array_add (argv, g_strdup ("--leave-reminder"));
  g_ptr_array_add (argv, NULL);
  g_autoptr (GError) error = NULL;
  GSubprocess *run = spawn ((const char *const *) argv->pdata, &error);
  if (!run) {
    g_message ("plan: no reminder: %s", error->message);
    arm_finish (arm, FALSE);
    return;
  }
  g_subprocess_communicate_utf8_async (run, NULL, NULL, run_done, arm);
}

static void
arm (gint64 time, NaviPlanDone done, gpointer data)
{
  Arm *a = g_new0 (Arm, 1);
  a->time = time;
  a->done = done;
  a->data = data;
  /* a timer of the same name would make systemd-run fail: the last plan's goes first */
  GSubprocess *stop = stop_timer ();
  if (stop)
    g_subprocess_wait_async (stop, NULL, stopped_then_run, a);
  else
    stopped_then_run (NULL, NULL, a);
}

/* ---- the plan ---- */

void
navi_plan_set (const NaviPlace *place, gint64 time, NaviPlanDone done, gpointer data)
{
  g_autoptr (GKeyFile) kf = g_key_file_new ();
  g_key_file_set_int64 (kf, GROUP, "time", time);
  navi_settings_write_place (kf, GROUP, place);
  g_autofree char *file = plan_path ();
  g_autofree char *dir = g_path_get_dirname (file);
  g_mkdir_with_parents (dir, 0700);
  g_autoptr (GError) error = NULL;
  if (!g_key_file_save_to_file (kf, file, &error)) {
    g_message ("plan: %s", error->message);
    if (done)
      done (FALSE, data);
    return;
  }
  arm (time, done, data);
}

static void
waited (GObject *src, GAsyncResult *res, gpointer data)
{
  g_subprocess_wait_finish (G_SUBPROCESS (src), res, NULL);
  g_object_unref (src);
}

void
navi_plan_cancel (void)
{
  g_autofree char *file = plan_path ();
  g_unlink (file);
  GSubprocess *stop = stop_timer ();
  if (stop)
    g_subprocess_wait_async (stop, NULL, waited, NULL);
  set_wakeup (0);
}

void
navi_plan_rearm (void)
{
  g_autoptr (NaviPlan) plan = navi_plan_load ();
  if (!plan)
    return;
  gint64 now = now_s ();
  if (plan->time < now - LATE_S)
    navi_plan_cancel ();
  else if (plan->time > now)
    arm (plan->time, NULL, NULL);
}

/* ---- the reminder itself ---- */

int
navi_plan_remind (void)
{
  g_autoptr (NaviPlan) plan = navi_plan_load ();
  gint64 now = now_s ();
  /* cancelled, replaced by a later one, or come far too late (the phone was off) */
  if (!plan || now < plan->time - 60 || now > plan->time + LATE_S)
    return 0;
  g_autoptr (GApplication) app = g_application_new (APP_ID, G_APPLICATION_NON_UNIQUE);
  g_autoptr (GError) error = NULL;
  if (!g_application_register (app, NULL, &error)) {
    g_message ("plan: %s", error->message);
    return 1;
  }
  g_autofree char *title = g_strdup_printf (_("Time to leave for %s"), plan->place->name);
  g_autoptr (GNotification) n = g_notification_new (title);
  g_notification_set_body (n, _("Tap for the routes from where you are."));
  g_notification_set_priority (n, G_NOTIFICATION_PRIORITY_HIGH);
  /* the plan it is for: a tap on an older one's opens nothing */
  g_notification_set_default_action_and_target (n, "app.planned-drive", "x", plan->time);
  g_application_send_notification (app, "leave", n);
  /* it is a message on the bus, not yet sent when this returns */
  GDBusConnection *bus = g_application_get_dbus_connection (app);
  if (bus)
    g_dbus_connection_flush_sync (bus, NULL, NULL);
  return 0;
}

/* local midnight of `t`'s day */
static GDateTime *
day_of (GDateTime *t)
{
  return g_date_time_new_local (g_date_time_get_year (t), g_date_time_get_month (t), g_date_time_get_day_of_month (t),
                                0, 0, 0);
}

char *
navi_plan_describe (gint64 time, gboolean use_24h)
{
  g_autoptr (GDateTime) t = g_date_time_new_from_unix_local (time);
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *clock = vibeos_format_date (t, use_24h ? "Hm" : "hm");
  /* the hour as the clock shows it ("1" for 1:00 and for 1:00 PM): the strings' forms follow it, for the languages
   * whose words before a time change with its hour (Italian "all'1:00", "alle 2:00") */
  int h = g_date_time_get_hour (t);
  gulong hour = use_24h ? (gulong) h : (gulong) (h % 12 ? h % 12 : 12);
  g_autoptr (GDateTime) d0 = day_of (now);
  g_autoptr (GDateTime) d1 = day_of (t);
  /* rounded: a day with a clock change is 23 or 25 hours long */
  gint64 days = (g_date_time_difference (d1, d0) + G_TIME_SPAN_HOUR * 12) / G_TIME_SPAN_DAY;
  if (days == 0)
    /* TRANSLATORS: when the reminder to leave comes, "Reminder at 08:15"; the form follows the time's hour as shown
     * (the singular for 1:00), for words that change with it */
    return g_strdup_printf (g_dngettext (NULL, "Reminder at %s", "Reminder at %s", hour), clock);
  if (days == 1)
    /* TRANSLATORS: "Reminder tomorrow at 08:15"; the form follows the time's hour, as above */
    return g_strdup_printf (g_dngettext (NULL, "Reminder tomorrow at %s", "Reminder tomorrow at %s", hour), clock);
  g_autofree char *day = vibeos_format_date (t, "EEE");
  /* TRANSLATORS: the day (its short name) and time, "Reminder on Mon at 08:15"; the form follows the time's hour, as
   * above */
  return g_strdup_printf (g_dngettext (NULL, "Reminder on %s at %s", "Reminder on %s at %s", hour), day, clock);
}
