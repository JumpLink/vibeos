/* The parallax wallpaper (see parallax.h). The filter is the GTK 3 launcher's, tuned there on this phone. The
 * accelerometer is IIO's, or on the POCO X3 NFC and the Galaxy Tab S5e the ADSP sensor hub's, through libssc.
 * Its readings are taken as iio-sensor-proxy takes them (through the same mount matrix, so on the panel's axes:
 * x to the right, y up, the vector g itself, upright reads y < 0), then onto the screen's as it is turned. */
#include "parallax.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gfx.h"
#include "image.h"
#include "pieces.h"

#define ACCEL_DIR "/sys/bus/iio/devices"
/* Per frame at 60 Hz: time constants of ≈ 0.25 s and ≈ 0.15 s, the picture glides after the phone (owner: the
 * faster ones were "too responsive, it was smoother") */
#define SMOOTH 0.065        /* the low-pass on the sensor, against its noise */
#define EASE 0.11           /* how quickly the picture follows that */
#define RATE_RUN 60         /* Hz asked of the accelerometer while it moves, and back to RATE_IDLE */
#define RATE_IDLE 10
#define REST 0.004          /* per frame: how quickly the rest position follows the way the phone is held */
#define IDLE_MS 100         /* sensor polling while nothing moves */
/* on a table the filtered reading strays at most 0.0035 g: closer than REST_G to where it settled for REST_TICKS
 * readings, the phone lies still and the picture follows that settled reading instead */
#define REST_G 0.006
#define REST_TICKS 60
#define STILL 30            /* frames without a move: at rest, the idle poll takes over */
#define RELEASE_MS 150      /* let go after a hold: from the held picture to the tilt's */
#define TICK_US 16667       /* the filter's step, 1/60 s: its constants are per step, whatever the frame rate */
#define TICK_MS 16          /* the timer that takes the steps, while the picture moves */
#define PIECE_TILE 16       /* texels: a layer is drawn as the tiles of it that are not clear (pieces.h) */

typedef struct {
  char   *file;
  double  depth;
  GfxTex *tex;
  float   margin;           /* px of the layer beyond each screen edge */
  GfxTex *kept;             /* its texture for the screen's other shape, while it may turn back */
  GArray *pieces;           /* the parts of tex worth drawing (Piece, texels); NULL: all of it */
  GArray *kept_pieces;      /* … and of kept */
} Layer;

struct Parallax {
  char       *dir;
  GPtrArray  *layers;
  double      range;        /* dp a depth-1 layer moves either way */
  double      depth_max;    /* the fastest layer's depth, either way */
  double      tilt;         /* the tilt (of g) that moves it that far */
  int         w, h;
  double      scale;
  int         tex_w, tex_h;   /* the screen the layers' textures were made for (the old one until a decode is in) */
  int         kept_w, kept_h; /* … and the one the kept textures were made for; 0 for none */
  GCancellable *decoding;     /* the layers being decoded for w × h in a thread */
  bool        running;
  char       *accel[3];
  char       *rate;
  bool        use_ssc;      /* no IIO accelerometer: the sensor hub's, through libssc */
  GObject    *ssc;          /* its sensor, while the parallax runs */
  guint       ssc_gen;      /* a creation or opening for an older one is dropped */
  guint       ssc_retry;
  int         ssc_tries;
  bool        ssc_have;
  bool        ssc_opened;
  gint64      ssc_last;     /* monotonic µs of the last measurement (or of the opening) */
  double      ssc_g[3];     /* its last measurement, m/s² */
  double      mount[9];     /* the accelerometer's axes onto the panel's (row by row, as iio-sensor-proxy has it) */
  int         transform;    /* how the screen is turned (wl_output): onto its axes from the panel's */
  bool        tell_turn;    /* log the next reading on the screen's axes: a turn's check */
  double      g[3], rest[3], settled[3];
  int         calm;
  bool        have_g;
  double      ox, oy, tx, ty;
  /* What the layers are drawn at: ox, oy in whole px of the fastest layer on the screen (at the zoom the picture
   * rests at). A frame is asked for only when these change; the filter steps on its own timer meanwhile. */
  double      qx, qy;
  float       zoom;
  guint       follow;
  gint64      follow_t;     /* µs: the steps taken up to here */
  int         still;
  /* A transition holds the offsets (the picture keeps still, and is drawn as one); let go, the picture goes from
   * where it was held to where the tilt has it in RELEASE_MS */
  bool        held;
  bool        resync;       /* the next reading is taken as it is, not through the low-pass */
  gint64      release_t;    /* µs, while going back to the tilt; 0 otherwise */
  double      from_ox, from_oy;
  guint       gen;          /* the layers' textures: a new one each time they change */
  /* The layers drawn into one picture, at the zoom the picture rests at, beyond the screen by flat_mx/my at each
   * side (as far as the zoom of 1 shows): drawn for them while they keep still against each other */
  GfxTex     *flat;
  float       flat_zoom;
  int         flat_mx, flat_my;
  double      flat_qx, flat_qy;
  guint       flat_gen;
  bool        use_flat;     /* this frame */
  double      last_qx, last_qy;   /* the last frame's, to see the layers keep still */
  guint       last_gen;
  guint       idle;
  void      (*wake) (void *data);
  void       *wake_data;
};

