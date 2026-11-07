/* The clock's timers (timer-model.h). */
#include "timer-model.h"

#include <stdio.h>

#define MINUTE 60000

Timer *
timer_new (guint id, gint64 length, const char *label, gboolean delete_after_use)
{
  Timer *timer = g_new0 (Timer, 1);

  timer->id = id;
  timer->state = TIMER_RESET;
  timer->length = timer->total_length = timer->remaining = length;
  timer->label = g_strdup (label ? label : "");
  timer->delete_after_use = delete_after_use;
  return timer;
}

Timer *
timer_copy (const Timer *timer)
{
  Timer *copy = g_memdup2 (timer, sizeof *timer);

  copy->label = g_strdup (timer->label);
  return copy;
}

void
timer_free (Timer *timer)
{
  if (!timer)
    return;
  g_free (timer->label);
  g_free (timer);
}

gint64
timer_remaining (const Timer *timer, gint64 now)
{
  if (timer->state == TIMER_PAUSED || timer->state == TIMER_RESET)
    return timer->remaining;
  /* the elapsed clock never goes back, but a stored start can be from before a reboot: never count it backwards */
  return timer->remaining - MAX (0, now - timer->last_start);
}

gint64
timer_elapsed (const Timer *timer, gint64 now)
{
  return timer->total_length - timer_remaining (timer, now);
}

gint64
timer_expiration (const Timer *timer)
{
  return timer->last_start + timer->remaining;
}

gint64
timer_total_length (const Timer *timer)
{
  return timer->total_length;
}

void
timer_start (Timer *timer, gint64 now, gint64 wall)
{
  if (timer->state == TIMER_RUNNING || timer->state == TIMER_EXPIRED || timer->state == TIMER_MISSED)
    return;
  timer->state = TIMER_RUNNING;
  timer->last_start = now;
  timer->last_start_wall = wall;
}

void
timer_pause (Timer *timer, gint64 now)
{
  if (timer->state == TIMER_PAUSED || timer->state == TIMER_RESET)
    return;
  if (timer->state == TIMER_EXPIRED || timer->state == TIMER_MISSED)
    {
      timer_reset (timer);
      return;
    }
  timer->remaining = timer_remaining (timer, now);
  timer->state = TIMER_PAUSED;
  timer->last_start = timer->last_start_wall = 0;
}

/* expired and missed: the overtime counts from now, and the total is no longer known (it shows no ring) */
static void
run_out (Timer *timer, TimerState state, gint64 now, gint64 wall)
{
  timer->remaining = MIN (0, timer_remaining (timer, now));
  timer->state = state;
  timer->total_length = 0;
  timer->last_start = now;
  timer->last_start_wall = wall;
}

void
timer_expire (Timer *timer, gint64 now, gint64 wall)
{
  if (timer->state == TIMER_EXPIRED || timer->state == TIMER_RESET || timer->state == TIMER_MISSED)
    return;
  run_out (timer, TIMER_EXPIRED, now, wall);
}

void
timer_miss (Timer *timer, gint64 now, gint64 wall)
{
  if (timer->state == TIMER_RESET || timer->state == TIMER_MISSED)
    return;
  run_out (timer, TIMER_MISSED, now, wall);
}

void
timer_reset (Timer *timer)
{
  if (timer->state == TIMER_RESET)
    return;
  timer->state = TIMER_RESET;
  timer->total_length = timer->remaining = timer->length;
  timer->last_start = timer->last_start_wall = 0;
}

/* setRemainingTime: the total moves with it, and a timer that ran out runs again once it has time left */
static void
set_remaining (Timer *timer, gint64 remaining, gint64 now, gint64 wall)
{
  if (timer->remaining == remaining || timer->state == TIMER_RESET)
    return;
  timer->total_length += remaining - timer->remaining;
  if (remaining > 0 && (timer->state == TIMER_EXPIRED || timer->state == TIMER_MISSED))
    {
      timer->state = TIMER_RUNNING;
      timer->last_start = now;
      timer->last_start_wall = wall;
    }
  timer->remaining = remaining;
}

void
timer_add_minute (Timer *timer, gint64 now, gint64 wall)
{
  if (timer->state == TIMER_EXPIRED || timer->state == TIMER_MISSED)
    set_remaining (timer, MINUTE, now, wall);
  else
    set_remaining (timer, timer->remaining + MINUTE, now, wall);
}

void
timer_set_label (Timer *timer, const char *label)
{
  g_free (timer->label);
  timer->label = g_strdup (label ? label : "");
}

void
timer_update_after_reboot (Timer *timer, gint64 now, gint64 wall)
{
  if (timer->state == TIMER_RESET || timer->state == TIMER_PAUSED)
    return;
  /* the time the phone was off, by the wall clock; a clock set back in the meantime counts as none */
  timer->remaining -= MAX (0, wall - timer->last_start_wall);
  timer->last_start = now;
  timer->last_start_wall = wall;
}

void
timer_update_after_time_set (Timer *timer, gint64 now, gint64 wall)
{
  if (timer->state == TIMER_RESET || timer->state == TIMER_PAUSED || now < timer->last_start)
    return;
  timer->remaining -= now - timer->last_start;
  timer->last_start = now;
  timer->last_start_wall = wall;
}

static int
expiry_rank (TimerState state)
{
  switch (state)
    {
    case TIMER_MISSED: return 0;
    case TIMER_EXPIRED: return 1;
    case TIMER_RUNNING: return 2;
    case TIMER_PAUSED: return 3;
    default: return 4;
    }
}

