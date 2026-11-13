/* The alarm engine (alarm-engine.h): ported from DeskClock's, function for function where it can be. */
#include "alarm-engine.h"

/* an instance found a little after its time (a time change landed on it) still rings; later than this it was missed */
#define FIRE_BUFFER_SECONDS 15

struct _AlarmEngine {
  GObject parent_instance;

  AlarmStore *store;
  AlarmEngineSettings settings;
  GDateTime *(*now) (gpointer data);
  gpointer now_data;
};

G_DEFINE_FINAL_TYPE (AlarmEngine, alarm_engine, G_TYPE_OBJECT)

enum {
  STORE_CHANGED,                /* alarms or instances changed: save the store, refresh the app */
  INSTANCE_CHANGED,             /* (id, state): an instance's state is new; show it so (ring, notify) */
  INSTANCE_REMOVED,             /* (id): an instance is gone; take down whatever showed it */
  N_SIGNALS
};
static guint signals[N_SIGNALS];

static GDateTime *
real_now (gpointer data)
{
  return g_date_time_new_now_local ();
}

static GDateTime *
now (AlarmEngine *self)
{
  return self->now (self->now_data);
}

static void
store_changed (AlarmEngine *self)
{
  g_signal_emit (self, signals[STORE_CHANGED], 0);
}

static void set_state (AlarmEngine *self, AlarmInstance *instance, AlarmState state);
static void register_instance (AlarmEngine *self, AlarmInstance *instance);

/* --- instances */

/* the moment an instance rings out unanswered, or NULL if it rings until answered */
static GDateTime *
instance_timeout (AlarmEngine *self, AlarmInstance *instance)
{
  g_autoptr(GDateTime) time = NULL;

  if (self->settings.auto_silence_minutes < 0)
    return NULL;
  time = alarm_instance_time (instance);
  return g_date_time_add_minutes (time, self->settings.auto_silence_minutes);
}

static void
remove_instance (AlarmEngine *self, AlarmInstance *instance)
{
  guint id = instance->id;

  g_ptr_array_remove (self->store->instances, instance);
  g_signal_emit (self, signals[INSTANCE_REMOVED], 0, id);
}

static gboolean
same_time (const AlarmInstance *a, const AlarmInstance *b)
{
  return a->year == b->year && a->month == b->month && a->day == b->day && a->hour == b->hour
         && a->minute == b->minute;
}

/* one instance per alarm and time. One at the same time as an existing instance of its alarm takes that one's place:
 * the existing instance gets the new one's settings and state, and keeps its id (and its address, which a caller may
 * hold) */
static AlarmInstance *
add_instance (AlarmEngine *self, AlarmInstance *instance)
{
  for (guint i = 0; i < self->store->instances->len; i++)
    {
      AlarmInstance *other = g_ptr_array_index (self->store->instances, i);

      if (other->alarm_id == instance->alarm_id && same_time (other, instance))
        {
          AlarmInstance old = *other;

          *other = *instance;
          other->id = old.id;
          *instance = old;
          alarm_instance_free (instance);
          return other;
        }
    }
  instance->id = self->store->next_id++;
  g_ptr_array_add (self->store->instances, instance);
  return instance;
}

/* the previous time the alarm went off before @time, for a repeating alarm */
static GDateTime *
previous_alarm_time (const Alarm *alarm, GDateTime *time)
{
  if (!alarm->days)
    return NULL;
  /* the first time after a moment 8 days before is more than a day before @time (it repeats at least weekly); walk
   * on to the last one before @time */
  g_autoptr(GDateTime) before = g_date_time_add_days (time, -8);
  GDateTime *previous = alarm_next_time (alarm, before);

  while (TRUE)
    {
      g_autoptr(GDateTime) next = alarm_next_time (alarm, previous);

      if (g_date_time_compare (next, time) >= 0)
        return previous;
      g_date_time_unref (previous);
      previous = g_steal_pointer (&next);
    }
}

/* the alarm after one of its instances is over or missed: a repeating alarm makes its next instance, one that rings
 * once switches off, or goes if it was made to be used once */
