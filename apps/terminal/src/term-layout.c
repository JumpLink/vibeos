/* TermLayout: see term-layout.h */
#include "term-layout.h"

#include <vibeos.h>

/* the keyboard's slide (keyboard.js SHOW_MS and HIDE_MS) and Clutter's curves for it, as cubic béziers */
#define RISE_MS  200
#define FALL_MS  190
#define EASE_OUT_CUBIC 0.215f, 0.61f, 0.355f, 1.0f
#define EASE_IN_QUAD   0.55f, 0.085f, 0.68f, 0.53f
/* the text cursor's line stops this far above the extra keys */
#define CURSOR_MARGIN 4

struct _TermLayout {
  GtkWidget     parent;
  GtkWidget    *content;
  GtkWidget    *keys;
  TermView     *view;
  GDBusProxy   *shell;
  GCancellable *cancellable;
  double        overlap;      /* how far up from the layout's bottom the keyboard's top edge goes, the shell says */
  double        reach;        /* the last overlap that was not 0: where the extra keys stand when the keyboard is up */
  VibeosAnim   *rise;         /* the keyboard's top edge above the bottom, now */
  VibeosAnim   *shift;        /* how far up the content is, now */
  guint         update;       /* a tick callback: the cursor's place to be looked at again */
  int           width, height;
};

G_DEFINE_FINAL_TYPE (TermLayout, term_layout, GTK_TYPE_WIDGET)

static int
keys_height (TermLayout *self)
{
  int height = 0;

  gtk_widget_measure (self->keys, GTK_ORIENTATION_VERTICAL, gtk_widget_get_width (GTK_WIDGET (self)), NULL, &height,
                      NULL, NULL);
  return height;
}

/* how far the content goes up for the keyboard at `overlap`: the cursor's line above the extra keys, the terminal's
 * last row no higher than their top */
static double
shift_for (TermLayout *self, double overlap)
{
  double height = gtk_widget_get_height (GTK_WIDGET (self));
  double most, bottom;
  graphene_point_t p;

  if (overlap <= 0)
    return 0;
  most = overlap + keys_height (self);
  /* the cursor's foot, in the content's own coordinates: where it is with the content down */
  if (!term_view_get_cursor_bottom (self->view, &bottom)
      || !gtk_widget_compute_point (GTK_WIDGET (self->view), self->content, &GRAPHENE_POINT_INIT (0, bottom), &p))
    return most;
  return CLAMP (p.y + CURSOR_MARGIN - (height - most), 0, most);
}

static void
slide (TermLayout *self, double to, guint ms, gboolean up)
{
  if (vibeos_anim_is_running (self->shift) ? fabs (vibeos_anim_get_to (self->shift) - to) < 0.5
                                           : fabs (vibeos_anim_get_value (self->shift) - to) < 0.5)
    return;
  if (up)
    vibeos_anim_start_cubic (self->shift, vibeos_anim_get_value (self->shift), to, ms, 0, EASE_OUT_CUBIC);
  else
    vibeos_anim_start_cubic (self->shift, vibeos_anim_get_value (self->shift), to, ms, 0, EASE_IN_QUAD);
}

static gboolean
on_update (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  TermLayout *self = data;

  self->update = 0;
  if (self->overlap > 0)
    slide (self, shift_for (self, self->overlap), RISE_MS, TRUE);
  return G_SOURCE_REMOVE;
}

/* the cursor moved, the text changed or scrolled: its place again, at the next frame */
static void
queue_update (TermLayout *self)
{
  if (self->overlap > 0 && !self->update)
    self->update = gtk_widget_add_tick_callback (GTK_WIDGET (self), on_update, self, NULL);
}

static void
set_overlap (TermLayout *self, guint overlap)
{
  GtkNative *native = gtk_widget_get_native (GTK_WIDGET (self));
  double below = 0, here;
  graphene_point_t p;

  /* the shell's figure is from the window's bottom */
  if (native && gtk_widget_compute_point (GTK_WIDGET (self), GTK_WIDGET (native),
                                          &GRAPHENE_POINT_INIT (0, gtk_widget_get_height (GTK_WIDGET (self))), &p))
    below = MAX (0, gtk_widget_get_height (GTK_WIDGET (native)) - p.y);
  here = MAX (0, overlap - below);
  if (fabs (here - self->overlap) < 0.5)
    return;
  self->overlap = here;
  if (here > 0) {
    self->reach = here;
    vibeos_anim_start_cubic (self->rise, vibeos_anim_get_value (self->rise), here, RISE_MS, 0, EASE_OUT_CUBIC);
    slide (self, shift_for (self, here), RISE_MS, TRUE);
  } else {
    vibeos_anim_start_cubic (self->rise, vibeos_anim_get_value (self->rise), 0, FALL_MS, 0, EASE_IN_QUAD);
    slide (self, 0, FALL_MS, FALSE);
  }
}

