/* VibeosSnackbar: see vibeos-snackbar.h. It slides in and out over 250 ms, fast-out-slow-in. */
#include "vibeos-snackbar.h"
#include "vibeos-anim.h"
#include "vibeos-pressable.h"

#define MARGIN   8               /* the default: a floating bar */
#define SLIDE_MS 250

struct _VibeosSnackbar {
  GtkWidget          parent;
  GtkWidget         *box, *text, *action;
  VibeosSnackbarAction  action_cb;
  gpointer           action_data;
  guint              timeout;
  double             shown;          /* 0 below the edge .. 1 in place */
  int                margin;         /* around the bar: MARGIN, or 0 for an edge-to-edge one */
  VibeosAnim           *slide;
};

G_DEFINE_FINAL_TYPE (VibeosSnackbar, vibeos_snackbar, GTK_TYPE_WIDGET)

enum { PROP_0, PROP_LIFT, N_PROPS };
static GParamSpec *props[N_PROPS];

double
vibeos_snackbar_get_lift (VibeosSnackbar *self)
{
  return (gtk_widget_get_height (self->box) + self->margin) * self->shown;
}

static void
on_slide (VibeosAnim *anim, double value, gpointer data)
{
  VibeosSnackbar *self = data;
  self->shown = value;
  if (value == 0 && !vibeos_anim_is_running (anim) && vibeos_anim_get_to (anim) == 0)
    gtk_widget_set_visible (GTK_WIDGET (self), FALSE);
  gtk_widget_queue_allocate (GTK_WIDGET (self));
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_LIFT]);
}

void
vibeos_snackbar_dismiss (VibeosSnackbar *self)
{
  g_clear_handle_id (&self->timeout, g_source_remove);
  if (gtk_widget_get_visible (GTK_WIDGET (self)) && vibeos_anim_get_to (self->slide) != 0)
    vibeos_anim_start (self->slide, self->shown, 0, SLIDE_MS, 0, SPONGY_FAST_OUT_SLOW_IN);
}

static gboolean
on_timeout (gpointer data)
{
  VibeosSnackbar *self = data;
  self->timeout = 0;
  vibeos_snackbar_dismiss (self);
  return G_SOURCE_REMOVE;
}

static void
on_action (VibeosPressable *button, VibeosSnackbar *self)
{
  VibeosSnackbarAction cb = self->action_cb;
  self->action_cb = NULL;
  vibeos_snackbar_dismiss (self);
  if (cb)
    cb (self->action_data);
}

void
vibeos_snackbar_show (VibeosSnackbar *self, const char *text, const char *action, VibeosSnackbarAction action_cb,
                   gpointer data, guint duration)
{
  gtk_label_set_text (GTK_LABEL (self->text), text);
  gtk_widget_set_visible (self->action, action != NULL);
  if (action)
    gtk_label_set_text (GTK_LABEL (vibeos_pressable_get_child (VIBEOS_PRESSABLE (self->action))), action);
  self->action_cb = action_cb;
  self->action_data = data;

  g_clear_handle_id (&self->timeout, g_source_remove);
  self->timeout = g_timeout_add (duration, on_timeout, self);
  gtk_widget_set_visible (GTK_WIDGET (self), TRUE);
  /* a new message while one shows takes its place; otherwise it slides in */
  if (self->shown < 1 || vibeos_anim_get_to (self->slide) != 1)
    vibeos_anim_start (self->slide, self->shown, 1, SLIDE_MS, 0, SPONGY_FAST_OUT_SLOW_IN);
}

/* ---- layout: the bar sits its margin above the bottom, pushed down by (1 - shown) of its height and margin ----- */

static void
vibeos_snackbar_measure (GtkWidget *widget, GtkOrientation orientation, int for_size,
                      int *minimum, int *natural, int *minimum_baseline, int *natural_baseline)
{
  VibeosSnackbar *self = VIBEOS_SNACKBAR (widget);
  gtk_widget_measure (self->box, orientation, for_size < 0 ? -1 : MAX (0, for_size - 2 * self->margin),
                      minimum, natural, NULL, NULL);
  *minimum += 2 * self->margin;
  *natural += 2 * self->margin;
}

