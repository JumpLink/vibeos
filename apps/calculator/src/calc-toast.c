/* CalcToast: see calc-toast.h. The motion is SystemUI's ToastDefaultAnimation: in, the toast scales up from 0.9
 * (333 ms, (0, 0, 0, 1)) while its background fades in over 66 ms and its icon and text over 283 ms after 50;
 * out, it scales back to 0.9 (250 ms, (0.3, 0, 1, 1)), the text and icon fading over 166 ms and the background
 * over the last 100. */
#include "calc-toast.h"
#include "calc-text.h"

#include <math.h>
#include <neo.h>

#define TEXT_SIZE     14
#define MIN_HEIGHT    48
#define PADDING       16
#define ICON          24
#define ICON_GAP      10
#define CORNER        28
#define Y_OFFSET      48      /* toast_y_offset, from the bottom */
#define MARGIN        16
#define SHORT_MS      2000    /* LENGTH_SHORT */
#define IN_MS         333
#define OUT_MS        250
#define APP_ICON      "de.yesman.Calculator"

#define CALC_TYPE_TOAST (calc_toast_get_type ())
G_DECLARE_FINAL_TYPE (CalcToast, calc_toast, CALC, TOAST, GtkWidget)

struct _CalcToast {
  GtkWidget     parent;
  PangoLayout  *layout;
  char         *text;
  GdkPaintable *icon;
  gint64        start_us;     /* when it was shown, frame time */
  guint         tick;
  SpongyInterp  in_curve, out_curve;
  double        scale, background, content;
};

G_DEFINE_FINAL_TYPE (CalcToast, calc_toast, GTK_TYPE_WIDGET)

static void
calc_toast_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                    int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 0;
}

static void
calc_toast_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CalcToast *self = CALC_TOAST (widget);
  double w = gtk_widget_get_width (widget), h = gtk_widget_get_height (widget);
  double icon_w = self->icon ? ICON + ICON_GAP : 0;
  double text_w, bar_w, x, y;
  GdkRGBA background = neo_color (NEO_SURFACE_CONTAINER_HIGH), text = neo_color (NEO_ON_SURFACE);
  GskRoundedRect shape;

  if (!self->layout) {
    self->layout = calc_text_layout (widget, self->text, TEXT_SIZE, CALC_WEIGHT_REGULAR);
    pango_layout_set_ellipsize (self->layout, PANGO_ELLIPSIZE_END);
  }
  pango_layout_set_width (self->layout, (int) (MAX (0, w - 2 * MARGIN - 2 * PADDING - icon_w) * PANGO_SCALE));
  text_w = ceil (calc_text_width (self->layout));
  bar_w = text_w + icon_w + 2 * PADDING;
  x = (w - bar_w) / 2;
  y = h - Y_OFFSET - MIN_HEIGHT;

  gtk_snapshot_save (snapshot);
  /* scaled about its centre */
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (x + bar_w / 2, y + MIN_HEIGHT / 2.0));
  gtk_snapshot_scale (snapshot, self->scale, self->scale);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (-bar_w / 2, -MIN_HEIGHT / 2.0));
  gsk_rounded_rect_init_from_rect (&shape, &GRAPHENE_RECT_INIT (0, 0, bar_w, MIN_HEIGHT), MIN (CORNER, MIN_HEIGHT / 2.0));
  gtk_snapshot_push_opacity (snapshot, self->background);
  gtk_snapshot_append_outset_shadow (snapshot, &shape, &(GdkRGBA) { 0, 0, 0, 0.3f }, 0, 1, 0, 3);
  gtk_snapshot_push_rounded_clip (snapshot, &shape);
  gtk_snapshot_append_color (snapshot, &background, &shape.bounds);
  gtk_snapshot_pop (snapshot);
  gtk_snapshot_pop (snapshot);

  gtk_snapshot_push_opacity (snapshot, self->content);
  if (self->icon) {
    gtk_snapshot_save (snapshot);
    gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (PADDING, (MIN_HEIGHT - ICON) / 2.0));
    gdk_paintable_snapshot (self->icon, snapshot, ICON, ICON);
    gtk_snapshot_restore (snapshot);
  }
  /* the line centred (gravity center_vertical): ascent over descent in the middle */
  calc_text_draw (snapshot, self->layout, PADDING + icon_w,
                  MIN_HEIGHT / 2.0 + (CALC_ASCENT - CALC_DESCENT) * TEXT_SIZE / 2, &text);
  gtk_snapshot_pop (snapshot);
  gtk_snapshot_restore (snapshot);
}