static void
update_parent_alarm (AlarmEngine *self, AlarmInstance *instance)
{
  Alarm *alarm = alarm_store_find_alarm (self->store, instance->alarm_id);
  g_autoptr(GDateTime) current = now (self);
  AlarmInstance *next;

  if (!alarm)
    return;
  if (!alarm->days)
    {
      if (alarm->delete_after_use)
        {
          /* its instances stay (a missed one is still shown) without it */
          for (guint i = 0; i < self->store->instances->len; i++)
            {
              AlarmInstance *other = g_ptr_array_index (self->store->instances, i);

              if (other->alarm_id == alarm->id)
                other->alarm_id = 0;
            }
          g_ptr_array_remove (self->store->alarms, alarm);
        }
      else
        alarm->enabled = FALSE;
      store_changed (self);
      return;
    }

  /* the next time after now, which may be before this instance after a time jump; when that is this very instance,
   * already rung (missed, dismissed), the one after it */
  next = alarm_instance_after (alarm, current);
  if (instance->state > ALARM_STATE_FIRED && same_time (next, instance))
    {
      g_autoptr(GDateTime) time = alarm_instance_time (instance);

      alarm_instance_free (next);
      next = alarm_instance_after (alarm, time);
    }
  next = add_instance (self, next);
  register_instance (self, next);
}

/* the instance goes, and its alarm moves on (update_parent_alarm) */
static void
delete_instance_and_update_parent (AlarmEngine *self, AlarmInstance *instance)
{
  g_autoptr(AlarmInstance) gone = alarm_instance_copy (instance);

  remove_instance (self, instance);
  if (gone->alarm_id)
    update_parent_alarm (self, gone);
  store_changed (self);
}

/* --- the states */

static void
set_state (AlarmEngine *self, AlarmInstance *instance, AlarmState state)
{
  instance->state = state;
  g_signal_emit (self, signals[INSTANCE_CHANGED], 0, instance->id, state);
  store_changed (self);
}

static void
set_missed_state (AlarmEngine *self, AlarmInstance *instance)
{
  /* the alarm goes on (its next instance) before this one is marked missed */
  if (instance->alarm_id)
    update_parent_alarm (self, instance);
  set_state (self, instance, ALARM_STATE_MISSED);
}

static void
set_fired_state (AlarmEngine *self, AlarmInstance *instance)
{
  /* one alarm rings at a time: one still ringing is missed */
  for (guint i = 0; i < self->store->instances->len; i++)
    {
      AlarmInstance *other = g_ptr_array_index (self->store->instances, i);

      if (other != instance && other->state == ALARM_STATE_FIRED)
        set_missed_state (self, other);
    }
  /* a time pushed back may have brought a missed instance back to ringing: its alarm's other instances go */
  for (guint i = self->store->instances->len; i-- > 0;)
    {
      AlarmInstance *other = g_ptr_array_index (self->store->instances, i);

      if (other != instance && instance->alarm_id && other->alarm_id == instance->alarm_id)
        remove_instance (self, other);
    }
  set_state (self, instance, ALARM_STATE_FIRED);
}

static void
set_dismiss_state (AlarmEngine *self, AlarmInstance *instance)
{
  instance->state = ALARM_STATE_DISMISSED;
  delete_instance_and_update_parent (self, instance);
}

static void
set_predismiss_state (AlarmEngine *self, AlarmInstance *instance)
{
  set_state (self, instance, ALARM_STATE_PREDISMISSED);
  /* a repeating alarm's next instance is made now, so the next alarm shows the one after this */
  if (instance->alarm_id)
    update_parent_alarm (self, instance);
}

/* the right state for an instance now, from its times; after a reboot or a time change too */
static void
register_instance (AlarmEngine *self, AlarmInstance *instance)
{
  g_autoptr(GDateTime) current = now (self);
  g_autoptr(GDateTime) time = alarm_instance_time (instance);
  g_autoptr(GDateTime) timeout = instance_timeout (self, instance);
  g_autoptr(GDateTime) low = alarm_instance_low_notification_time (instance);
  g_autoptr(GDateTime) high = alarm_instance_high_notification_time (instance);
  g_autoptr(GDateTime) missed_ttl = alarm_instance_missed_ttl (instance);

  switch (instance->state)
    {
    case ALARM_STATE_DISMISSED:
      delete_instance_and_update_parent (self, instance);
      return;
    case ALARM_STATE_FIRED:
      /* it rings on unless it is past its auto-silence */
      if (!timeout || g_date_time_compare (current, timeout) <= 0)
        {
          set_fired_state (self, instance);
          return;
        }
      break;
    case ALARM_STATE_MISSED:
      /* the clock went back before it: its alarm comes back on and it is reckoned again below */
      if (g_date_time_compare (current, time) < 0)
        {
          Alarm *alarm = alarm_store_find_alarm (self->store, instance->alarm_id);

          if (!alarm)
            {
              delete_instance_and_update_parent (self, instance);
              return;
            }
          alarm->enabled = TRUE;
        }
      break;
    case ALARM_STATE_PREDISMISSED:
      if (g_date_time_compare (current, time) < 0)
        set_state (self, instance, ALARM_STATE_PREDISMISSED);
      else
        delete_instance_and_update_parent (self, instance);
      return;
    default:
      break;
    }

  if (g_date_time_compare (current, missed_ttl) > 0)
    {
      /* so old it is just over */
      delete_instance_and_update_parent (self, instance);
    }
  else if (g_date_time_compare (current, time) >= 0)
    {
      /* at or just after its time it rings; well after, it was missed */
      g_autoptr(GDateTime) buffer = g_date_time_add_seconds (time, FIRE_BUFFER_SECONDS);

      if (g_date_time_compare (current, buffer) < 0)
        set_fired_state (self, instance);
      else
        set_missed_state (self, instance);
    }
  else if (instance->state == ALARM_STATE_SNOOZE)
    set_state (self, instance, ALARM_STATE_SNOOZE);
  else if (g_date_time_compare (current, high) > 0)
    set_state (self, instance, ALARM_STATE_HIGH_NOTIFICATION);
  else if (g_date_time_compare (current, low) > 0)
    {
      /* swiped away earlier: it stays away */
      set_state (self, instance, instance->state == ALARM_STATE_HIDE_NOTIFICATION
                 ? ALARM_STATE_HIDE_NOTIFICATION : ALARM_STATE_LOW_NOTIFICATION);
    }
  else
    set_state (self, instance, ALARM_STATE_SILENT);
}

