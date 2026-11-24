/* DictateOrb: see dictate-orb.h. Sizes are the large orb's, in dp; the small one is SMALL of it. The colours are
 * the blue, violet and magenta of the Vibe icon's background (data/agent-icon/make-icon.py), as the same diagonal
 * gradient. The cloud is drawn in GtkSnapshot, a few soft closed curves, and moves only while the frame clock ticks
 * it, which it does for listening and thinking alone. */
#include "dictate-orb.h"

#include <math.h>
#include <vibeos.h>

#define RING            216     /* the ring round a caller's initials (2 dp, the magenta at RING_ALPHA) */
#define RING_WIDTH      2
#define RING_ALPHA      0.25f
#define HALO            200     /* the halo at rest */
#define HALO_LISTENING  180     /* ... and for a caller, stronger, inside the ring */
#define SQUARE_RADIUS   0.22    /* the idle square's corners, of its side: the icon tile's 28 in 128 */
#define CLOUD_BLOBS     3       /* the cloud: soft closed curves, each in one of the three colours */
#define CLOUD_POINTS    7       /* the points a curve is drawn through */
#define CLOUD_RADIUS    76      /* the first one's radius, dp (the core's is 60) ... */
#define CLOUD_STEP      7       /* ... and how much larger each next one is */
#define CLOUD_WOBBLE    0.07    /* how far a point strays from its radius, at rest ... */
#define CLOUD_LOUD      0.07    /* ... and more at full loudness */
#define CLOUD_SWELL     0.10    /* how much larger it grows at full loudness */
#define CLOUD_ALPHA     0.8     /* at the core's edge; it fades to nothing at the outer edge */
#define HALO_ALPHA      0.08f
#define HALO_LIVE_ALPHA 0.18f
#define CORE            120
#define SMALL           0.8     /* beside a bubble: 160 and 96 */
#define GLYPH           40      /* the ✓, and the ✕ 4 dp larger */
#define INITIALS_SP     44
#define LOOK_MS         250     /* a new look comes in */
#define SIZE_MS         300     /* large to small and back */
#define BREATH_US       1400000.0  /* the halo's breath while the model answers */
#define RISE_TAU_US     60000.0 /* the halo follows a louder voice this quickly (time constant) */
#define FALL_TAU_US     250000.0/* ... and a quieter one this slowly */

struct _DictateOrb
{
  GtkWidget parent;
  DictateEngine *engine;
  DictateOrbLook look, was;     /* the look now, and the one it comes from */
  char *initials, *was_initials;
  double change;                /* from `was` to `look`, 0 .. 1 */
  double scale;                 /* 1 large .. SMALL */
  double level;                 /* the loudness as drawn, eased */
  double cloud_time;            /* seconds the cloud has moved */
  VibeosAnim *look_anim, *size_anim;
  guint tick;
  gint64 last_frame, breath_start;
};

G_DEFINE_FINAL_TYPE (DictateOrb, dictate_orb, GTK_TYPE_WIDGET)

static gboolean
animations (DictateOrb *self)
{
  gboolean enabled = TRUE;

  g_object_get (gtk_widget_get_settings (GTK_WIDGET (self)), "gtk-enable-animations", &enabled, NULL);
  return enabled;
}

/* the icon background's three colours, #1F3BFF #7B2CFF #F21FC2 */
static const GdkRGBA BLUE = { 0.1216f, 0.2314f, 1.0f, 1.0f }, VIOLET = { 0.4824f, 0.1725f, 1.0f, 1.0f },
                     MAGENTA = { 0.9490f, 0.1216f, 0.7608f, 1.0f }, WHITE = { 1, 1, 1, 1 };

/* what a look draws: the core's three gradient colours and corner radius (of its side), the halo's colour, and how
 * strong the halo, the ring and the cloud are */
typedef struct {
  GdkRGBA stop[3], halo_color, glyph;
  double corner, halo, halo_alpha, ring, cloud;
  const char *icon;
  double icon_size;
} Look;

