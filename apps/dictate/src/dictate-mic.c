/* DictateMic: see dictate-mic.h. */
#include "dictate-mic.h"

#include <glib/gi18n.h>
#include <math.h>
#include <vibeos.h>

#define HALO_REST       8       /* px the disc reaches past the FAB's edge while it listens to silence */
#define HALO_SWELL      28      /* ... and further at full loudness */
#define RISE_TAU_US     60000.0 /* the disc follows a louder voice this quickly (time constant) */
#define FALL_TAU_US     250000.0/* ... and a quieter one this slowly */
#define FADE_MS         200     /* the disc coming and going */

struct _DictateMic
{
  GtkWidget parent;
  DictateEngine *engine;
  GtkWidget *fab;
  gboolean listening;
  double level;                 /* the loudness as drawn, eased */
  double shown;                 /* the disc's opacity, 0 .. 1 */
  VibeosAnim *fade;
  guint tick;
  gint64 last_frame;
};

enum { SIGNAL_CLICKED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (DictateMic, dictate_mic, GTK_TYPE_WIDGET)

static gboolean
on_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  DictateMic *self = DICTATE_MIC (widget);
  gint64 now = gdk_frame_clock_get_frame_time (clock);
  double dt = self->last_frame ? (double) (now - self->last_frame) : 16667.0;
  double target = self->listening ? dictate_engine_get_level (self->engine) : 0;
  double tau = target > self->level ? RISE_TAU_US : FALL_TAU_US;

  self->last_frame = now;
  self->level += (target - self->level) * (1.0 - exp (-dt / tau));
  gtk_widget_queue_draw (widget);
  if (!self->listening && self->shown == 0)
    {
      self->tick = 0;
      self->last_frame = 0;
      self->level = 0;
      return G_SOURCE_REMOVE;
    }
  return G_SOURCE_CONTINUE;
}

static void
on_fade (VibeosAnim *anim, double value, gpointer data)
{
  DictateMic *self = data;

  self->shown = value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
dictate_mic_set_listening (DictateMic *self, gboolean listening)
{
  if (self->listening == listening)
    return;
  self->listening = listening;
  vibeos_fab_set_icon (VIBEOS_PRESSABLE (self->fab), listening ? "dictate-stop-symbolic" : "dictate-mic-symbolic");
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->fab), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  listening ? _("Stop") : _("Start dictating"), -1);
  vibeos_anim_start (self->fade, self->shown, listening ? 1 : 0, FADE_MS, 0, SPONGY_FAST_OUT_SLOW_IN);
  if (!self->tick)
    self->tick = gtk_widget_add_tick_callback (GTK_WIDGET (self), on_tick, NULL, NULL);
}

static void
dictate_mic_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  DictateMic *self = DICTATE_MIC (widget);
  double w = gtk_widget_get_width (widget), h = gtk_widget_get_height (widget);

  if (self->shown > 0)
    {
      double radius = MIN (w, h) / 2 + (HALO_REST + HALO_SWELL * self->level) * self->shown;
      GdkRGBA halo = vibeos_color_alpha (VIBEOS_PRIMARY, 0.24f * (float) self->shown);
      GskRoundedRect disc;

      gsk_rounded_rect_init_from_rect (&disc, &GRAPHENE_RECT_INIT (w / 2 - radius, h / 2 - radius, 2 * radius,
                                                                   2 * radius), radius);
      gtk_snapshot_push_rounded_clip (snapshot, &disc);
      gtk_snapshot_append_color (snapshot, &halo, &disc.bounds);
      gtk_snapshot_pop (snapshot);
    }
  gtk_widget_snapshot_child (widget, self->fab, snapshot);
}

static void
on_clicked (VibeosPressable *fab, DictateMic *self)
{
  g_signal_emit (self, signals[SIGNAL_CLICKED], 0);
}

static void
dictate_mic_dispose (GObject *object)
{
  DictateMic *self = DICTATE_MIC (object);

  g_clear_pointer (&self->fade, vibeos_anim_free);
  g_clear_pointer (&self->fab, gtk_widget_unparent);
  g_clear_object (&self->engine);
  G_OBJECT_CLASS (dictate_mic_parent_class)->dispose (object);
}

static void
dictate_mic_class_init (DictateMicClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = dictate_mic_dispose;
  widget_class->snapshot = dictate_mic_snapshot;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "dictate-mic");
  signals[SIGNAL_CLICKED] = g_signal_new ("clicked", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                          NULL, G_TYPE_NONE, 0);
}

static void
dictate_mic_init (DictateMic *self)
{
  self->fab = vibeos_fab_new ("dictate-mic-symbolic", TRUE);
  /* Material 3's large FAB has a 36 px icon: the one thing on the screen to tap */
  gtk_image_set_pixel_size (GTK_IMAGE (vibeos_pressable_get_child (VIBEOS_PRESSABLE (self->fab))), 36);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->fab), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Start dictating"),
                                  -1);
  gtk_widget_set_parent (self->fab, GTK_WIDGET (self));
  g_signal_connect (self->fab, "clicked", G_CALLBACK (on_clicked), self);
  self->fade = vibeos_anim_new (GTK_WIDGET (self), on_fade, self);
}

GtkWidget *
dictate_mic_new (DictateEngine *engine)
{
  DictateMic *self = g_object_new (DICTATE_TYPE_MIC, NULL);

  self->engine = g_object_ref (engine);
  return GTK_WIDGET (self);
}