/* --- the schedule */

/* when the instance changes state next, and to what */
static GDateTime *
next_change (AlarmEngine *self, AlarmInstance *instance, AlarmState *to)
{
  switch (instance->state)
    {
    case ALARM_STATE_SILENT:
      *to = ALARM_STATE_LOW_NOTIFICATION;
      return alarm_instance_low_notification_time (instance);
    case ALARM_STATE_LOW_NOTIFICATION:
    case ALARM_STATE_HIDE_NOTIFICATION:
      *to = ALARM_STATE_HIGH_NOTIFICATION;
      return alarm_instance_high_notification_time (instance);
    case ALARM_STATE_HIGH_NOTIFICATION:
    case ALARM_STATE_SNOOZE:
      *to = ALARM_STATE_FIRED;
      return alarm_instance_time (instance);
    case ALARM_STATE_FIRED:
      *to = ALARM_STATE_MISSED;
      return instance_timeout (self, instance);
    case ALARM_STATE_MISSED:
      *to = ALARM_STATE_DISMISSED;
      return alarm_instance_missed_ttl (instance);
    case ALARM_STATE_PREDISMISSED:
      *to = ALARM_STATE_DISMISSED;
      return alarm_instance_time (instance);
    default:
      return NULL;
    }
}

GDateTime *
alarm_engine_next_change (AlarmEngine *self)
{
  GDateTime *earliest = NULL;

  for (guint i = 0; i < self->store->instances->len; i++)
    {
      AlarmInstance *instance = g_ptr_array_index (self->store->instances, i);
      AlarmState to;
      GDateTime *time = next_change (self, instance, &to);

      if (time && (!earliest || g_date_time_compare (time, earliest) < 0))
        {
          g_clear_pointer (&earliest, g_date_time_unref);
          earliest = time;
        }
      else if (time)
        g_date_time_unref (time);
    }
  return earliest;
}

void
alarm_engine_tick (AlarmEngine *self)
{
  gboolean changed = TRUE;

  /* one change can make the next due as well (a late wake: upcoming, then ringing): until none is */
  while (changed)
    {
      g_autoptr(GDateTime) current = now (self);

      changed = FALSE;
      for (guint i = 0; i < self->store->instances->len; i++)
        {
          AlarmInstance *instance = g_ptr_array_index (self->store->instances, i);
          AlarmState to;
          g_autoptr(GDateTime) time = next_change (self, instance, &to);

          if (!time || g_date_time_compare (time, current) > 0)
            continue;

          switch (to)
            {
            case ALARM_STATE_FIRED:
              {
                /* late (the phone woke late) it still rings, unless it would already have rung out */
                g_autoptr(GDateTime) timeout = instance_timeout (self, instance);

                if (timeout && g_date_time_compare (current, timeout) > 0)
                  set_missed_state (self, instance);
                else
                  set_fired_state (self, instance);
                break;
              }
            case ALARM_STATE_MISSED:
              set_missed_state (self, instance);
              break;
            case ALARM_STATE_DISMISSED:
              set_dismiss_state (self, instance);
              break;
            default:
              set_state (self, instance, to);
              break;
            }
          /* the instances may have changed under us: look again from the start */
          changed = TRUE;
          break;
        }
    }
}