static Look
look_of (DictateOrb *self, DictateOrbLook look, double breath)
{
  Look l = { { BLUE, VIOLET, MAGENTA }, VIOLET, WHITE, 0.5, HALO, 0, 0, 0, NULL, GLYPH };

  switch (look)
    {
    case DICTATE_ORB_LISTENING:
      l.cloud = 1;
      break;
    case DICTATE_ORB_CALLER:
      l.halo = HALO_LISTENING;
      l.halo_alpha = HALO_LIVE_ALPHA;
      l.ring = 1;
      break;
    case DICTATE_ORB_WORKING:
      l.halo_alpha = HALO_ALPHA + (HALO_LIVE_ALPHA - HALO_ALPHA) * breath;
      break;
    case DICTATE_ORB_DONE:
      l.icon = "dictate-check-symbolic";
      l.halo_alpha = HALO_ALPHA;
      break;
    case DICTATE_ORB_REFUSED:
      /* the same gradient, running into the error colour */
      l.stop[0] = MAGENTA;
      l.stop[2] = vibeos_color (VIBEOS_ERROR);
      l.stop[1] = vibeos_rgba_mix (&l.stop[0], &l.stop[2], 0.5f);
      l.halo_color = l.stop[2];
      l.halo_alpha = HALO_ALPHA;
      l.icon = "dictate-close-symbolic";
      l.icon_size = GLYPH + 4;
      break;
    case DICTATE_ORB_DIM:
      l.stop[0] = l.stop[1] = l.stop[2] = vibeos_color (VIBEOS_OUTLINE_VARIANT);
      break;
    case DICTATE_ORB_REST:
      /* a still square */
      l.corner = SQUARE_RADIUS;
      break;
    }
  return l;
}

static void
disc (GtkSnapshot *snapshot, double cx, double cy, double diameter, const GdkRGBA *color)
{
  GskRoundedRect rect;
  double r = diameter / 2;

  if (color->alpha <= 0 || r <= 0)
    return;
  gsk_rounded_rect_init_from_rect (&rect, &GRAPHENE_RECT_INIT (cx - r, cy - r, 2 * r, 2 * r), r);
  gtk_snapshot_push_rounded_clip (snapshot, &rect);
  gtk_snapshot_append_color (snapshot, color, &rect.bounds);
  gtk_snapshot_pop (snapshot);
}

/* the core: a square with the corner radius `corner` (0.5 of the side: a circle), in the diagonal gradient */
static void
core_shape (GtkSnapshot *snapshot, double cx, double cy, double side, double corner, const GdkRGBA stop[3])
{
  GskRoundedRect rect;
  graphene_rect_t *b;

  gsk_rounded_rect_init_from_rect (&rect, &GRAPHENE_RECT_INIT (cx - side / 2, cy - side / 2, side, side),
                                   (float) (side * corner));
  b = &rect.bounds;
  gtk_snapshot_push_rounded_clip (snapshot, &rect);
  gtk_snapshot_append_linear_gradient (snapshot, b, &GRAPHENE_POINT_INIT (b->origin.x, b->origin.y),
                                       &GRAPHENE_POINT_INIT (b->origin.x + b->size.width,
                                                             b->origin.y + b->size.height),
                                       (GskColorStop[3]) { { 0, stop[0] }, { 0.5f, stop[1] }, { 1, stop[2] } }, 3);
  gtk_snapshot_pop (snapshot);
}

/* the cloud round the core: CLOUD_BLOBS closed curves through CLOUD_POINTS points whose distances from the middle
 * drift (each point with its own phase), turning slowly, each filled with a radial gradient that is faint at its
 * edge, so the three blend into one soft cloud. `t` is the time in seconds (0: still), `level` the loudness,
 * `strength` how much of it shows (a look coming in or going out) */
