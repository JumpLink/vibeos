/* DictateOrb: see dictate-orb.h. Sizes are the large orb's, in dp; the small one is SMALL of it. */
#include "dictate-orb.h"

#include <math.h>
#include <vibeos.h>

#define RING            216     /* the ring round it while it listens (2 dp, the primary colour at RING_ALPHA) */
#define RING_WIDTH      2
#define RING_ALPHA      0.25f
#define HALO            200     /* the halo at rest */
#define HALO_LISTENING  180     /* ... and while it listens, stronger, inside the ring */
#define HALO_SWELL      28      /* how much further it reaches at full loudness */
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

/* what a look draws: the core's colour, the halo's, and how strong the halo and the ring are */
typedef struct {
  GdkRGBA core, halo_color, glyph;
  double halo, halo_alpha, ring;
  const char *icon;
  double icon_size;
} Look;

static Look
look_of (DictateOrb *self, DictateOrbLook look, double breath)
{
  Look l = { vibeos_color (VIBEOS_PRIMARY), vibeos_color (VIBEOS_PRIMARY), vibeos_color (VIBEOS_ON_PRIMARY), HALO,
             HALO_ALPHA, 0, NULL, GLYPH };

  switch (look)
    {
    case DICTATE_ORB_LISTENING:
    case DICTATE_ORB_CALLER:
      l.halo = HALO_LISTENING + HALO_SWELL * (look == DICTATE_ORB_LISTENING ? self->level : 0);
      l.halo_alpha = HALO_LIVE_ALPHA;
      l.ring = 1;
      break;
    case DICTATE_ORB_WORKING:
      l.halo_alpha = HALO_ALPHA + (HALO_LIVE_ALPHA - HALO_ALPHA) * breath;
      break;
    case DICTATE_ORB_DONE:
      l.icon = "dictate-check-symbolic";
      break;
    case DICTATE_ORB_REFUSED:
      l.core = vibeos_color (VIBEOS_ERROR);
      l.halo_color = vibeos_color (VIBEOS_ERROR);
      l.glyph = vibeos_color (VIBEOS_ON_ERROR);
      l.icon = "dictate-close-symbolic";
      l.icon_size = GLYPH + 4;
      break;
    case DICTATE_ORB_DIM:
      l.core = vibeos_color (VIBEOS_OUTLINE_VARIANT);
      break;
    case DICTATE_ORB_REST:
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
  GdkRGBA core, halo, ring;

  if (self->look == DICTATE_ORB_WORKING || self->was == DICTATE_ORB_WORKING)
    breath = animations (self) && self->breath_start
             ? 0.5 - 0.5 * cos (2 * G_PI * (self->last_frame - self->breath_start) / BREATH_US) : 0.5;
  from = look_of (self, self->was, breath);
  to = look_of (self, self->look, breath);

  ring = vibeos_color_alpha (VIBEOS_PRIMARY, RING_ALPHA * (float) (from.ring + (to.ring - from.ring) * t));
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
  core = vibeos_rgba_mix (&from.core, &to.core, (float) t);
  disc (snapshot, cx, cy, CORE * scale, &core);
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
  if (!self->breath_start)
    self->breath_start = now;
  self->level += (target - self->level) * (1.0 - exp (-dt / tau));
  gtk_widget_queue_draw (widget);
  if (!listening && self->look != DICTATE_ORB_WORKING && self->level < 0.001)
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
  if ((look == DICTATE_ORB_LISTENING || look == DICTATE_ORB_WORKING) && !self->tick)
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
