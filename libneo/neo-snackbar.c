/* NeoSnackbar: see neo-snackbar.h. Style from DeskClock's Widget.DeskClock.Snackbar (spec/clock.md §12.4); the
 * motion is MDC's default slide: in and out over 250 ms, fast-out-slow-in (BaseTransientBottomBar). */
#include "neo-snackbar.h"
#include "neo-anim.h"
#include "neo-pressable.h"

#define MARGIN   8
#define SLIDE_MS 250

struct _NeoSnackbar {
  GtkWidget          parent;
  GtkWidget         *box, *text, *action;
  NeoSnackbarAction  action_cb;
  gpointer           action_data;
  guint              timeout;
  double             shown;          /* 0 below the edge .. 1 in place */
  NeoAnim           *slide;
};

G_DEFINE_FINAL_TYPE (NeoSnackbar, neo_snackbar, GTK_TYPE_WIDGET)

enum { PROP_0, PROP_LIFT, N_PROPS };
static GParamSpec *props[N_PROPS];

double
neo_snackbar_get_lift (NeoSnackbar *self)
{
  return (gtk_widget_get_height (self->box) + MARGIN) * self->shown;
}

static void
on_slide (NeoAnim *anim, double value, gpointer data)
{
  NeoSnackbar *self = data;
  self->shown = value;
  if (value == 0 && !neo_anim_is_running (anim) && neo_anim_get_to (anim) == 0)
    gtk_widget_set_visible (GTK_WIDGET (self), FALSE);
  gtk_widget_queue_allocate (GTK_WIDGET (self));
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_LIFT]);
}

void
neo_snackbar_dismiss (NeoSnackbar *self)
{
  g_clear_handle_id (&self->timeout, g_source_remove);
  if (gtk_widget_get_visible (GTK_WIDGET (self)) && neo_anim_get_to (self->slide) != 0)
    neo_anim_start (self->slide, self->shown, 0, SLIDE_MS, 0, SPONGY_FAST_OUT_SLOW_IN);
}

static gboolean
on_timeout (gpointer data)
{
  NeoSnackbar *self = data;
  self->timeout = 0;
  neo_snackbar_dismiss (self);
  return G_SOURCE_REMOVE;
}

static void
on_action (NeoPressable *button, NeoSnackbar *self)
{
  NeoSnackbarAction cb = self->action_cb;
  self->action_cb = NULL;
  neo_snackbar_dismiss (self);
  if (cb)
    cb (self->action_data);
}

void
neo_snackbar_show (NeoSnackbar *self, const char *text, const char *action, NeoSnackbarAction action_cb,
                   gpointer data, guint duration)
{
  gtk_label_set_text (GTK_LABEL (self->text), text);
  gtk_widget_set_visible (self->action, action != NULL);
  if (action)
    gtk_label_set_text (GTK_LABEL (neo_pressable_get_child (NEO_PRESSABLE (self->action))), action);
  self->action_cb = action_cb;
  self->action_data = data;

  g_clear_handle_id (&self->timeout, g_source_remove);
  self->timeout = g_timeout_add (duration, on_timeout, self);
  gtk_widget_set_visible (GTK_WIDGET (self), TRUE);
  /* a new message while one shows takes its place; otherwise it slides in */
  if (self->shown < 1 || neo_anim_get_to (self->slide) != 1)
    neo_anim_start (self->slide, self->shown, 1, SLIDE_MS, 0, SPONGY_FAST_OUT_SLOW_IN);
}

/* ---- layout: the bar sits MARGIN above the bottom, pushed down by (1 - shown) of its height and margin ---------- */

static void
neo_snackbar_measure (GtkWidget *widget, GtkOrientation orientation, int for_size,
                      int *minimum, int *natural, int *minimum_baseline, int *natural_baseline)
{
  NeoSnackbar *self = NEO_SNACKBAR (widget);
  gtk_widget_measure (self->box, orientation, for_size < 0 ? -1 : MAX (0, for_size - 2 * MARGIN),
                      minimum, natural, NULL, NULL);
  *minimum += 2 * MARGIN;
  *natural += 2 * MARGIN;
}

static void
neo_snackbar_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  NeoSnackbar *self = NEO_SNACKBAR (widget);
  int h, unused;
  gtk_widget_measure (self->box, GTK_ORIENTATION_VERTICAL, width - 2 * MARGIN, &h, &unused, NULL, NULL);
  double y = height - MARGIN - h + (h + MARGIN) * (1 - self->shown);
  gtk_widget_allocate (self->box, width - 2 * MARGIN, h, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (MARGIN, y)));
}

static gboolean
neo_snackbar_contains (GtkWidget *widget, double x, double y)
{
  /* only the bar itself takes touches: the margins and the space above it let them through */
  NeoSnackbar *self = NEO_SNACKBAR (widget);
  graphene_rect_t bounds;
  return gtk_widget_compute_bounds (self->box, widget, &bounds) &&
         graphene_rect_contains_point (&bounds, &GRAPHENE_POINT_INIT (x, y));
}

/* ---- GObject --------------------------------------------------------------------------------------------------- */

static void
neo_snackbar_get_property (GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  if (id == PROP_LIFT)
    g_value_set_double (value, neo_snackbar_get_lift (NEO_SNACKBAR (object)));
}

static void
neo_snackbar_dispose (GObject *object)
{
  NeoSnackbar *self = NEO_SNACKBAR (object);
  g_clear_handle_id (&self->timeout, g_source_remove);
  g_clear_pointer (&self->slide, neo_anim_free);
  g_clear_pointer (&self->box, gtk_widget_unparent);
  G_OBJECT_CLASS (neo_snackbar_parent_class)->dispose (object);
}

static void
neo_snackbar_class_init (NeoSnackbarClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  object_class->get_property = neo_snackbar_get_property;
  object_class->dispose = neo_snackbar_dispose;
  widget_class->measure = neo_snackbar_measure;
  widget_class->size_allocate = neo_snackbar_size_allocate;
  widget_class->contains = neo_snackbar_contains;
  gtk_widget_class_set_css_name (widget_class, "neo-snackbar-host");

  props[PROP_LIFT] = g_param_spec_double ("lift", NULL, NULL, 0, G_MAXDOUBLE, 0,
                                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
neo_snackbar_init (NeoSnackbar *self)
{
  self->box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (self->box, "neo-snackbar");
  gtk_widget_set_parent (self->box, GTK_WIDGET (self));

  self->text = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (self->text), TRUE);
  gtk_label_set_xalign (GTK_LABEL (self->text), 0);
  gtk_widget_set_hexpand (self->text, TRUE);
  gtk_widget_set_valign (self->text, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (self->text, "neo-snackbar-text");
  gtk_box_append (GTK_BOX (self->box), self->text);

  self->action = neo_text_button_new ("");
  gtk_widget_set_valign (self->action, GTK_ALIGN_CENTER);
  g_signal_connect (self->action, "clicked", G_CALLBACK (on_action), self);
  gtk_box_append (GTK_BOX (self->box), self->action);

  self->slide = neo_anim_new (GTK_WIDGET (self), on_slide, self);
  gtk_widget_set_valign (GTK_WIDGET (self), GTK_ALIGN_END);
  gtk_widget_set_visible (GTK_WIDGET (self), FALSE);
  gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);
}

GtkWidget *
neo_snackbar_new (void)
{
  return g_object_new (NEO_TYPE_SNACKBAR, NULL);
}
