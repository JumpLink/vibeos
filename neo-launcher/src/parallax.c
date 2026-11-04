/* The parallax wallpaper (see parallax.h). The filter is the GTK 3 launcher's, tuned there on this phone. */
#include "parallax.h"

#include <fcntl.h>
#include <glib.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

#include "gfx.h"
#include "image.h"

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

typedef struct {
  char   *file;
  double  depth;
  GfxTex *tex;
  float   margin;           /* px of the layer beyond each screen edge */
} Layer;

struct Parallax {
  char       *dir;
  GPtrArray  *layers;
  double      range;        /* dp a depth-1 layer moves either way */
  double      tilt;         /* the tilt (of g) that moves it that far */
  int         w, h;
  double      scale;
  bool        running;
  char       *accel[3];
  char       *rate;
  double      g[3], rest[3], settled[3];
  int         calm;
  bool        have_g;
  double      ox, oy, tx, ty;
  int         still;
  guint       idle;
  void      (*wake) (void *data);
  void       *wake_data;
};

static void
find_accel (Parallax *p)
{
  g_autoptr (GDir) dir = g_dir_open (ACCEL_DIR, 0, NULL);
  const char *name;
  while (dir && (name = g_dir_read_name (dir))) {
    g_autofree char *x = g_build_filename (ACCEL_DIR, name, "in_accel_x_raw", NULL);
    if (!g_file_test (x, G_FILE_TEST_EXISTS))
      continue;
    const char *axes[] = { "x", "y", "z" };
    for (int i = 0; i < 3; i++) {
      g_autofree char *f = g_strdup_printf ("in_accel_%s_raw", axes[i]);
      p->accel[i] = g_build_filename (ACCEL_DIR, name, f, NULL);
    }
    p->rate = g_build_filename (ACCEL_DIR, name, "in_accel_sampling_frequency", NULL);
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
read_accel (Parallax *p, double g[3])
{
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

/* one reading: true when the picture moved by more than a hair */
static bool
step (Parallax *p)
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
  double ox = p->ox + (p->tx - p->ox) * EASE;
  double oy = p->oy + (p->ty - p->oy) * EASE;
  double moved = (fabs (ox - p->ox) + fabs (oy - p->oy)) * p->range * p->scale;
  if (moved < 0.02)
    return false;
  p->ox = ox;
  p->oy = oy;
  return true;
}

static gboolean
idle_poll (gpointer data)
{
  Parallax *p = data;
  if (!p->running) {
    p->idle = 0;
    return G_SOURCE_REMOVE;
  }
  if (step (p)) {
    set_rate (p, RATE_RUN);
    p->still = 0;
    p->idle = 0;
    if (p->wake)
      p->wake (p->wake_data);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

bool
parallax_tick (Parallax *p)
{
  if (!p || !p->running)
    return false;
  if (p->still >= STILL)
    return false;           /* the idle poll has it */
  if (step (p)) {
    p->still = 0;
    return true;
  }
  if (++p->still == STILL) {
    set_rate (p, RATE_IDLE);
    if (!p->idle)
      p->idle = g_timeout_add (IDLE_MS, idle_poll, p);
  }
  return true;              /* a few more frames before it counts as still */
}

static void
layer_free (gpointer data)
{
  Layer *l = data;
  gfx_tex_free (l->tex);
  g_free (l->file);
  g_free (l);
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
    g_ptr_array_add (p->layers, l);
  }
  find_accel (p);
  return p;
}

void
parallax_free (Parallax *p)
{
  if (!p)
    return;
  if (p->idle)
    g_source_remove (p->idle);
  set_rate (p, RATE_IDLE);
  g_ptr_array_unref (p->layers);
  for (int i = 0; i < 3; i++)
    g_free (p->accel[i]);
  g_free (p->rate);
  g_free (p->dir);
  g_free (p);
}

void
parallax_resize (Parallax *p, int w, int h, double scale)
{
  if (p->w == w && p->h == h && p->scale == scale)
    return;
  p->w = w;
  p->h = h;
  p->scale = scale;
  gint64 t0 = g_get_monotonic_time ();
  for (guint i = 0; i < p->layers->len; i++) {
    Layer *l = p->layers->pdata[i];
    l->margin = ceilf ((float) (p->range * fabs (l->depth) * scale));
    int bw = w + 2 * (int) l->margin, bh = h + 2 * (int) l->margin;
    GdkPixbuf *pb = image_load (l->file, bw, bh, TRUE);
    gfx_tex_free (l->tex);
    l->tex = pb ? image_texture (pb) : NULL;
    g_clear_object (&pb);
  }
  g_message ("parallax: %u layers for %dx%d in %.0f ms", p->layers->len, w, h,
             (g_get_monotonic_time () - t0) / 1000.0);
}

void
parallax_run (Parallax *p, bool run)
{
  if (!p || p->running == run)
    return;
  p->running = run;
  p->still = 0;
  set_rate (p, run ? RATE_RUN : RATE_IDLE);
  if (!run && p->idle) {
    g_source_remove (p->idle);
    p->idle = 0;
  }
}

void
parallax_draw (Parallax *p, float zoom)
{
  gfx_save ();
  if (zoom != 1)
    gfx_scale (zoom, zoom, p->w / 2.f, p->h / 2.f);
  for (guint i = 0; i < p->layers->len; i++) {
    Layer *l = p->layers->pdata[i];
    if (!l->tex)
      continue;
    /* depth -1…1: the layers behind the screen plane (negative) move against the ones in front */
    float dx = (float) (-p->ox * p->range * l->depth * p->scale);
    float dy = (float) (-p->oy * p->range * l->depth * p->scale);
    gfx_image (l->tex, -(l->margin + dx), -(l->margin + dy), l->tex->w, l->tex->h, 0, 1);
  }
  gfx_restore ();
}
