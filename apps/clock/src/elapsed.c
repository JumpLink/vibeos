/* The timers' clocks (elapsed.h). */
#include "elapsed.h"

#include <time.h>

gint64
elapsed_now (void)
{
  struct timespec now;

  clock_gettime (CLOCK_BOOTTIME, &now);
  return (gint64) now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

gint64
elapsed_wall_now (void)
{
  return g_get_real_time () / 1000;
}

char *
elapsed_boot_id (void)
{
  char *id = NULL;

  if (!g_file_get_contents ("/proc/sys/kernel/random/boot_id", &id, NULL, NULL))
    return g_strdup ("");
  return g_strstrip (id);
}