static void
vibeos_snackbar_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  VibeosSnackbar *self = VIBEOS_SNACKBAR (widget);
  int h, unused;
  int margin = self->margin;
  gtk_widget_measure (self->box, GTK_ORIENTATION_VERTICAL, width - 2 * margin, &h, &unused, NULL, NULL);
  double y = height - margin - h + (h + margin) * (1 - self->shown);
  gtk_widget_allocate (self->box, width - 2 * margin, h, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (margin, y)));
}

static gboolean
vibeos_snackbar_contains (GtkWidget *widget, double x, double y)
{
  /* only the bar itself takes touches: the margins and the space above it let them through */
  VibeosSnackbar *self = VIBEOS_SNACKBAR (widget);
  graphene_rect_t bounds;
  return gtk_widget_compute_bounds (self->box, widget, &bounds) &&
         graphene_rect_contains_point (&bounds, &GRAPHENE_POINT_INIT (x, y));
}

/* ---- GObject --------------------------------------------------------------------------------------------------- */

static void
vibeos_snackbar_get_property (GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  if (id == PROP_LIFT)
    g_value_set_double (value, vibeos_snackbar_get_lift (VIBEOS_SNACKBAR (object)));
}

static void
vibeos_snackbar_dispose (GObject *object)
{
  VibeosSnackbar *self = VIBEOS_SNACKBAR (object);
  g_clear_handle_id (&self->timeout, g_source_remove);
  g_clear_pointer (&self->slide, vibeos_anim_free);
  g_clear_pointer (&self->box, gtk_widget_unparent);
  G_OBJECT_CLASS (vibeos_snackbar_parent_class)->dispose (object);
}

static void
vibeos_snackbar_class_init (VibeosSnackbarClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  object_class->get_property = vibeos_snackbar_get_property;
  object_class->dispose = vibeos_snackbar_dispose;
  widget_class->measure = vibeos_snackbar_measure;
  widget_class->size_allocate = vibeos_snackbar_size_allocate;
  widget_class->contains = vibeos_snackbar_contains;
  gtk_widget_class_set_css_name (widget_class, "vibeos-snackbar-host");

  props[PROP_LIFT] = g_param_spec_double ("lift", NULL, NULL, 0, G_MAXDOUBLE, 0,
                                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
vibeos_snackbar_init (VibeosSnackbar *self)
{
  self->margin = MARGIN;
  self->box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (self->box, "vibeos-snackbar");
  gtk_widget_set_parent (self->box, GTK_WIDGET (self));

  self->text = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (self->text), TRUE);
  gtk_label_set_xalign (GTK_LABEL (self->text), 0);
  gtk_widget_set_hexpand (self->text, TRUE);
  gtk_widget_set_valign (self->text, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (self->text, "vibeos-snackbar-text");
  gtk_box_append (GTK_BOX (self->box), self->text);

  self->action = vibeos_text_button_new ("");
  gtk_widget_set_valign (self->action, GTK_ALIGN_CENTER);
  g_signal_connect (self->action, "clicked", G_CALLBACK (on_action), self);
  gtk_box_append (GTK_BOX (self->box), self->action);

  self->slide = vibeos_anim_new (GTK_WIDGET (self), on_slide, self);
  gtk_widget_set_valign (GTK_WIDGET (self), GTK_ALIGN_END);
  gtk_widget_set_visible (GTK_WIDGET (self), FALSE);
  gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);
}

GtkWidget *
vibeos_snackbar_new (void)
{
  return g_object_new (VIBEOS_TYPE_SNACKBAR, NULL);
}

void
vibeos_snackbar_set_margin (VibeosSnackbar *self, int margin)
{
  self->margin = margin;
  gtk_widget_queue_resize (GTK_WIDGET (self));
}