/* ---- the sensor hub's accelerometer (SSC: the POCO X3 NFC's, the Tab S5e's), reached through libssc, not IIO ----
 * libssc is loaded at run time (only these have it). A sensor lives only while the parallax runs: the hub's
 * protection domain restarts at every screen-on on the POCO (surya-sleep-resume), and a sensor opened before that
 * talks to nothing. It measures at its own rate (~23 Hz on the POCO, ~13 Hz on the Tab S5e; set_rate has no say), in
 * m/s² on Android's axes; step() uses only the direction, the latest measurement at every step. */
typedef void (*SscNew) (GCancellable *, GAsyncReadyCallback, gpointer);
typedef GObject *(*SscNewFinish) (GAsyncResult *, GError **);
typedef void (*SscOpen) (GObject *, GCancellable *, GAsyncReadyCallback, gpointer);
typedef gboolean (*SscOpenFinish) (GObject *, GAsyncResult *, GError **);
static struct {
  bool          tried, ok;
  SscNew        create;
  SscNewFinish  create_finish;
  SscOpen       open, close;
  SscOpenFinish open_finish;
} ssc;
static Parallax *ssc_owner;  /* the running parallax that has the sensor */
static guint ssc_serial;     /* every start and stop takes a new one: a callback for an older one is stale */

#define SSC_TRIES 20         /* one a second: the hub's domain takes a few seconds to come back after a screen-on */
#define SSC_QUIET_US 3000000 /* no measurement for this long while open: the hub restarted under it, open anew */

static bool
ssc_load (void)
{
  if (ssc.tried)
    return ssc.ok;
  ssc.tried = true;
  void *h = dlopen ("libssc.so.2", RTLD_NOW | RTLD_LOCAL);
  if (!h)
    return false;
  ssc.create = (SscNew) dlsym (h, "ssc_sensor_accelerometer_new");
  ssc.create_finish = (SscNewFinish) dlsym (h, "ssc_sensor_accelerometer_new_finish");
  ssc.open = (SscOpen) dlsym (h, "ssc_sensor_accelerometer_open");
  ssc.open_finish = (SscOpenFinish) dlsym (h, "ssc_sensor_accelerometer_open_finish");
  ssc.close = (SscOpen) dlsym (h, "ssc_sensor_accelerometer_close");
  ssc.ok = ssc.create && ssc.create_finish && ssc.open && ssc.open_finish && ssc.close;
  return ssc.ok;
}

static void ssc_start (Parallax *p);

static gboolean
ssc_retry (gpointer data)
{
  Parallax *p = data;
  p->ssc_retry = 0;
  ssc_start (p);
  return G_SOURCE_REMOVE;
}

static void
ssc_drop (Parallax *p)
{
  if (p->ssc) {
    g_signal_handlers_disconnect_by_data (p->ssc, p);
    ssc.close (p->ssc, NULL, NULL, NULL);     /* its task keeps it until the hub answers */
    g_clear_object (&p->ssc);
  }
  p->ssc_have = false;
  p->ssc_opened = false;
}

static void
ssc_failed (Parallax *p, const char *what, GError *err)
{
  ssc_drop (p);
  if (++p->ssc_tries == 1 || p->ssc_tries == SSC_TRIES)
    g_message ("parallax: the sensor hub's accelerometer: %s%s%s%s", what, err ? ": " : "", err ? err->message : "",
               p->ssc_tries == SSC_TRIES ? " (giving up until the next screen-on)" : ", trying again");
  if (p->ssc_tries < SSC_TRIES && p->running)
    p->ssc_retry = g_timeout_add_seconds (1, ssc_retry, p);
}

static void
ssc_measured (GObject *sensor, gfloat x, gfloat y, gfloat z, gpointer data)
{
  Parallax *p = data;
  p->ssc_g[0] = x;
  p->ssc_g[1] = y;
  p->ssc_g[2] = z;
  p->ssc_have = true;
  p->ssc_last = g_get_monotonic_time ();
  if (p->ssc_tries) {       /* it works: a later failure gets its retries again */
    g_message ("parallax: the sensor hub's accelerometer measures");
    p->ssc_tries = 0;
  }
}

/* the parallax a callback was for, if it still wants that sensor */
static Parallax *
ssc_current (gpointer gen)
{
  return ssc_owner && ssc_owner->ssc_gen == GPOINTER_TO_UINT (gen) ? ssc_owner : NULL;
}

static void
ssc_opened (GObject *src, GAsyncResult *res, gpointer gen)
{
  g_autoptr (GError) err = NULL;
  gboolean ok = ssc.open_finish (src, res, &err);
  Parallax *p = ssc_current (gen);
  if (!p)
    return;                 /* stopped meanwhile: ssc_drop closed it */
  if (!ok) {
    ssc_failed (p, "open", err);
    return;
  }
  p->ssc_opened = true;
  p->ssc_last = g_get_monotonic_time ();
}

