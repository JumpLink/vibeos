/* The clock's stopwatch (stopwatch-model.h). */
#include "stopwatch-model.h"

Stopwatch *
stopwatch_new (void)
{
  Stopwatch *stopwatch = g_new0 (Stopwatch, 1);

  stopwatch->laps = g_array_new (FALSE, FALSE, sizeof (gint64));
  stopwatch->boot_id = g_strdup ("");
  return stopwatch;
}

Stopwatch *
stopwatch_copy (const Stopwatch *stopwatch)
{
  Stopwatch *copy = g_memdup2 (stopwatch, sizeof *stopwatch);

  copy->laps = g_array_copy (stopwatch->laps);
  copy->boot_id = g_strdup (stopwatch->boot_id);
  return copy;
}

void
stopwatch_free (Stopwatch *stopwatch)
{
  if (!stopwatch)
    return;
  g_array_unref (stopwatch->laps);
  g_free (stopwatch->boot_id);
  g_free (stopwatch);
}

gint64
stopwatch_total (const Stopwatch *stopwatch, gint64 now)
{
  if (stopwatch->state != STOPWATCH_RUNNING)
    return stopwatch->accumulated;
  /* a stored start from before a reboot must not count backwards */
  return stopwatch->accumulated + MAX (0, now - stopwatch->last_start);
}

void
stopwatch_start (Stopwatch *stopwatch, gint64 now, gint64 wall)
{
  if (stopwatch->state == STOPWATCH_RUNNING)
    return;
  stopwatch->accumulated = stopwatch_total (stopwatch, now);
  stopwatch->state = STOPWATCH_RUNNING;
  stopwatch->last_start = now;
  stopwatch->last_start_wall = wall;
}

void
stopwatch_pause (Stopwatch *stopwatch, gint64 now)
{
  if (stopwatch->state != STOPWATCH_RUNNING)
    return;
  stopwatch->accumulated = stopwatch_total (stopwatch, now);
  stopwatch->state = STOPWATCH_PAUSED;
  stopwatch->last_start = stopwatch->last_start_wall = 0;
}

void
stopwatch_reset (Stopwatch *stopwatch)
{
  stopwatch->state = STOPWATCH_RESET;
  stopwatch->last_start = stopwatch->last_start_wall = stopwatch->accumulated = 0;
  g_array_set_size (stopwatch->laps, 0);
}

gboolean
stopwatch_can_lap (const Stopwatch *stopwatch)
{
  return stopwatch->laps->len < STOPWATCH_MAX_LAPS;
}

gboolean
stopwatch_lap (Stopwatch *stopwatch, gint64 now)
{
  gint64 total = stopwatch_total (stopwatch, now);

  if (stopwatch->state != STOPWATCH_RUNNING || !stopwatch_can_lap (stopwatch))
    return FALSE;
  g_array_append_val (stopwatch->laps, total);
  return TRUE;
}

void
stopwatch_update_after_reboot (Stopwatch *stopwatch, gint64 now, gint64 wall)
{
  if (stopwatch->state != STOPWATCH_RUNNING)
    return;
  /* the time the phone was off, by the wall clock; a clock set back in the meantime counts as none */
  stopwatch->accumulated += MAX (0, wall - stopwatch->last_start_wall);
  stopwatch->last_start = now;
  stopwatch->last_start_wall = wall;
}

void
stopwatch_update_after_time_set (Stopwatch *stopwatch, gint64 now, gint64 wall)
{
  if (stopwatch->state != STOPWATCH_RUNNING || now < stopwatch->last_start)
    return;
  stopwatch->accumulated += now - stopwatch->last_start;
  stopwatch->last_start = now;
  stopwatch->last_start_wall = wall;
}

gint64
stopwatch_lap_time (const Stopwatch *stopwatch, guint number)
{
  gint64 end = g_array_index (stopwatch->laps, gint64, number - 1);

  return number == 1 ? end : end - g_array_index (stopwatch->laps, gint64, number - 2);
}

gint64
stopwatch_current_lap_time (const Stopwatch *stopwatch, gint64 total)
{
  guint n = stopwatch->laps->len;

  return n ? MAX (0, total - g_array_index (stopwatch->laps, gint64, n - 1)) : total;
}

