/* input-boost: what Android's input boost does, for the phone's GPU (and its CPUs, when asked). While a finger is on
 * the touchscreen, and for a moment after it lifts, the GPU's clock has a floor; the rest of the time its governor is
 * free to go down. Panfrost's simple_ondemand rests the S10e's Mali-G76 at 156 MHz and looks at its load every 50 ms,
 * so the first frames of every gesture rendered at a quarter of the clock they needed: the launcher's first blur frame
 * took 29.8 ms of GPU at 156 MHz and 5.7 ms at 572 MHz, and with a 572 MHz floor every frame of the back and home
 * flights made 60 fps (2026-11-04). The hold covers what runs on after the finger: a fling's scroll, a flight home,
 * the drawer settling. Keys (power, volume) boost too, for what they animate.
 *
 * The input devices are read, never grabbed: the compositor gets every event as before. Sysfs is written only when the
 * boost changes. Environment:
 *   INPUT_BOOST_GPU      the GPU's devfreq directory (default: the first /sys/class/devfreq/<*.gpu>)
 *   INPUT_BOOST_GPU_HZ   its floor while boosted (default 572000000)
 *   INPUT_BOOST_CPU      CPU floors while boosted, as "policy0=1300000 policy4=1066000" (kHz; default none)
 *   INPUT_BOOST_HOLD_MS  how long the boost outlasts the last event (default 1200)  */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define MAX_DEVICES 16
#define MAX_FLOORS 8
#define STUCK_MS 10000          /* a contact with no event for this long is a lost release, not a finger */
#define LONG_BITS (8 * sizeof (unsigned long))
#define NBITS(n) ((n) / LONG_BITS + 1)
#define TEST_BIT(bits, n) (((bits)[(n) / LONG_BITS] >> ((n) % LONG_BITS)) & 1)

typedef struct {
  char path[256];               /* a min_freq file */
  char boost[24];               /* what it holds while boosted */
  char rest[24];                /* and otherwise */
} Floor;

typedef struct {
  int fd;
  char node[300];               /* /dev/input/eventN */
  bool touch;                   /* a touchscreen: boosted for as long as it reports contact */
  bool down;
} Device;

static Floor floors[MAX_FLOORS];
static int n_floors;
static Device devices[MAX_DEVICES];
static int n_devices;
static volatile sig_atomic_t quit;