static void
remove_toast (CalcToast *self)
{
  GtkWidget *overlay = gtk_widget_get_parent (GTK_WIDGET (self));

  if (self->tick)
    gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->tick);
  self->tick = 0;
  if (overlay) {
    g_object_set_data (G_OBJECT (overlay), "calc-toast", NULL);
    gtk_overlay_remove_overlay (GTK_OVERLAY (overlay), GTK_WIDGET (self));
  }
}

static double
clamp01 (double v)
{
  return CLAMP (v, 0, 1);
}

static gboolean
on_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  CalcToast *self = CALC_TOAST (widget);
  gint64 now = gdk_frame_clock_get_frame_time (clock);
  double t;

  if (!self->start_us)
    self->start_us = now;
  t = (now - self->start_us) / 1000.0;
  if (t < SHORT_MS) {
    self->scale = 0.9 + 0.1 * spongy_interp_get (&self->in_curve, clamp01 (t / IN_MS));
    self->background = clamp01 (t / 66);
    self->content = clamp01 ((t - 50) / 283);
  } else {
    double u = t - SHORT_MS;

    if (u >= OUT_MS) {
      self->tick = 0;
      remove_toast (self);
      return G_SOURCE_REMOVE;
    }
    self->scale = 1 - 0.1 * spongy_interp_get (&self->out_curve, clamp01 (u / OUT_MS));
    self->background = 1 - clamp01 ((u - 150) / 100);
    self->content = 1 - clamp01 (u / 166);
  }
  gtk_widget_queue_draw (widget);
  return G_SOURCE_CONTINUE;
}

static void
calc_toast_dispose (GObject *object)
{
  CalcToast *self = CALC_TOAST (object);

  g_clear_object (&self->layout);
  g_clear_object (&self->icon);
  G_OBJECT_CLASS (calc_toast_parent_class)->dispose (object);
}

static void
calc_toast_finalize (GObject *object)
{
  g_free (CALC_TOAST (object)->text);
  G_OBJECT_CLASS (calc_toast_parent_class)->finalize (object);
}

static void
calc_toast_class_init (CalcToastClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = calc_toast_dispose;
  G_OBJECT_CLASS (klass)->finalize = calc_toast_finalize;
  widget_class->measure = calc_toast_measure;
  widget_class->snapshot = calc_toast_snapshot;
  gtk_widget_class_set_css_name (widget_class, "calc-toast");
}

static void
calc_toast_init (CalcToast *self)
{
  spongy_interp_cubic (&self->in_curve, 0, 0, 0, 1);
  spongy_interp_cubic (&self->out_curve, 0.3f, 0, 1, 1);
  self->scale = 0.9;
  /* a toast takes no touches: they go to the app under it */
  gtk_widget_set_can_target (GTK_WIDGET (self), FALSE);
}

void
calc_toast_show (GtkWidget *widget, const char *text)
{
  GtkOverlay *overlay = neo_window_get_overlay (widget);
  GtkIconTheme *theme;
  CalcToast *self, *old;

  g_return_if_fail (overlay != NULL);
  old = g_object_get_data (G_OBJECT (overlay), "calc-toast");
  if (old)
    remove_toast (old);
  self = g_object_new (CALC_TYPE_TOAST, NULL);
  self->text = g_strdup (text);
  /* the app's icon, as SystemUI shows it next to an app's text toast; none if the theme does not have it */
  theme = gtk_icon_theme_get_for_display (gtk_widget_get_display (widget));
  if (gtk_icon_theme_has_icon (theme, APP_ICON))
    self->icon = GDK_PAINTABLE (gtk_icon_theme_lookup_icon (theme, APP_ICON, NULL, ICON,
                                                            gtk_widget_get_scale_factor (widget),
                                                            GTK_TEXT_DIR_LTR, 0));
  g_object_set_data (G_OBJECT (overlay), "calc-toast", self);
  gtk_overlay_add_overlay (overlay, GTK_WIDGET (self));
  gtk_overlay_set_measure_overlay (overlay, GTK_WIDGET (self), FALSE);
  gtk_overlay_set_clip_overlay (overlay, GTK_WIDGET (self), TRUE);
  self->tick = gtk_widget_add_tick_callback (GTK_WIDGET (self), on_tick, NULL, NULL);
}