/* wall-clock order, latest first */
static int
later_first (gconstpointer a, gconstpointer b)
{
  const AlarmInstance *x = *(AlarmInstance **) a, *y = *(AlarmInstance **) b;
  int fields_x[] = { x->year, x->month, x->day, x->hour, x->minute };
  int fields_y[] = { y->year, y->month, y->day, y->hour, y->minute };

  for (guint i = 0; i < G_N_ELEMENTS (fields_x); i++)
    if (fields_x[i] != fields_y[i])
      return fields_y[i] - fields_x[i];
  return 0;
}

void
alarm_engine_fix (AlarmEngine *self)
{
  g_autoptr(GPtrArray) sorted = g_ptr_array_new ();       /* a copy would free the instances with it */
  g_autoptr(GArray) ids = g_array_new (FALSE, FALSE, sizeof (guint));
  g_autoptr(GDateTime) current = now (self);

  /* latest first, so the later instances are settled before earlier ones may make or move them; by id, as each
   * step may remove instances */
  for (guint i = 0; i < self->store->instances->len; i++)
    g_ptr_array_add (sorted, self->store->instances->pdata[i]);
  g_ptr_array_sort (sorted, later_first);
  for (guint i = 0; i < sorted->len; i++)
    g_array_append_val (ids, ((AlarmInstance *) sorted->pdata[i])->id);
  for (guint i = 0; i < ids->len; i++)
    {
      AlarmInstance *instance = alarm_store_find_instance (self->store, g_array_index (ids, guint, i));
      Alarm *alarm;
      g_autoptr(GDateTime) time = NULL;
      g_autoptr(GDateTime) prior = NULL;
      g_autoptr(GDateTime) missed_ttl = NULL;

      if (!instance)
        continue;
      alarm = alarm_store_find_alarm (self->store, instance->alarm_id);
      if (!alarm)
        {
          remove_instance (self, instance);
          continue;
        }
      /* a time change so large the instance makes no sense (before its alarm's previous time, or past its
       * remembering): it goes, and the alarm makes the right one */
      time = alarm_instance_time (instance);
      prior = previous_alarm_time (alarm, time);
      missed_ttl = alarm_instance_missed_ttl (instance);
      if ((prior && g_date_time_compare (current, prior) < 0) || g_date_time_compare (current, missed_ttl) > 0)
        delete_instance_and_update_parent (self, instance);
      else
        register_instance (self, instance);
    }

  /* an enabled alarm with no instance (the store edited, a crash between two writes) gets one */
  for (guint i = 0; i < self->store->alarms->len; i++)
    {
      Alarm *alarm = g_ptr_array_index (self->store->alarms, i);

      if (alarm->enabled && !alarm_store_instance_of (self->store, alarm->id))
        register_instance (self, add_instance (self, alarm_instance_after (alarm, current)));
    }
  store_changed (self);
}

AlarmInstance *
alarm_engine_next_alarm (AlarmEngine *self)
{
  AlarmInstance *next = NULL;
  g_autoptr(GDateTime) next_time = NULL;

  /* the instances still to ring: up to snoozed */
  for (guint i = 0; i < self->store->instances->len; i++)
    {
      AlarmInstance *instance = g_ptr_array_index (self->store->instances, i);
      g_autoptr(GDateTime) time = NULL;

      if (instance->state >= ALARM_STATE_FIRED)
        continue;
      time = alarm_instance_time (instance);
      if (!next || g_date_time_compare (time, next_time) < 0)
        {
          next = instance;
          g_clear_pointer (&next_time, g_date_time_unref);
          next_time = g_steal_pointer (&time);
        }
    }
  return next;
}

AlarmInstance *
alarm_engine_ringing (AlarmEngine *self)
{
  for (guint i = 0; i < self->store->instances->len; i++)
    {
      AlarmInstance *instance = g_ptr_array_index (self->store->instances, i);

      if (instance->state == ALARM_STATE_FIRED)
        return instance;
    }
  return NULL;
}

/* --- the user's changes */

/* the alarm's instances go, and an enabled alarm gets a new one */
static void
renew_instances (AlarmEngine *self, Alarm *alarm)
{
  g_autoptr(GDateTime) current = now (self);

  for (guint i = self->store->instances->len; i-- > 0;)
    {
      AlarmInstance *instance = g_ptr_array_index (self->store->instances, i);

      if (instance->alarm_id == alarm->id)
        remove_instance (self, instance);
    }
  if (alarm->enabled)
    register_instance (self, add_instance (self, alarm_instance_after (alarm, current)));
  store_changed (self);
}