static int64_t
now_ms (void)
{
  struct timespec t;
  clock_gettime (CLOCK_MONOTONIC, &t);
  return (int64_t) t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static bool
read_str (const char *path, char *buf, size_t size)
{
  FILE *f = fopen (path, "r");
  if (!f)
    return false;
  bool ok = fgets (buf, size, f) != NULL;
  fclose (f);
  if (ok)
    buf[strcspn (buf, "\n")] = 0;
  return ok;
}

static void
write_str (const char *path, const char *value)
{
  int fd = open (path, O_WRONLY | O_CLOEXEC);
  if (fd < 0 || write (fd, value, strlen (value)) < 0)
    fprintf (stderr, "input-boost: %s: %s\n", path, strerror (errno));
  if (fd >= 0)
    close (fd);
}

/* A floor rests at what it was when we started, unless that is the boost itself (a boost that died boosted): then at
 * the lowest the hardware allows */
static void
add_floor (const char *path, const char *boost, const char *lowest)
{
  if (n_floors == MAX_FLOORS)
    return;
  Floor *f = &floors[n_floors];
  char now[24];
  if (!read_str (path, now, sizeof now)) {
    fprintf (stderr, "input-boost: %s: %s\n", path, strerror (errno));
    return;
  }
  snprintf (f->path, sizeof f->path, "%s", path);
  snprintf (f->boost, sizeof f->boost, "%s", boost);
  snprintf (f->rest, sizeof f->rest, "%s", strcmp (now, boost) ? now : lowest);
  n_floors++;
  printf ("input-boost: %s %s while boosted, %s at rest\n", f->path, f->boost, f->rest);
}

static void
add_gpu (void)
{
  const char *hz = getenv ("INPUT_BOOST_GPU_HZ") ?: "572000000";
  char dir[200] = "";
  const char *env = getenv ("INPUT_BOOST_GPU");
  if (env) {
    snprintf (dir, sizeof dir, "%s", env);
  } else {
    DIR *d = opendir ("/sys/class/devfreq");
    struct dirent *e;
    while (d && (e = readdir (d))) {
      size_t n = strlen (e->d_name);
      if (n > 4 && !strcmp (e->d_name + n - 4, ".gpu")) {
        snprintf (dir, sizeof dir, "/sys/class/devfreq/%s", e->d_name);
        break;
      }
    }
    if (d)
      closedir (d);
  }
  if (!*dir || !strcmp (hz, "0")) {
    fprintf (stderr, "input-boost: no GPU floor\n");
    return;
  }
  /* the lowest of its frequencies, listed lowest first */
  char path[256], freqs[512], lowest[24] = "0";
  snprintf (path, sizeof path, "%s/available_frequencies", dir);
  if (read_str (path, freqs, sizeof freqs))
    sscanf (freqs, "%23s", lowest);
  snprintf (path, sizeof path, "%s/min_freq", dir);
  add_floor (path, hz, lowest);
}

static void
add_cpus (void)
{
  const char *env = getenv ("INPUT_BOOST_CPU");
  char policy[32], khz[24], path[256], lowest[24];
  int n;
  while (env && sscanf (env, " %31[^= ]=%23s%n", policy, khz, &n) == 2) {
    snprintf (path, sizeof path, "/sys/devices/system/cpu/cpufreq/%s/cpuinfo_min_freq", policy);
    if (!read_str (path, lowest, sizeof lowest))
      snprintf (lowest, sizeof lowest, "0");
    snprintf (path, sizeof path, "/sys/devices/system/cpu/cpufreq/%s/scaling_min_freq", policy);
    add_floor (path, khz, lowest);
    env += n;
  }
}

static void
apply (bool boost)
{
  for (int i = 0; i < n_floors; i++)
    write_str (floors[i].path, boost ? floors[i].boost : floors[i].rest);
}

/* A touchscreen (multitouch positions), or the keys (power, volume) */
static bool
wanted (int fd, bool *touch)
{
  unsigned long abs[NBITS (ABS_MAX)] = { 0 }, key[NBITS (KEY_MAX)] = { 0 };
  ioctl (fd, EVIOCGBIT (EV_ABS, sizeof abs), abs);
  ioctl (fd, EVIOCGBIT (EV_KEY, sizeof key), key);
  *touch = TEST_BIT (abs, ABS_MT_POSITION_X);
  return *touch || TEST_BIT (key, KEY_POWER) || TEST_BIT (key, KEY_VOLUMEUP) || TEST_BIT (key, KEY_VOLUMEDOWN);
}

static bool
contact (int fd)
{
  unsigned long key[NBITS (KEY_MAX)] = { 0 };
  ioctl (fd, EVIOCGKEY (sizeof key), key);
  return TEST_BIT (key, BTN_TOUCH);
}

/* Opens every device not open yet: the panel and keys at start, any that appears later (the panel again after its
 * driver is reloaded) */
static void
scan (void)
{
  DIR *d = opendir ("/dev/input");
  struct dirent *e;
  while (d && (e = readdir (d)) && n_devices < MAX_DEVICES) {
    if (strncmp (e->d_name, "event", 5))
      continue;
    char node[300];
    snprintf (node, sizeof node, "/dev/input/%s", e->d_name);
    bool known = false;
    for (int i = 0; i < n_devices && !known; i++)
      known = !strcmp (devices[i].node, node);
    if (known)
      continue;
    int fd = open (node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    bool touch;
    if (fd < 0)
      continue;
    if (!wanted (fd, &touch)) {
      close (fd);
      continue;
    }
    Device *dev = &devices[n_devices++];
    char name[128] = "";
    ioctl (fd, EVIOCGNAME (sizeof name), name);
    *dev = (Device) { .fd = fd, .touch = touch, .down = touch && contact (fd) };
    snprintf (dev->node, sizeof dev->node, "%s", node);
    printf ("input-boost: %s (%s), %s\n", node, name, touch ? "touchscreen" : "keys");
  }
  if (d)
    closedir (d);
  fflush (stdout);
}

/* Reads what the device has; false once it is gone */
static bool
drain (Device *dev)
{
  struct input_event ev[64];
  for (;;) {
    ssize_t n = read (dev->fd, ev, sizeof ev);
    if (n <= 0)
      return n == 0 || errno == EAGAIN || errno == EINTR;
    for (size_t i = 0; i < n / sizeof *ev; i++) {
      if (!dev->touch)
        continue;
      if (ev[i].type == EV_KEY && ev[i].code == BTN_TOUCH)
        dev->down = ev[i].value != 0;
      else if (ev[i].type == EV_SYN && ev[i].code == SYN_DROPPED)
        dev->down = contact (dev->fd);
    }
  }
}

static void
on_signal (int sig)
{
  quit = 1;
}

int
main (void)
{
  int hold = atoi (getenv ("INPUT_BOOST_HOLD_MS") ?: "1200");
  setvbuf (stdout, NULL, _IOLBF, 0);
  add_gpu ();
  add_cpus ();
  if (!n_floors)
    return 1;
  struct sigaction sa = { .sa_handler = on_signal };      /* no SA_RESTART: poll returns to look at quit */
  sigaction (SIGTERM, &sa, NULL);
  sigaction (SIGINT, &sa, NULL);
  int watch = inotify_init1 (IN_NONBLOCK | IN_CLOEXEC);
  inotify_add_watch (watch, "/dev/input", IN_CREATE | IN_ATTRIB);
  scan ();
  printf ("input-boost: hold %d ms\n", hold);

  bool boosted = false;
  int64_t last = 0;
  while (!quit) {
    bool held = false;
    for (int i = 0; i < n_devices; i++)
      held |= devices[i].down;
    held &= now_ms () - last < STUCK_MS;
    int64_t left = boosted ? last + (held ? STUCK_MS : hold) - now_ms () : -1;
    struct pollfd p[MAX_DEVICES + 1] = { { .fd = watch, .events = POLLIN } };
    for (int i = 0; i < n_devices; i++)
      p[i + 1] = (struct pollfd) { .fd = devices[i].fd, .events = POLLIN };
    if (poll (p, n_devices + 1, left < 0 ? -1 : (int) left) < 0 && errno != EINTR)
      break;

    bool active = false;
    for (int i = n_devices - 1; i >= 0; i--) {
      if (!p[i + 1].revents)
        continue;
      active = true;
      if (!drain (&devices[i]) || (p[i + 1].revents & (POLLERR | POLLHUP))) {
        close (devices[i].fd);
        devices[i] = devices[--n_devices];
      }
    }
    if (p[0].revents) {
      char buf[4096];
      while (read (watch, buf, sizeof buf) > 0)
        ;
      scan ();
    }
    if (active) {
      last = now_ms ();
      if (!boosted) {
        boosted = true;
        apply (true);
      }
    } else if (boosted && now_ms () >= last + (held ? STUCK_MS : hold)) {
      boosted = false;
      apply (false);
    }
  }
  if (boosted)
    apply (false);
  return 0;
}