static void
on_shell_properties (GDBusProxy *shell, GVariant *changed, GStrv invalidated, TermLayout *self)
{
  g_autoptr (GVariant) v = g_dbus_proxy_get_cached_property (shell, "KeyboardOverlap");

  set_overlap (self, v && g_variant_is_of_type (v, G_VARIANT_TYPE_UINT32) ? g_variant_get_uint32 (v) : 0);
}

static void
on_shell_ready (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr (GError) error = NULL;
  GDBusProxy *shell = g_dbus_proxy_new_for_bus_finish (result, &error);
  TermLayout *self;

  if (!shell)
    return;     /* cancelled (the window went), or no session bus */
  self = data;
  self->shell = shell;
  g_signal_connect (shell, "g-properties-changed", G_CALLBACK (on_shell_properties), self);
  on_shell_properties (shell, NULL, NULL, self);
}

static void
on_anim (VibeosAnim *anim, double value, gpointer data)
{
  gtk_widget_queue_allocate (GTK_WIDGET (data));
}

static void
on_view_changed (TermLayout *self)
{
  queue_update (self);
}

static void
term_layout_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                     int *minimum_baseline, int *natural_baseline)
{
  TermLayout *self = TERM_LAYOUT (widget);

  gtk_widget_measure (self->content, orientation, for_size, minimum, natural, minimum_baseline, natural_baseline);
}

static void
term_layout_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  TermLayout *self = TERM_LAYOUT (widget);
  double rise = vibeos_anim_get_value (self->rise);
  double shift = vibeos_anim_get_value (self->shift);
  int keys_h = 0;
  double keys_y = height;

  gtk_widget_allocate (self->content, width, height, baseline,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (0, (float) -round (shift))));
  /* the extra keys ride on the keyboard as the top of it: below the window's bottom with the keyboard down, on its
   * top edge with it up, coming up from behind it in between */
  gtk_widget_measure (self->keys, GTK_ORIENTATION_VERTICAL, width, NULL, &keys_h, NULL, NULL);
  if (rise > 0 && self->reach > 0)
    keys_y = height - rise - keys_h * MIN (1, rise / self->reach);
  gtk_widget_allocate (self->keys, width, keys_h, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (0, (float) round (keys_y))));
  if (width != self->width || height != self->height) {
    self->width = width;
    self->height = height;
    queue_update (self);
  }
}

static void
term_layout_dispose (GObject *object)
{
  TermLayout *self = TERM_LAYOUT (object);

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  if (self->shell)
    g_signal_handlers_disconnect_by_data (self->shell, self);
  g_clear_object (&self->shell);
  if (self->update)
    gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->update);
  self->update = 0;
  g_clear_pointer (&self->rise, vibeos_anim_free);
  g_clear_pointer (&self->shift, vibeos_anim_free);
  g_clear_pointer (&self->content, gtk_widget_unparent);
  g_clear_pointer (&self->keys, gtk_widget_unparent);
  G_OBJECT_CLASS (term_layout_parent_class)->dispose (object);
}

static void
term_layout_class_init (TermLayoutClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = term_layout_dispose;
  widget_class->measure = term_layout_measure;
  widget_class->size_allocate = term_layout_size_allocate;
  gtk_widget_class_set_css_name (widget_class, "term-layout");
}

static void
term_layout_init (TermLayout *self)
{
  /* what slides out of the layout, the content's top or the extra keys under the bottom, is not drawn or tapped */
  gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);
  self->rise = vibeos_anim_new (GTK_WIDGET (self), on_anim, self);
  self->shift = vibeos_anim_new (GTK_WIDGET (self), on_anim, self);
  self->cancellable = g_cancellable_new ();
  g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, NULL, "vibeos.Shell",
                            "/vibeos/Shell", "vibeos.Shell", self->cancellable, on_shell_ready, self);
}

GtkWidget *
term_layout_new (GtkWidget *content, TermView *view, GtkWidget *keys)
{
  TermLayout *self = g_object_new (TERM_TYPE_LAYOUT, NULL);

  self->content = content;
  self->keys = keys;
  self->view = view;
  /* the keys over the content: drawn after it, tapped first */
  gtk_widget_set_parent (content, GTK_WIDGET (self));
  gtk_widget_set_parent (keys, GTK_WIDGET (self));
  g_signal_connect_object (view, "cursor-moved", G_CALLBACK (on_view_changed), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (view, "contents-changed", G_CALLBACK (on_view_changed), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (view)), "value-changed",
                           G_CALLBACK (on_view_changed), self, G_CONNECT_SWAPPED);
  return GTK_WIDGET (self);
}