static void
ssc_created (GObject *src, GAsyncResult *res, gpointer gen)
{
  g_autoptr (GError) err = NULL;
  GObject *sensor = ssc.create_finish (res, &err);
  Parallax *p = ssc_current (gen);
  if (!p) {
    g_clear_object (&sensor);
    return;
  }
  if (!sensor) {
    ssc_failed (p, "connect", err);
    return;
  }
  p->ssc = sensor;
  g_signal_connect (sensor, "measurement", G_CALLBACK (ssc_measured), p);
  ssc.open (sensor, NULL, ssc_opened, gen);
}

static void
ssc_start (Parallax *p)
{
  if (!p->use_ssc || p->ssc || !p->running)
    return;
  if (ssc_owner && ssc_owner != p)
    return;                 /* one sensor: the other parallax has it */
  ssc_owner = p;
  p->ssc_gen = ++ssc_serial;
  ssc.create (NULL, ssc_created, GUINT_TO_POINTER (p->ssc_gen));
}

static void
ssc_stop (Parallax *p)
{
  p->ssc_gen = ++ssc_serial;
  if (p->ssc_retry) {
    g_source_remove (p->ssc_retry);
    p->ssc_retry = 0;
  }
  p->ssc_tries = 0;
  ssc_drop (p);
  if (ssc_owner == p)
    ssc_owner = NULL;
}

/* ---- the mount matrix, from where iio-sensor-proxy takes it: the device's udev ACCEL_MOUNT_MATRIX, or else the IIO
 * device's own mount matrix in sysfs. libudev is loaded at run time, as libssc is. ---- */
typedef void *(*UdevNew) (void);
typedef void *(*UdevUnref) (void *);
typedef void *(*UdevFromSyspath) (void *, const char *);
typedef const char *(*UdevProperty) (void *, const char *);

/* a udev property of the device at syspath (a /sys path, links followed), or NULL */
static char *
udev_property (const char *syspath, const char *key)
{
  static bool tried;
  static UdevNew u_new;
  static UdevUnref u_unref, d_unref;
  static UdevFromSyspath d_new;
  static UdevProperty d_get;
  if (!tried) {
    tried = true;
    void *h = dlopen ("libudev.so.1", RTLD_NOW | RTLD_LOCAL);
    if (h) {
      u_new = (UdevNew) dlsym (h, "udev_new");
      u_unref = (UdevUnref) dlsym (h, "udev_unref");
      d_new = (UdevFromSyspath) dlsym (h, "udev_device_new_from_syspath");
      d_get = (UdevProperty) dlsym (h, "udev_device_get_property_value");
      d_unref = (UdevUnref) dlsym (h, "udev_device_unref");
    }
  }
  g_autofree char *path = realpath (syspath, NULL);
  if (!path || !u_new || !u_unref || !d_new || !d_get || !d_unref)
    return NULL;
  void *u = u_new ();
  if (!u)
    return NULL;
  char *v = NULL;
  void *d = d_new (u, path);
  if (d) {
    v = g_strdup (d_get (d, key));
    d_unref (d);
  }
  u_unref (u);
  return v;
}

/* "a, b, c; d, e, f; g, h, i" (rows) into m; false, and m left alone, if it is not that */
static bool
parse_mount (const char *s, double m[9])
{
  double v[9];
  g_auto (GStrv) rows = s ? g_strsplit (s, ";", -1) : NULL;
  if (!rows || g_strv_length (rows) != 3)
    return false;
  for (int r = 0; r < 3; r++) {
    g_auto (GStrv) c = g_strsplit (rows[r], ",", -1);
    if (g_strv_length (c) != 3)
      return false;
    for (int i = 0; i < 3; i++) {
      char *end;
      v[r * 3 + i] = g_ascii_strtod (c[i], &end);
      if (end == c[i])
        return false;
    }
  }
  memcpy (m, v, sizeof v);
  return true;
}

