#include "ns-motor.h"

#include <gio/gio.h>
#include <string.h>

/* hexadecimal words: anything but zeros and spaces is a bit set */
static gboolean
any_bit (const char *words)
{
  for (const char *c = words; *c; c++)
    if (g_ascii_isxdigit (*c) && *c != '0')
      return TRUE;
  return FALSE;
}

static gboolean
read_bits (const char *dir, const char *node, const char *name)
{
  g_autofree char *path = g_build_filename (dir, node, "device", "capabilities", name, NULL);
  g_autofree char *text = NULL;
  return g_file_get_contents (path, &text, NULL, NULL) && any_bit (text);
}

static gboolean
input_has_force_feedback (const char *dir)
{
  g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
  for (const char *n; d && (n = g_dir_read_name (d));)
    if (g_str_has_prefix (n, "event") && read_bits (dir, n, "ff") && !read_bits (dir, n, "abs"))
      return TRUE;
  return FALSE;
}

static gboolean
has_vibrator_led (const char *dir)
{
  g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
  for (const char *n; d && (n = g_dir_read_name (d));) {
    g_autofree char *lower = g_ascii_strdown (n, -1);
    if (strstr (lower, "vibrator"))
      return TRUE;
  }
  return FALSE;
}

/* udev's database has a file per device node (c13:64 is an input event node) and "E:" lines for its properties */
static gboolean
feedbackd_has_haptic (const char *dir)
{
  g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
  for (const char *n; d && (n = g_dir_read_name (d));) {
    if (!g_str_has_prefix (n, "c13:"))
      continue;
    g_autofree char *path = g_build_filename (dir, n, NULL);
    g_autofree char *text = NULL;
    if (g_file_get_contents (path, &text, NULL, NULL) &&
        (g_str_has_prefix (text, "E:FEEDBACKD_TYPE=vibra\n") || strstr (text, "\nE:FEEDBACKD_TYPE=vibra\n")))
      return TRUE;
  }
  return FALSE;
}

gboolean
ns_motor_present_in (const char *input_dir, const char *leds_dir, const char *udev_dir)
{
  return input_has_force_feedback (input_dir ? input_dir : "/sys/class/input") ||
         has_vibrator_led (leds_dir ? leds_dir : "/sys/class/leds") ||
         feedbackd_has_haptic (udev_dir ? udev_dir : "/run/udev/data");
}

gboolean
ns_motor_present (void)
{
  static int known = -1;
  if (known < 0)
    known = ns_motor_present_in (NULL, NULL, NULL);
  return known;
}