static void
cloud (GtkSnapshot *snapshot, double cx, double cy, double scale, double t, double level, double strength)
{
  const GdkRGBA *colors[CLOUD_BLOBS] = { &BLUE, &VIOLET, &MAGENTA };
  double wobble = CLOUD_WOBBLE + CLOUD_LOUD * level;

  if (strength <= 0.003)
    return;
  for (int b = 0; b < CLOUD_BLOBS; b++)
    {
      graphene_point_t p[CLOUD_POINTS];
      double base = (CLOUD_RADIUS + CLOUD_STEP * b) * scale * (0.7 + 0.3 * strength) * (1 + CLOUD_SWELL * level);
      double reach = base * (1 + wobble) * 1.04, turn = t * (0.35 + 0.17 * b) * (b % 2 ? -1 : 1) + b * 2.1;
      GskPathBuilder *builder = gsk_path_builder_new ();
      GskPath *path;
      GdkRGBA in = *colors[b], mid = *colors[b], low = *colors[b], out = *colors[b];

      for (int i = 0; i < CLOUD_POINTS; i++)
        {
          double a = 2 * G_PI * i / CLOUD_POINTS + turn,
                 drift = sin (t * (1.1 + 0.23 * b) + i * 2.399 + b) * 0.6 + sin (t * (1.9 - 0.2 * b) + i * 4.1) * 0.4,
                 r = base * (1 + wobble * drift);

          p[i] = GRAPHENE_POINT_INIT ((float) (cx + r * cos (a)), (float) (cy + r * sin (a)));
        }
      /* Catmull-Rom through the points, as cubic curves */
      gsk_path_builder_move_to (builder, p[0].x, p[0].y);
      for (int i = 0; i < CLOUD_POINTS; i++)
        {
          const graphene_point_t *p0 = &p[(i + CLOUD_POINTS - 1) % CLOUD_POINTS], *p1 = &p[i],
                                 *p2 = &p[(i + 1) % CLOUD_POINTS], *p3 = &p[(i + 2) % CLOUD_POINTS];

          gsk_path_builder_cubic_to (builder, p1->x + (p2->x - p0->x) / 6, p1->y + (p2->y - p0->y) / 6,
                                     p2->x - (p3->x - p1->x) / 6, p2->y - (p3->y - p1->y) / 6, p2->x, p2->y);
        }
      gsk_path_builder_close (builder);
      path = gsk_path_builder_free_to_path (builder);
      in.alpha = (float) (CLOUD_ALPHA * strength);
      mid.alpha = in.alpha * 0.55f;
      low.alpha = in.alpha * 0.15f;
      out.alpha = 0;
      gtk_snapshot_push_fill (snapshot, path, GSK_FILL_RULE_WINDING);
      gtk_snapshot_append_radial_gradient (snapshot,
                                           &GRAPHENE_RECT_INIT ((float) (cx - reach), (float) (cy - reach),
                                                                (float) (2 * reach), (float) (2 * reach)),
                                           &GRAPHENE_POINT_INIT ((float) cx, (float) cy), (float) reach,
                                           (float) reach, 0, 1,
                                           (GskColorStop[4]) { { 0.0f, in }, { 0.5f, mid }, { 0.8f, low }, { 1.0f, out } },
                                           4);
      gtk_snapshot_pop (snapshot);
      gsk_path_unref (path);
    }
}

static void
glyph (DictateOrb *self, GtkSnapshot *snapshot, const Look *l, const char *initials, double cx, double cy,
       double scale, float alpha)
{
  GdkRGBA color = l->glyph;

  color.alpha *= alpha;
  if (color.alpha <= 0)
    return;
  if (l->icon)
    {
      double size = l->icon_size * scale;

      vibeos_snapshot_icon (snapshot, GTK_WIDGET (self), l->icon, cx - size / 2, cy - size / 2, size, &color);
    }
  else if (initials && *initials)
    {
      PangoLayout *layout = gtk_widget_create_pango_layout (GTK_WIDGET (self), initials);
      PangoFontDescription *font = pango_font_description_new ();
      int w, h;

      pango_font_description_set_absolute_size (font, vibeos_sp (INITIALS_SP) * scale * PANGO_SCALE);
      pango_font_description_set_weight (font, vibeos_font_weight (500));
      pango_layout_set_font_description (layout, font);
      pango_layout_get_pixel_size (layout, &w, &h);
      gtk_snapshot_save (snapshot);
      gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (cx - w / 2.0, cy - h / 2.0));
      gtk_snapshot_append_layout (snapshot, layout, &color);
      gtk_snapshot_restore (snapshot);
      pango_font_description_free (font);
      g_object_unref (layout);
    }
}

