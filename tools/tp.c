/* tp: scripted touches and keys, written into the touchscreen's and the keys' evdev nodes as if a finger did them
 * (the compositor's libinput reads them like any other). The touchscreen is the first device with
 * ABS_MT_POSITION_X, the keys the one named gpio-keys / gpio_keys; TP_TOUCH and TP_KEYS name others.
 * Commands on stdin, one per line (pixels on a 1080x2280 panel, times in ms):
 *   down X Y | move X Y | up | wait MS
 *   swipe X1 Y1 X2 Y2 MS [linear|out|in|inout]   one finger from down to up
 *   drag  X1 Y1 X2 Y2 MS [curve]                   move without lifting (needs a down first, ends where it got)
 *   key CODE [MS]                                  press and release a key on gpio_keys
 *   mark TEXT                                      print TEXT with the time since start (to stderr)        */
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int ts, keys;
static int max_x = 4095, max_y = 4095;
static double cx, cy;
static int tid = 100;
static struct timespec t0;

static double now_ms (void)
{
  struct timespec t; clock_gettime (CLOCK_MONOTONIC, &t);
  return (t.tv_sec - t0.tv_sec) * 1e3 + (t.tv_nsec - t0.tv_nsec) / 1e6;
}
static void ev (int fd, int type, int code, int value)
{
  struct input_event e = { 0 };
  e.type = type; e.code = code; e.value = value;
  if (write (fd, &e, sizeof e) != sizeof e) perror ("write");
}
static void pos (double x, double y)
{
  cx = x; cy = y;
  ev (ts, EV_ABS, ABS_MT_POSITION_X, (int) lround (x * max_x / 1079.0));
  ev (ts, EV_ABS, ABS_MT_POSITION_Y, (int) lround (y * max_y / 2279.0));
  ev (ts, EV_SYN, SYN_REPORT, 0);
}
static void down (double x, double y)
{
  ev (ts, EV_ABS, ABS_MT_SLOT, 0);
  ev (ts, EV_ABS, ABS_MT_TRACKING_ID, tid++);
  ev (ts, EV_KEY, BTN_TOUCH, 1);
  ev (ts, EV_KEY, BTN_TOOL_FINGER, 1);
  ev (ts, EV_ABS, ABS_MT_TOUCH_MAJOR, 12);
  ev (ts, EV_ABS, ABS_MT_PRESSURE, 30);
  pos (x, y);
}
static void up (void)
{
  ev (ts, EV_ABS, ABS_MT_SLOT, 0);
  ev (ts, EV_ABS, ABS_MT_TRACKING_ID, -1);
  ev (ts, EV_KEY, BTN_TOUCH, 0);
  ev (ts, EV_KEY, BTN_TOOL_FINGER, 0);
  ev (ts, EV_SYN, SYN_REPORT, 0);
}
static void sleep_ms (double ms)
{
  if (ms <= 0) return;
  struct timespec t = { (time_t) (ms / 1000), (long) (fmod (ms, 1000) * 1e6) };
  nanosleep (&t, NULL);
}
static double curve (const char *c, double t)
{
  if (!strcmp (c, "out")) return 1 - (1 - t) * (1 - t);
  if (!strcmp (c, "in")) return t * t;
  if (!strcmp (c, "inout")) return t < .5 ? 2 * t * t : 1 - 2 * (1 - t) * (1 - t);
  return t;
}
/* every 8 ms, on the clock, so a slow drag is as even as a finger */
static void path (double x1, double y1, double x2, double y2, double ms, const char *c)
{
  double start = now_ms ();
  for (;;) {
    double t = ms > 0 ? (now_ms () - start) / ms : 1;
    if (t >= 1) break;
    double k = curve (c, t);
    pos (x1 + (x2 - x1) * k, y1 + (y2 - y1) * k);
    sleep_ms (8 - fmod (now_ms () - start, 8));
  }
  pos (x2, y2);
}
/* the first /dev/input/event* that has ABS_MT_POSITION_X (touch) or is named like the keys */
static int find (int touch)
{
  DIR *d = opendir ("/dev/input");
  struct dirent *e;
  int found = -1;
  while (d && found < 0 && (e = readdir (d))) {
    if (strncmp (e->d_name, "event", 5)) continue;
    char p[64], name[128] = "";
    snprintf (p, sizeof p, "/dev/input/%s", e->d_name);
    int fd = open (p, O_RDWR);
    if (fd < 0) continue;
    ioctl (fd, EVIOCGNAME (sizeof name), name);
    struct input_absinfo ai;
    if (touch ? ioctl (fd, EVIOCGABS (ABS_MT_POSITION_X), &ai) == 0 && ai.maximum > 0
              : strstr (name, "gpio-keys") || strstr (name, "gpio_keys"))
      found = fd;
    else
      close (fd);
  }
  if (d) closedir (d);
  return found;
}
int main (void)
{
  const char *t = getenv ("TP_TOUCH"), *k = getenv ("TP_KEYS");
  ts = t ? open (t, O_RDWR) : find (1);
  keys = k ? open (k, O_RDWR) : find (0);
  if (ts < 0 || keys < 0) { perror ("open"); return 1; }
  struct input_absinfo ai;
  if (ioctl (ts, EVIOCGABS (ABS_MT_POSITION_X), &ai) == 0) max_x = ai.maximum;
  if (ioctl (ts, EVIOCGABS (ABS_MT_POSITION_Y), &ai) == 0) max_y = ai.maximum;
  clock_gettime (CLOCK_MONOTONIC, &t0);
  char line[256], c[16];
  while (fgets (line, sizeof line, stdin)) {
    double a[5] = { 0 }; char cv[16] = "linear";
    if (sscanf (line, "%15s", c) != 1 || c[0] == '#') continue;
    if (!strcmp (c, "down") && sscanf (line, "%*s %lf %lf", a, a + 1) == 2) down (a[0], a[1]);
    else if (!strcmp (c, "move") && sscanf (line, "%*s %lf %lf", a, a + 1) == 2) pos (a[0], a[1]);
    else if (!strcmp (c, "up")) up ();
    else if (!strcmp (c, "wait") && sscanf (line, "%*s %lf", a) == 1) sleep_ms (a[0]);
    else if (!strcmp (c, "swipe") && sscanf (line, "%*s %lf %lf %lf %lf %lf %15s", a, a + 1, a + 2, a + 3, a + 4, cv) >= 5) {
      down (a[0], a[1]); path (a[0], a[1], a[2], a[3], a[4], cv); up ();
    } else if (!strcmp (c, "drag") && sscanf (line, "%*s %lf %lf %lf %lf %lf %15s", a, a + 1, a + 2, a + 3, a + 4, cv) >= 5)
      path (a[0], a[1], a[2], a[3], a[4], cv);
    else if (!strcmp (c, "key") && sscanf (line, "%*s %lf %lf", a, a + 1) >= 1) {
      ev (keys, EV_KEY, (int) a[0], 1); ev (keys, EV_SYN, SYN_REPORT, 0);
      sleep_ms (a[1] > 0 ? a[1] : 80);
      ev (keys, EV_KEY, (int) a[0], 0); ev (keys, EV_SYN, SYN_REPORT, 0);
    } else if (!strcmp (c, "mark")) fprintf (stderr, "%9.1f %s", now_ms (), line + 5);
    else fprintf (stderr, "tp: ? %s", line);
  }
  return 0;
}