int
timer_compare_expiry (const Timer *a, const Timer *b, gint64 now)
{
  gint64 x, y;

  if (expiry_rank (a->state) != expiry_rank (b->state))
    return expiry_rank (a->state) - expiry_rank (b->state);
  x = a->state == TIMER_RESET ? a->length : timer_remaining (a, now);
  y = b->state == TIMER_RESET ? b->length : timer_remaining (b, now);
  return x < y ? -1 : x > y ? 1 : 0;
}

/* --- the store */

static int
newest_first (gconstpointer a, gconstpointer b)
{
  const Timer *x = *(Timer **) a, *y = *(Timer **) b;

  return x->id < y->id ? 1 : x->id > y->id ? -1 : 0;
}

TimerStore *
timer_store_load (const char *path, GError **error)
{
  g_autoptr(TimerStore) store = g_new0 (TimerStore, 1);
  g_autoptr(GKeyFile) file = g_key_file_new ();
  g_autoptr(GError) local_error = NULL;
  g_auto(GStrv) groups = NULL;

  store->timers = g_ptr_array_new_with_free_func ((GDestroyNotify) timer_free);
  store->next_id = 1;
  if (!g_key_file_load_from_file (file, path, G_KEY_FILE_KEEP_COMMENTS, &local_error))
    {
      if (g_error_matches (local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        {
          store->boot_id = g_strdup ("");
          return g_steal_pointer (&store);
        }
      g_propagate_error (error, g_steal_pointer (&local_error));
      return NULL;
    }

  store->boot_id = g_key_file_get_string (file, "timers", "boot", NULL);
  if (!store->boot_id)
    store->boot_id = g_strdup ("");
  groups = g_key_file_get_groups (file, NULL);
  for (char **group = groups; *group; group++)
    {
      Timer *timer;
      guint id;

      if (sscanf (*group, "timer %u", &id) != 1)
        continue;
      timer = timer_new (id, g_key_file_get_int64 (file, *group, "length", NULL), NULL, FALSE);
      timer->state = g_key_file_get_integer (file, *group, "state", NULL);
      if (timer->state < TIMER_RUNNING || timer->state > TIMER_MISSED)
        timer->state = TIMER_RESET;
      timer->total_length = g_key_file_get_int64 (file, *group, "total-length", NULL);
      timer->last_start = g_key_file_get_int64 (file, *group, "last-start", NULL);
      timer->last_start_wall = g_key_file_get_int64 (file, *group, "last-start-wall", NULL);
      timer->remaining = g_key_file_get_int64 (file, *group, "remaining", NULL);
      g_free (timer->label);
      timer->label = g_key_file_get_string (file, *group, "label", NULL);
      if (!timer->label)
        timer->label = g_strdup ("");
      timer->delete_after_use = g_key_file_get_boolean (file, *group, "delete-after-use", NULL);
      g_ptr_array_add (store->timers, timer);
      store->next_id = MAX (store->next_id, id + 1);
    }
  g_ptr_array_sort (store->timers, newest_first);
  return g_steal_pointer (&store);
}

gboolean
timer_store_save (TimerStore *store, const char *path, GError **error)
{
  g_autoptr(GKeyFile) file = g_key_file_new ();
  g_autofree char *dir = g_path_get_dirname (path);

  g_key_file_set_comment (file, NULL, NULL,
                          " The clock's timers. Times in ms: last-start on the clock since boot of the boot named\n"
                          " here, last-start-wall on the wall clock (Unix).\n"
                          " neo-clock-alarms keeps this file: edit it only while that is stopped.", NULL);
  g_key_file_set_string (file, "timers", "boot", store->boot_id);
  for (guint i = 0; i < store->timers->len; i++)
    {
      Timer *timer = g_ptr_array_index (store->timers, i);
      g_autofree char *group = g_strdup_printf ("timer %u", timer->id);

      g_key_file_set_integer (file, group, "state", timer->state);
      g_key_file_set_int64 (file, group, "length", timer->length);
      g_key_file_set_int64 (file, group, "total-length", timer->total_length);
      g_key_file_set_int64 (file, group, "last-start", timer->last_start);
      g_key_file_set_int64 (file, group, "last-start-wall", timer->last_start_wall);
      g_key_file_set_int64 (file, group, "remaining", timer->remaining);
      g_key_file_set_string (file, group, "label", timer->label);
      g_key_file_set_boolean (file, group, "delete-after-use", timer->delete_after_use);
    }
  g_mkdir_with_parents (dir, 0700);
  return g_key_file_save_to_file (file, path, error);
}

void
timer_store_free (TimerStore *store)
{
  if (!store)
    return;
  g_ptr_array_unref (store->timers);
  g_free (store->boot_id);
  g_free (store);
}

Timer *
timer_store_find (TimerStore *store, guint id)
{
  for (guint i = 0; i < store->timers->len; i++)
    if (((Timer *) store->timers->pdata[i])->id == id)
      return store->timers->pdata[i];
  return NULL;
}

Timer *
timer_store_add (TimerStore *store, Timer *timer)
{
  timer->id = store->next_id++;
  g_ptr_array_insert (store->timers, 0, timer);
  return timer;
}

gboolean
timer_store_remove (TimerStore *store, guint id)
{
  Timer *timer = timer_store_find (store, id);

  return timer && g_ptr_array_remove (store->timers, timer);
}