static void
dictate_orb_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  DictateOrb *self = DICTATE_ORB (widget);
  double w = gtk_widget_get_width (widget), h = gtk_widget_get_height (widget);
  double fit = MIN (1.0, MIN (w, h) / RING), scale = self->scale * fit, cx = w / 2, cy = h / 2;
  double breath = 0, t = self->change;
  Look from, to;
  GdkRGBA stop[3], halo, ring;
  double ct = animations (self) ? self->cloud_time : 0;

  if (self->look == DICTATE_ORB_WORKING || self->was == DICTATE_ORB_WORKING)
    breath = animations (self) && self->breath_start
             ? 0.5 - 0.5 * cos (2 * G_PI * (self->last_frame - self->breath_start) / BREATH_US) : 0.5;
  from = look_of (self, self->was, breath);
  to = look_of (self, self->look, breath);

  /* the cloud (under everything else): out of a listening look, or into it */
  cloud (snapshot, cx, cy, scale, ct, self->level, from.cloud + (to.cloud - from.cloud) * t);
  ring = MAGENTA;
  ring.alpha = RING_ALPHA * (float) (from.ring + (to.ring - from.ring) * t);
  if (ring.alpha > 0)
    {
      GskRoundedRect outer;
      double r = RING * scale / 2;

      gsk_rounded_rect_init_from_rect (&outer, &GRAPHENE_RECT_INIT (cx - r, cy - r, 2 * r, 2 * r), r);
      gtk_snapshot_append_border (snapshot, &outer,
                                  (float[4]) { RING_WIDTH, RING_WIDTH, RING_WIDTH, RING_WIDTH },
                                  (GdkRGBA[4]) { ring, ring, ring, ring });
    }
  halo = vibeos_rgba_mix (&from.halo_color, &to.halo_color, (float) t);
  halo.alpha = (float) (from.halo_alpha + (to.halo_alpha - from.halo_alpha) * t);
  disc (snapshot, cx, cy, MIN (from.halo + (to.halo - from.halo) * t, RING - 2 * RING_WIDTH - 2) * scale, &halo);
  for (int i = 0; i < 3; i++)
    stop[i] = vibeos_rgba_mix (&from.stop[i], &to.stop[i], (float) t);
  core_shape (snapshot, cx, cy, CORE * scale, from.corner + (to.corner - from.corner) * t, stop);
  glyph (self, snapshot, &from, self->was_initials, cx, cy, scale, (float) (1 - t));
  glyph (self, snapshot, &to, self->initials, cx, cy, scale, (float) t);
}

static void
dictate_orb_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                     int *minimum_baseline, int *natural_baseline)
{
  DictateOrb *self = DICTATE_ORB (widget);

  /* as large as it is, or what the box can spare: never under the small core */
  *minimum = (int) (CORE * SMALL);
  *natural = (int) ceil (RING * self->scale);
}

static gboolean
on_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  DictateOrb *self = DICTATE_ORB (widget);
  gint64 now = gdk_frame_clock_get_frame_time (clock);
  double dt = self->last_frame ? (double) (now - self->last_frame) : 16667.0;
  gboolean listening = self->look == DICTATE_ORB_LISTENING;
  double target = listening && self->engine ? dictate_engine_get_level (self->engine) : 0;
  double tau = target > self->level ? RISE_TAU_US : FALL_TAU_US;

  self->last_frame = now;
  self->cloud_time += dt / 1e6;
  if (!self->breath_start)
    self->breath_start = now;
  self->level += (target - self->level) * (1.0 - exp (-dt / tau));
  gtk_widget_queue_draw (widget);
  /* the clock stops with nothing moving: not listening or thinking, the loudness gone, and the cloud faded out */
  if (!listening && self->look != DICTATE_ORB_WORKING && self->level < 0.001
      && !vibeos_anim_is_running (self->look_anim))
    {
      self->tick = 0;
      self->last_frame = self->breath_start = 0;
      self->level = 0;
      return G_SOURCE_REMOVE;
    }
  return G_SOURCE_CONTINUE;
}