gint64
stopwatch_longest_lap_time (const Stopwatch *stopwatch, gint64 total)
{
  gint64 longest = 0;

  if (!stopwatch->laps->len)
    return 0;
  for (guint i = 1; i <= stopwatch->laps->len; i++)
    longest = MAX (longest, stopwatch_lap_time (stopwatch, i));
  return MAX (longest, stopwatch_current_lap_time (stopwatch, total));
}

/* --- the store */

Stopwatch *
stopwatch_load (const char *path, GError **error)
{
  g_autoptr(Stopwatch) stopwatch = stopwatch_new ();
  g_autoptr(GKeyFile) file = g_key_file_new ();
  g_autoptr(GError) local_error = NULL;
  g_autofree char *boot_id = NULL;
  g_auto(GStrv) laps = NULL;
  gsize n_laps = 0;

  if (!g_key_file_load_from_file (file, path, G_KEY_FILE_KEEP_COMMENTS, &local_error))
    {
      if (g_error_matches (local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        return g_steal_pointer (&stopwatch);
      g_propagate_error (error, g_steal_pointer (&local_error));
      return NULL;
    }

  stopwatch->state = g_key_file_get_integer (file, "stopwatch", "state", NULL);
  if (stopwatch->state > STOPWATCH_PAUSED)
    stopwatch->state = STOPWATCH_RESET;
  stopwatch->last_start = g_key_file_get_int64 (file, "stopwatch", "last-start", NULL);
  stopwatch->last_start_wall = g_key_file_get_int64 (file, "stopwatch", "last-start-wall", NULL);
  stopwatch->accumulated = g_key_file_get_int64 (file, "stopwatch", "accumulated", NULL);
  if ((boot_id = g_key_file_get_string (file, "stopwatch", "boot", NULL)))
    {
      g_free (stopwatch->boot_id);
      stopwatch->boot_id = g_steal_pointer (&boot_id);
    }
  /* a list of numbers, as strings: int64 has no key file list of its own */
  laps = g_key_file_get_string_list (file, "stopwatch", "laps", &n_laps, NULL);
  for (gsize i = 0; i < n_laps && i < STOPWATCH_MAX_LAPS; i++)
    {
      gint64 end = g_ascii_strtoll (laps[i], NULL, 10);

      g_array_append_val (stopwatch->laps, end);
    }
  return g_steal_pointer (&stopwatch);
}

gboolean
stopwatch_save (const Stopwatch *stopwatch, const char *path, GError **error)
{
  g_autoptr(GKeyFile) file = g_key_file_new ();
  g_autofree char *dir = g_path_get_dirname (path);
  g_autoptr(GPtrArray) laps = g_ptr_array_new_with_free_func (g_free);

  g_key_file_set_comment (file, NULL, NULL,
                          " The clock's stopwatch. Times in ms: last-start on the clock since boot of the boot named\n"
                          " here, last-start-wall on the wall clock (Unix); laps: the total at the end of each.\n"
                          " vibeos-clock-alarms keeps this file: edit it only while that is stopped.", NULL);
  g_key_file_set_string (file, "stopwatch", "boot", stopwatch->boot_id);
  g_key_file_set_integer (file, "stopwatch", "state", stopwatch->state);
  g_key_file_set_int64 (file, "stopwatch", "last-start", stopwatch->last_start);
  g_key_file_set_int64 (file, "stopwatch", "last-start-wall", stopwatch->last_start_wall);
  g_key_file_set_int64 (file, "stopwatch", "accumulated", stopwatch->accumulated);
  for (guint i = 0; i < stopwatch->laps->len; i++)
    g_ptr_array_add (laps, g_strdup_printf ("%" G_GINT64_FORMAT, g_array_index (stopwatch->laps, gint64, i)));
  g_key_file_set_string_list (file, "stopwatch", "laps", (const char * const *) laps->pdata, laps->len);
  g_mkdir_with_parents (dir, 0700);
  return g_key_file_save_to_file (file, path, error);
}