guint
alarm_engine_add_alarm (AlarmEngine *self, const Alarm *alarm)
{
  Alarm *added = alarm_copy (alarm);

  added->id = self->store->next_id++;
  g_ptr_array_add (self->store->alarms, added);
  renew_instances (self, added);
  return added->id;
}

gboolean
alarm_engine_update_alarm (AlarmEngine *self, const Alarm *alarm)
{
  Alarm *stored = alarm_store_find_alarm (self->store, alarm->id);

  if (!stored)
    return FALSE;
  stored->hour = alarm->hour;
  stored->minute = alarm->minute;
  stored->days = alarm->days;
  stored->enabled = alarm->enabled;
  stored->vibrate = alarm->vibrate;
  stored->delete_after_use = alarm->delete_after_use;
  g_free (stored->label);
  stored->label = g_strdup (alarm->label);
  g_free (stored->ringtone);
  stored->ringtone = g_strdup (alarm->ringtone);
  renew_instances (self, stored);
  return TRUE;
}

gboolean
alarm_engine_delete_alarm (AlarmEngine *self, guint alarm_id)
{
  Alarm *alarm = alarm_store_find_alarm (self->store, alarm_id);

  if (!alarm)
    return FALSE;
  alarm->enabled = FALSE;
  renew_instances (self, alarm);
  g_ptr_array_remove (self->store->alarms, alarm);
  store_changed (self);
  return TRUE;
}

/* --- the answers */

gboolean
alarm_engine_snooze (AlarmEngine *self, guint instance_id)
{
  AlarmInstance *instance = alarm_store_find_instance (self->store, instance_id);
  g_autoptr(GDateTime) current = now (self);
  g_autoptr(GDateTime) time = NULL;

  if (!instance || instance->state != ALARM_STATE_FIRED)
    return FALSE;
  /* it rings again the snooze length from now: the instance's time moves there */
  time = g_date_time_add_minutes (current, self->settings.snooze_minutes);
  alarm_instance_set_time (instance, time);
  set_state (self, instance, ALARM_STATE_SNOOZE);
  return TRUE;
}

gboolean
alarm_engine_dismiss (AlarmEngine *self, guint instance_id)
{
  AlarmInstance *instance = alarm_store_find_instance (self->store, instance_id);

  if (!instance)
    return FALSE;
  switch (instance->state)
    {
    case ALARM_STATE_SILENT:
    case ALARM_STATE_LOW_NOTIFICATION:
    case ALARM_STATE_HIDE_NOTIFICATION:
    case ALARM_STATE_HIGH_NOTIFICATION:
      set_predismiss_state (self, instance);
      return TRUE;
    case ALARM_STATE_PREDISMISSED:
    case ALARM_STATE_DISMISSED:
      return FALSE;
    default:
      set_dismiss_state (self, instance);
      return TRUE;
    }
}

gboolean
alarm_engine_hide_notification (AlarmEngine *self, guint instance_id)
{
  AlarmInstance *instance = alarm_store_find_instance (self->store, instance_id);

  if (!instance || instance->state != ALARM_STATE_LOW_NOTIFICATION)
    return FALSE;
  set_state (self, instance, ALARM_STATE_HIDE_NOTIFICATION);
  return TRUE;
}

/* --- the object */

AlarmEngine *
alarm_engine_new (AlarmStore *store)
{
  AlarmEngine *self = g_object_new (ALARM_TYPE_ENGINE, NULL);

  self->store = store;
  return self;
}

AlarmStore *
alarm_engine_get_store (AlarmEngine *self)
{
  return self->store;
}

void
alarm_engine_set_settings (AlarmEngine *self, const AlarmEngineSettings *settings)
{
  self->settings = *settings;
}

void
alarm_engine_set_clock (AlarmEngine *self, GDateTime *(*clock) (gpointer data), gpointer data)
{
  self->now = clock ? clock : real_now;
  self->now_data = data;
}

static void
alarm_engine_finalize (GObject *object)
{
  AlarmEngine *self = ALARM_ENGINE (object);

  alarm_store_free (self->store);
  G_OBJECT_CLASS (alarm_engine_parent_class)->finalize (object);
}

static void
alarm_engine_class_init (AlarmEngineClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = alarm_engine_finalize;

  signals[STORE_CHANGED] = g_signal_new ("store-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                         NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[INSTANCE_CHANGED] = g_signal_new ("instance-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                            NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_INT);
  signals[INSTANCE_REMOVED] = g_signal_new ("instance-removed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                            NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_UINT);
}

static void
alarm_engine_init (AlarmEngine *self)
{
  self->now = real_now;
  self->settings.snooze_minutes = 10;
  self->settings.auto_silence_minutes = 10;
}