static void
on_change (VibeosAnim *anim, double value, gpointer data)
{
  DictateOrb *self = data;

  self->change = value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_size (VibeosAnim *anim, double value, gpointer data)
{
  DictateOrb *self = data;

  self->scale = value;
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

void
dictate_orb_set_look (DictateOrb *self, DictateOrbLook look, const char *initials)
{
  if (look == self->look && g_strcmp0 (initials, self->initials) == 0)
    return;
  self->was = self->change < 0.5 ? self->was : self->look;
  g_free (self->was_initials);
  self->was_initials = g_steal_pointer (&self->initials);
  self->look = look;
  self->initials = g_strdup (initials);
  if (animations (self) && gtk_widget_get_mapped (GTK_WIDGET (self)))
    vibeos_anim_start (self->look_anim, 0, 1, LOOK_MS, 0, SPONGY_FAST_OUT_SLOW_IN);
  else
    on_change (self->look_anim, 1, self);
  /* with animations off nothing ticks: the cloud stands still, and the clock costs nothing */
  if ((look == DICTATE_ORB_LISTENING || look == DICTATE_ORB_WORKING) && !self->tick && animations (self))
    self->tick = gtk_widget_add_tick_callback (GTK_WIDGET (self), on_tick, NULL, NULL);
  {
    const char *name = look == DICTATE_ORB_CALLER && initials ? initials : NULL;

    gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
  }
}

DictateOrbLook
dictate_orb_get_look (DictateOrb *self)
{
  return self->look;
}

void
dictate_orb_set_small (DictateOrb *self, gboolean small)
{
  double to = small ? SMALL : 1;

  if (vibeos_anim_get_to (self->size_anim) == to && (vibeos_anim_is_running (self->size_anim) || self->scale == to))
    return;
  if (animations (self) && gtk_widget_get_mapped (GTK_WIDGET (self)))
    vibeos_anim_start (self->size_anim, self->scale, to, SIZE_MS, 0, SPONGY_FAST_OUT_SLOW_IN);
  else
    {
      vibeos_anim_stop (self->size_anim);
      on_size (self->size_anim, to, self);
    }
}

static void
dictate_orb_dispose (GObject *object)
{
  DictateOrb *self = DICTATE_ORB (object);

  g_clear_pointer (&self->look_anim, vibeos_anim_free);
  g_clear_pointer (&self->size_anim, vibeos_anim_free);
  g_clear_object (&self->engine);
  G_OBJECT_CLASS (dictate_orb_parent_class)->dispose (object);
}

static void
dictate_orb_finalize (GObject *object)
{
  DictateOrb *self = DICTATE_ORB (object);

  g_free (self->initials);
  g_free (self->was_initials);
  G_OBJECT_CLASS (dictate_orb_parent_class)->finalize (object);
}

static void
dictate_orb_class_init (DictateOrbClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = dictate_orb_dispose;
  G_OBJECT_CLASS (klass)->finalize = dictate_orb_finalize;
  widget_class->snapshot = dictate_orb_snapshot;
  widget_class->measure = dictate_orb_measure;
  gtk_widget_class_set_css_name (widget_class, "dictate-orb");
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_IMG);
}

static void
dictate_orb_init (DictateOrb *self)
{
  self->scale = 1;
  self->change = 1;
  self->look_anim = vibeos_anim_new (GTK_WIDGET (self), on_change, self);
  self->size_anim = vibeos_anim_new (GTK_WIDGET (self), on_size, self);
  gtk_widget_set_can_target (GTK_WIDGET (self), FALSE);
  /* a theme change redraws it in the new colours */
  g_signal_connect_object (vibeos_theme_get_default (), "changed", G_CALLBACK (gtk_widget_queue_draw), self,
                           G_CONNECT_SWAPPED);
}

void
dictate_orb_set_engine (DictateOrb *self, DictateEngine *engine)
{
  g_set_object (&self->engine, engine);
}

GtkWidget *
dictate_orb_new (DictateEngine *engine)
{
  DictateOrb *self = g_object_new (DICTATE_TYPE_ORB, NULL);

  dictate_orb_set_engine (self, engine);
  return GTK_WIDGET (self);
}