static void
set_mount (Parallax *p, const char *matrix, const char *from)
{
  if (!parse_mount (matrix, p->mount))
    return;
  static const double id[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
  if (memcmp (p->mount, id, sizeof id))
    g_message ("parallax: the accelerometer's mount matrix (%s): %s", from, matrix);
}

/* the sensor hub's: on its fastrpc node, the one iio-sensor-proxy's rule marks ssc-accel */
static void
find_ssc_mount (Parallax *p)
{
  g_autoptr (GDir) dir = g_dir_open ("/sys/class/misc", 0, NULL);
  const char *name;
  while (dir && (name = g_dir_read_name (dir))) {
    if (!g_str_has_prefix (name, "fastrpc-"))
      continue;
    g_autofree char *path = g_build_filename ("/sys/class/misc", name, NULL);
    g_autofree char *types = udev_property (path, "IIO_SENSOR_PROXY_TYPE");
    if (!types || !strstr (types, "ssc-accel"))
      continue;
    g_autofree char *m = udev_property (path, "ACCEL_MOUNT_MATRIX");
    set_mount (p, m, name);
    return;
  }
}

static void
find_iio_mount (Parallax *p, const char *dev)
{
  g_autofree char *m = udev_property (dev, "ACCEL_MOUNT_MATRIX");
  if (m) {
    set_mount (p, m, "udev");
    return;
  }
  const char *files[] = { "in_accel_mount_matrix", "in_mount_matrix", "mount_matrix" };
  for (guint i = 0; i < G_N_ELEMENTS (files); i++) {
    g_autofree char *f = g_build_filename (dev, files[i], NULL);
    g_autofree char *s = NULL;
    if (g_file_get_contents (f, &s, NULL, NULL)) {
      set_mount (p, g_strstrip (s), files[i]);
      return;
    }
  }
}

static void
find_accel (Parallax *p)
{
  static const double id[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
  memcpy (p->mount, id, sizeof id);
  /* VIBEOS_ACCEL_DIR: a directory in place of the IIO devices', for a test that writes the readings itself */
  const char *base = g_getenv ("VIBEOS_ACCEL_DIR") ? g_getenv ("VIBEOS_ACCEL_DIR") : ACCEL_DIR;
  g_autoptr (GDir) dir = g_dir_open (base, 0, NULL);
  const char *name;
  while (dir && (name = g_dir_read_name (dir))) {
    g_autofree char *x = g_build_filename (base, name, "in_accel_x_raw", NULL);
    if (!g_file_test (x, G_FILE_TEST_EXISTS))
      continue;
    const char *axes[] = { "x", "y", "z" };
    for (int i = 0; i < 3; i++) {
      g_autofree char *f = g_strdup_printf ("in_accel_%s_raw", axes[i]);
      p->accel[i] = g_build_filename (base, name, f, NULL);
    }
    p->rate = g_build_filename (base, name, "in_accel_sampling_frequency", NULL);
    if (!g_getenv ("VIBEOS_ACCEL_DIR")) {
      g_autofree char *dev = g_build_filename (base, name, NULL);
      find_iio_mount (p, dev);
    }
    return;
  }
  if (ssc_load ()) {
    p->use_ssc = true;
    g_message ("parallax: the sensor hub's accelerometer (libssc)");
    find_ssc_mount (p);
    return;
  }
  g_message ("parallax: no accelerometer, the wallpaper stays still");
}

static void
set_rate (Parallax *p, int hz)
{
  char v[16];
  int fd = p->rate ? open (p->rate, O_WRONLY) : -1;
  if (fd < 0)
    return;
  int n = g_snprintf (v, sizeof v, "%d", hz);
  if (write (fd, v, n) != n)
    g_debug ("parallax: the accelerometer rate stays");
  close (fd);
}

static bool
read_raw (Parallax *p, double g[3])
{
  if (p->use_ssc) {
    if (p->ssc_opened && !p->ssc_retry && g_get_monotonic_time () - p->ssc_last > SSC_QUIET_US) {
      ssc_failed (p, "no measurements", NULL);
      return false;
    }
    if (!p->ssc_have)
      return false;
    memcpy (g, p->ssc_g, sizeof p->ssc_g);
    return true;
  }
  for (int i = 0; i < 3; i++) {
    char buf[32];
    if (!p->accel[i])
      return false;
    int fd = open (p->accel[i], O_RDONLY);
    if (fd < 0)
      return false;
    ssize_t n = read (fd, buf, sizeof buf - 1);
    close (fd);
    if (n <= 0)
      return false;
    buf[n] = '\0';
    g[i] = g_ascii_strtod (buf, NULL);
  }
  return true;
}

/* a reading on the screen's axes as it shows: the sensor's through the mount matrix onto the panel's, then turned
 * with the screen (wl_output transform 1: the content 90° counter-clockwise, the device turned clockwise with its
 * left edge up, so the screen's right is the panel's up and its up the panel's left) */
static bool
read_accel (Parallax *p, double g[3])
{
  double s[3];
  if (!read_raw (p, s))
    return false;
  const double *m = p->mount;
  double x = m[0] * s[0] + m[1] * s[1] + m[2] * s[2];
  double y = m[3] * s[0] + m[4] * s[1] + m[5] * s[2];
  g[2] = m[6] * s[0] + m[7] * s[1] + m[8] * s[2];
  switch (p->transform & 3) {
  case 1: g[0] = y;  g[1] = -x; break;
  case 2: g[0] = -x; g[1] = -y; break;
  case 3: g[0] = -y; g[1] = x;  break;
  default: g[0] = x; g[1] = y;  break;
  }
  if (p->tell_turn) {
    p->tell_turn = false;
    double n = sqrt (g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
    if (n > 0)                /* held upright on the screen as it shows: x ≈ 0, y ≈ -1 */
      g_message ("parallax: on the screen's axes (transform %d): x %.2f y %.2f z %.2f", p->transform,
                 g[0] / n, g[1] / n, g[2] / n);
  }
  return true;
}

/* one reading into the filters: where the tilt has the picture now (p->tx, p->ty); false without a reading */
static bool
filter (Parallax *p)
{
  double g[3];
  if (!read_accel (p, g))
    return false;
  double n = sqrt (g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
  if (n <= 0)
    return false;
  for (int i = 0; i < 3; i++)
    g[i] /= n;
  if (!p->have_g) {
    memcpy (p->g, g, sizeof g);
    memcpy (p->rest, g, sizeof g);
    memcpy (p->settled, g, sizeof g);
    p->calm = 0;
    p->have_g = true;
  }
  if (p->resync) {
    memcpy (p->g, g, sizeof g);     /* back from a hold: the phone as it is held now, not the low-pass's way there */
    p->resync = false;
  }
  double off = 0;
  for (int i = 0; i < 3; i++) {
    p->g[i] += (g[i] - p->g[i]) * SMOOTH;
    off = MAX (off, fabs (p->g[i] - p->settled[i]));
  }
  if (off > REST_G) {
    memcpy (p->settled, p->g, sizeof p->g);
    p->calm = 0;
  } else if (p->calm < REST_TICKS) {
    p->calm++;
  }
  const double *in = p->calm >= REST_TICKS ? p->settled : p->g;
  for (int i = 0; i < 3; i++)
    p->rest[i] += (in[i] - p->rest[i]) * REST;
  /* x: the right edge dipping; y: the top edge coming towards you */
  p->tx = CLAMP ((in[0] - p->rest[0]) / p->tilt, -1, 1);
  p->ty = CLAMP ((in[1] - p->rest[1]) / p->tilt, -1, 1);
  return true;
}

/* ox, oy to what the layers are drawn at; true when that changed */
static bool
quantize (Parallax *p)
{
  double px = p->range * p->depth_max * p->scale * p->zoom;     /* the fastest layer's px per unit of offset */
  double qx = px > 0 ? round (p->ox * px) / px : p->ox, qy = px > 0 ? round (p->oy * px) / px : p->oy;
  if (qx == p->qx && qy == p->qy)
    return false;
  p->qx = qx;
  p->qy = qy;
  return true;
}

/* one reading: true when the picture moved by more than a hair */
static bool
step (Parallax *p)
{
  if (!filter (p))
    return false;
  double ox = p->ox + (p->tx - p->ox) * EASE;
  double oy = p->oy + (p->ty - p->oy) * EASE;
  double moved = (fabs (ox - p->ox) + fabs (oy - p->oy)) * p->range * p->scale;
  if (moved < 0.02)
    return false;
  p->ox = ox;
  p->oy = oy;
  return true;
}

static void
wake (Parallax *p)
{
  if (p->wake)
    p->wake (p->wake_data);
}

static gboolean idle_poll (gpointer data);

/* The picture follows the tilt: a filter step every 1/60 s, a frame only when that moves the layers a px. Still for
 * STILL steps, it rests: one more frame draws its layers as one picture for the next transition, and the idle poll
 * watches for the next move. */
static gboolean
follow (gpointer data)
{
  Parallax *p = data;
  gint64 now = g_get_monotonic_time ();
  if (now - p->follow_t > 4 * TICK_US)
    p->follow_t = now - TICK_US;    /* the main loop was held up: the steps missed are not made up */
  while (now - p->follow_t >= TICK_US) {
    p->follow_t += TICK_US;
    if (step (p))
      p->still = 0;
    else
      p->still++;
  }
  if (quantize (p))
    wake (p);
  if (p->still < STILL)
    return G_SOURCE_CONTINUE;
  p->follow = 0;
  set_rate (p, RATE_IDLE);
  if (!p->idle)
    p->idle = g_timeout_add (IDLE_MS, idle_poll, p);
  wake (p);
  return G_SOURCE_REMOVE;
}

static void
follow_start (Parallax *p)
{
  if (p->follow || !p->running || p->held || p->release_t)
    return;
  if (p->idle) {
    g_source_remove (p->idle);
    p->idle = 0;
  }
  set_rate (p, RATE_RUN);
  p->still = 0;
  p->follow_t = g_get_monotonic_time () - TICK_US;
  p->follow = g_timeout_add (TICK_MS, follow, p);
}

static void
follow_stop (Parallax *p)
{
  if (p->follow) {
    g_source_remove (p->follow);
    p->follow = 0;
  }
  if (p->idle) {
    g_source_remove (p->idle);
    p->idle = 0;
  }
}

static gboolean
idle_poll (gpointer data)
{
  Parallax *p = data;
  if (!p->running || p->held) {
    p->idle = 0;
    return G_SOURCE_REMOVE;
  }
  if (step (p)) {
    p->idle = 0;
    if (quantize (p))
      wake (p);
    follow_start (p);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

/* let go: from the held offsets to the tilt's, decelerating; the tilt's own easing takes over at the end */
static bool
release_step (Parallax *p)
{
  filter (p);
  double t = (g_get_monotonic_time () - p->release_t) / (RELEASE_MS * 1000.0);
  if (t >= 1) {
    p->ox = p->tx;
    p->oy = p->ty;
    p->release_t = 0;
    quantize (p);
    follow_start (p);
    return true;
  }
  double e = 1 - (1 - t) * (1 - t);
  p->ox = p->from_ox + (p->tx - p->from_ox) * e;
  p->oy = p->from_oy + (p->ty - p->from_oy) * e;
  quantize (p);
  return true;
}

void
parallax_hold (Parallax *p, bool hold)
{
  if (!p || p->held == hold)
    return;
  p->held = hold;
  if (hold) {
    p->release_t = 0;               /* held where a release had got to */
    follow_stop (p);
    if (p->running)
      set_rate (p, RATE_IDLE);      /* nothing reads it until the hold ends */
    return;
  }
  if (!p->running)
    return;
  set_rate (p, RATE_RUN);
  p->from_ox = p->qx;               /* from the picture as it was held */
  p->from_oy = p->qy;
  p->release_t = g_get_monotonic_time ();
  p->resync = true;
  p->still = 0;
}

bool
parallax_tick (Parallax *p)
{
  /* held, the picture keeps still; else, but for the way back from a hold, follow () asks for the frames it needs */
  if (!p || !p->running || p->held || !p->release_t)
    return false;
  return release_step (p);
}

static void
layer_free (gpointer data)
{
  Layer *l = data;
  gfx_tex_free (l->tex);
  gfx_tex_free (l->kept);
  g_clear_pointer (&l->pieces, g_array_unref);
  g_clear_pointer (&l->kept_pieces, g_array_unref);
  g_free (l->file);
  g_free (l);
}

/* the parts of a layer's picture worth drawing; NULL: all of it */
static GArray *
layer_pieces (GdkPixbuf *pb)
{
  if (!pb || !gdk_pixbuf_get_has_alpha (pb) || gdk_pixbuf_get_n_channels (pb) != 4)
    return NULL;
  return pieces_find (gdk_pixbuf_read_pixels (pb), gdk_pixbuf_get_width (pb), gdk_pixbuf_get_height (pb),
                      gdk_pixbuf_get_rowstride (pb), PIECE_TILE);
}

Parallax *
parallax_new (const char *dir, void (*wake) (void *data), void *data)
{
  g_autofree char *json = g_build_filename (dir, "parallax.json", NULL);
  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_file (parser, json, NULL))
    return NULL;
  JsonObject *o = json_node_get_object (json_parser_get_root (parser));
  JsonArray *ls = o && json_object_has_member (o, "layers") ? json_object_get_array_member (o, "layers") : NULL;
  if (!ls)
    return NULL;
  Parallax *p = g_new0 (Parallax, 1);
  p->dir = g_strdup (dir);
  p->zoom = 1;
  p->wake = wake;
  p->wake_data = data;
  p->layers = g_ptr_array_new_with_free_func (layer_free);
  p->range = json_object_has_member (o, "range") ? json_object_get_double_member (o, "range") : 30;
  p->tilt = json_object_has_member (o, "tilt") ? json_object_get_double_member (o, "tilt") : 0.2;
  for (guint i = 0; i < json_array_get_length (ls); i++) {
    JsonObject *lo = json_array_get_object_element (ls, i);
    Layer *l = g_new0 (Layer, 1);
    l->file = g_build_filename (dir, json_object_get_string_member (lo, "file"), NULL);
    l->depth = json_object_has_member (lo, "depth") ? json_object_get_double_member (lo, "depth") : 0;
    p->depth_max = MAX (p->depth_max, fabs (l->depth));
    g_ptr_array_add (p->layers, l);
  }
  find_accel (p);
  return p;
}

Parallax *
parallax_new_image (const char *file)
{
  Parallax *p = g_new0 (Parallax, 1);
  p->dir = g_path_get_dirname (file);
  p->zoom = 1;
  p->layers = g_ptr_array_new_with_free_func (layer_free);
  p->tilt = 0.2;
  Layer *l = g_new0 (Layer, 1);
  l->file = g_strdup (file);
  g_ptr_array_add (p->layers, l);
  return p;
}

void
parallax_free (Parallax *p)
{
  if (!p)
    return;
  follow_stop (p);
  if (p->decoding) {
    g_cancellable_cancel (p->decoding);
    g_clear_object (&p->decoding);
  }
  ssc_stop (p);
  set_rate (p, RATE_IDLE);
  gfx_tex_free (p->flat);
  g_ptr_array_unref (p->layers);
  for (int i = 0; i < 3; i++)
    g_free (p->accel[i]);
  g_free (p->rate);
  g_free (p->dir);
  g_free (p);
}

/* A layer's texture size for a w × h screen: the screen and its margin at each side */
static void
layer_size (Parallax *p, Layer *l, int w, int h, int *bw, int *bh)
{
  l->margin = ceilf ((float) (p->range * fabs (l->depth) * p->scale));
  *bw = w + 2 * (int) l->margin;
  *bh = h + 2 * (int) l->margin;
}

typedef struct {
  guint       n;
  char      **files;
  int        *bw, *bh;
  GdkPixbuf **out;
  GArray    **pieces;
  int         w, h;
} Decode;

static void
decode_free (gpointer data)
{
  Decode *d = data;
  for (guint i = 0; i < d->n; i++) {
    g_free (d->files[i]);
    g_clear_object (&d->out[i]);
    g_clear_pointer (&d->pieces[i], g_array_unref);
  }
  g_free (d->files);
  g_free (d->pieces);
  g_free (d->bw);
  g_free (d->bh);
  g_free (d->out);
  g_free (d);
}

static void
decode_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  Decode *d = data;
  for (guint i = 0; i < d->n && !g_cancellable_is_cancelled (cancellable); i++) {
    d->out[i] = image_load (d->files[i], d->bw[i], d->bh[i], TRUE);
    d->pieces[i] = layer_pieces (d->out[i]);
  }
  g_task_return_boolean (task, TRUE);
}

/* the layers decoded for the new shape are in: they take over, and the old ones are kept for a turn back */
static void
decode_done (GObject *source, GAsyncResult *res, gpointer data)
{
  GTask *task = G_TASK (res);
  if (g_cancellable_is_cancelled (g_task_get_cancellable (task)))
    return;                 /* the picture or its parallax is gone, or the screen turned back first */
  Parallax *p = data;
  Decode *d = g_task_get_task_data (task);
  for (guint i = 0; i < d->n && i < p->layers->len; i++) {
    Layer *l = p->layers->pdata[i];
    gfx_tex_free (l->kept);
    l->kept = l->tex;
    l->tex = d->out[i] ? image_texture (d->out[i]) : NULL;
    g_clear_pointer (&l->kept_pieces, g_array_unref);
    l->kept_pieces = l->pieces;
    l->pieces = g_steal_pointer (&d->pieces[i]);
  }
  p->kept_w = p->tex_w;
  p->kept_h = p->tex_h;
  p->tex_w = d->w;
  p->tex_h = d->h;
  p->gen++;
  g_clear_object (&p->decoding);
  g_message ("parallax: %u layers for %dx%d decoded", p->layers->len, d->w, d->h);
  if (p->wake)
    p->wake (p->wake_data);
}

/* The screen's size changed. The first time, the layers are decoded for it at once. A turned screen (a tablet's) has
 * its picture at once if it was this shape before (the textures are kept for both), else the layers are decoded in a
 * thread while the old ones, scaled to cover the new shape, stand in: decoding them took the main loop 2.5 s, and
 * meanwhile the turned window showed its old frame cut by the new shape. */
void
parallax_resize (Parallax *p, int w, int h, double scale)
{
  if (p->w == w && p->h == h && p->scale == scale)
    return;
  bool first = !p->tex_w || p->scale != scale;
  p->w = w;
  p->h = h;
  p->scale = scale;
  if (p->decoding) {
    g_cancellable_cancel (p->decoding);
    g_clear_object (&p->decoding);
  }
  if (first) {
    gint64 t0 = g_get_monotonic_time ();
    for (guint i = 0; i < p->layers->len; i++) {
      Layer *l = p->layers->pdata[i];
      int bw, bh;
      layer_size (p, l, w, h, &bw, &bh);
      GdkPixbuf *pb = image_load (l->file, bw, bh, TRUE);
      gfx_tex_free (l->tex);
      gfx_tex_free (l->kept);
      l->kept = NULL;
      l->tex = pb ? image_texture (pb) : NULL;
      g_clear_pointer (&l->pieces, g_array_unref);
      g_clear_pointer (&l->kept_pieces, g_array_unref);
      l->pieces = layer_pieces (pb);
      g_clear_object (&pb);
    }
    p->tex_w = w;
    p->tex_h = h;
    p->kept_w = p->kept_h = 0;
    p->gen++;
    g_message ("parallax: %u layers for %dx%d in %.0f ms", p->layers->len, w, h,
               (g_get_monotonic_time () - t0) / 1000.0);
    return;
  }
  if (p->tex_w == w && p->tex_h == h)
    return;                 /* turned back before the decode was in */
  if (p->kept_w == w && p->kept_h == h) {
    for (guint i = 0; i < p->layers->len; i++) {
      Layer *l = p->layers->pdata[i];
      GfxTex *t = l->tex;
      l->tex = l->kept;
      l->kept = t;
      GArray *pc = l->pieces;
      l->pieces = l->kept_pieces;
      l->kept_pieces = pc;
    }
    p->kept_w = p->tex_w;
    p->kept_h = p->tex_h;
    p->tex_w = w;
    p->tex_h = h;
    p->gen++;
    return;
  }
  Decode *d = g_new0 (Decode, 1);
  d->n = p->layers->len;
  d->files = g_new0 (char *, d->n);
  d->bw = g_new0 (int, d->n);
  d->bh = g_new0 (int, d->n);
  d->out = g_new0 (GdkPixbuf *, d->n);
  d->pieces = g_new0 (GArray *, d->n);
  d->w = w;
  d->h = h;
  for (guint i = 0; i < d->n; i++) {
    Layer *l = p->layers->pdata[i];
    d->files[i] = g_strdup (l->file);
    layer_size (p, l, w, h, &d->bw[i], &d->bh[i]);
  }
  p->decoding = g_cancellable_new ();
  g_autoptr (GTask) task = g_task_new (NULL, p->decoding, decode_done, p);
  g_task_set_task_data (task, d, decode_free);
  g_task_run_in_thread (task, decode_thread);
}

void
parallax_set_wake (Parallax *p, void (*wake) (void *data), void *data)
{
  p->wake = wake;
  p->wake_data = data;
}

void
parallax_set_transform (Parallax *p, int transform)
{
  if (!p || p->transform == transform)
    return;
  p->transform = transform;
  p->have_g = false;        /* the axes turned under the filter: it starts again from the reading as it comes */
  p->tell_turn = true;
}

void
parallax_run (Parallax *p, bool run)
{
  if (!p || p->running == run || (!p->accel[0] && !p->use_ssc))
    return;                 /* a still picture has no sensor to follow */
  p->running = run;
  p->still = 0;
  set_rate (p, run ? RATE_RUN : RATE_IDLE);
  if (run) {
    ssc_start (p);
    follow_start (p);
  } else {
    ssc_stop (p);
    follow_stop (p);
  }
}

/* a layer's picture, at x y w h, as its pieces: what is clear in it is not drawn */
static void
draw_pieces (GfxTex *t, GArray *pieces, float x, float y, float w, float h)
{
  float kx = w / t->w, ky = h / t->h;
  for (guint i = 0; i < pieces->len; i++) {
    const Piece *c = &g_array_index (pieces, Piece, i);
    /* an edge two pieces share is the same texel number put through the same sums: the same float, so no seam */
    gfx_image_piece (t, x + c->x0 * kx, y + c->y0 * ky, x + c->x1 * kx, y + c->y1 * ky,
                     (float) c->x0 / t->w, (float) c->y0 / t->h, (float) c->x1 / t->w, (float) c->y1 / t->h);
  }
}

/* the layers, back to front, as they sit now on the w × h screen; the first thing drawn in its pass, so the back one
 * is written, not blended, over the pass's black (the same pixels: the textures are premultiplied) */
static void
draw_layers (Parallax *p)
{
  bool back = true;
  for (guint i = 0; i < p->layers->len; i++) {
    Layer *l = p->layers->pdata[i];
    if (!l->tex)
      continue;
    /* depth -1…1: the layers behind the screen plane (negative) move against the ones in front */
    float dx = (float) (-p->qx * p->range * l->depth * p->scale);
    float dy = (float) (-p->qy * p->range * l->depth * p->scale);
    float x = -(l->margin + dx), y = -(l->margin + dy), w = l->tex->w, h = l->tex->h;
    if (p->tex_w != p->w || p->tex_h != p->h) {
      /* made for the screen's old shape, until the new one's is in: scaled to cover it, about its centre */
      float k = MAX ((float) p->w / p->tex_w, (float) p->h / p->tex_h);
      w = l->tex->w * k;
      h = l->tex->h * k;
      x = (p->w - w) / 2 - dx;
      y = (p->h - h) / 2 - dy;
    }
    if (back)
      gfx_image_opaque (l->tex, x, y, w, h);
    else if (l->pieces)
      draw_pieces (l->tex, l->pieces, x, y, w, h);
    else
      gfx_image (l->tex, x, y, w, h, 0, 1);
    back = false;
  }
}

void
parallax_prepare (Parallax *p, float rest_zoom)
{
  if (!p)
    return;
  if (p->zoom != rest_zoom) {
    p->zoom = rest_zoom;
    quantize (p);
  }
  bool moved = p->qx != p->last_qx || p->qy != p->last_qy || p->gen != p->last_gen;
  p->last_qx = p->qx;
  p->last_qy = p->qy;
  p->last_gen = p->gen;
  /* past the screen far enough to cover it at a zoom of 1 */
  int mx = (int) ceilf (p->w * (rest_zoom - 1) / 2), my = (int) ceilf (p->h * (rest_zoom - 1) / 2);
  p->use_flat = p->flat && p->flat_gen == p->gen && p->flat_qx == p->qx && p->flat_qy == p->qy &&
                p->flat_zoom == rest_zoom && p->flat->w == p->w + 2 * mx && p->flat->h == p->h + 2 * my;
  if (p->use_flat || moved || p->w <= 0 || p->h <= 0 || rest_zoom < 1)
    return;
  if (!p->flat || p->flat->w != p->w + 2 * mx || p->flat->h != p->h + 2 * my) {
    gfx_tex_free (p->flat);
    p->flat = gfx_target_new (p->w + 2 * mx, p->h + 2 * my);
    if (!p->flat)
      return;
  }
  gfx_pass_begin (p->flat, (GfxColor) { 0, 0, 0, 1 });
  gfx_translate (mx, my);
  gfx_scale (rest_zoom, rest_zoom, p->w / 2.f, p->h / 2.f);
  draw_layers (p);
  gfx_pass_end ();
  p->flat_zoom = rest_zoom;
  p->flat_mx = mx;
  p->flat_my = my;
  p->flat_qx = p->qx;
  p->flat_qy = p->qy;
  p->flat_gen = p->gen;
  p->use_flat = true;
}

void
parallax_draw (Parallax *p, float zoom)
{
  gfx_save ();
  if (p->use_flat && zoom >= 1) {
    /* at the rest zoom, pixel for pixel what the layers draw */
    float k = zoom / p->flat_zoom;
    if (k != 1)
      gfx_scale (k, k, p->w / 2.f, p->h / 2.f);
    gfx_image_opaque (p->flat, -p->flat_mx, -p->flat_my, p->flat->w, p->flat->h);
  } else {
    if (zoom != 1)
      gfx_scale (zoom, zoom, p->w / 2.f, p->h / 2.f);
    draw_layers (p);
  }
  gfx_restore ();
}
