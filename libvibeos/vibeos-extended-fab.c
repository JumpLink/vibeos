/* VibeosExtendedFab: see vibeos-extended-fab.h.
 *
 * The button is a VibeosPressable (the fill, the shadow, the ripple) around a content widget that lays out the icon
 * and the label itself: its width is the collapsed square's plus a share of the rest, so it can be narrower than the
 * label, which it then clips. */
#include "vibeos-extended-fab.h"
#include "vibeos-anim.h"
#include "vibeos-pressable.h"
#include "vibeos-style.h"
#include "vibeos-text.h"

#define HEIGHT        56
#define CORNER        16
#define ICON          24
#define ICON_X        16     /* the icon's inset, which centres it on the collapsed square */
#define LABEL_GAP     12     /* from the icon to the label */
#define END_PAD       20     /* after the label */
#define ANIM_MS       250

/* ---- the content: the icon and the label, as wide as the button is extended ------------------------------------ */

#define VIBEOS_TYPE_EXTENDED_FAB_CONTENT (vibeos_extended_fab_content_get_type ())
G_DECLARE_FINAL_TYPE (VibeosExtendedFabContent, vibeos_extended_fab_content, VIBEOS, EXTENDED_FAB_CONTENT,
                      GtkWidget)

struct _VibeosExtendedFabContent {
  GtkWidget  parent;
  GtkWidget *icon, *label;
  double     t;                 /* 0 collapsed .. 1 extended */
};

G_DEFINE_FINAL_TYPE (VibeosExtendedFabContent, vibeos_extended_fab_content, GTK_TYPE_WIDGET)

static int
full_width (VibeosExtendedFabContent *self)
{
  int min, nat;

  gtk_widget_measure (self->label, GTK_ORIENTATION_HORIZONTAL, -1, &min, &nat, NULL, NULL);
  return ICON_X + ICON + LABEL_GAP + nat + END_PAD;
}

static void
content_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                 int *min_baseline, int *nat_baseline)
{
  VibeosExtendedFabContent *self = VIBEOS_EXTENDED_FAB_CONTENT (widget);

  if (orientation == GTK_ORIENTATION_VERTICAL)
    *min = *nat = HEIGHT;
  else
    *min = *nat = HEIGHT + (int) (self->t * (full_width (self) - HEIGHT) + 0.5);
}

static void
content_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  VibeosExtendedFabContent *self = VIBEOS_EXTENDED_FAB_CONTENT (widget);
  int min, nat, h;

  gtk_widget_allocate (self->icon, ICON, ICON, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (ICON_X, (height - ICON) / 2.0)));
  gtk_widget_measure (self->label, GTK_ORIENTATION_HORIZONTAL, -1, &min, &nat, NULL, NULL);
  gtk_widget_measure (self->label, GTK_ORIENTATION_VERTICAL, nat, NULL, &h, NULL, NULL);
  gtk_widget_allocate (self->label, nat, h, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (ICON_X + ICON + LABEL_GAP,
                                                                            (height - h) / 2.0)));
}

static void
content_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  VibeosExtendedFabContent *self = VIBEOS_EXTENDED_FAB_CONTENT (widget);
  double width = gtk_widget_get_width (widget);

  gtk_widget_snapshot_child (widget, self->icon, snapshot);
  /* the label fades as the card narrows over it, and is cut at the card's end padding */
  gtk_snapshot_push_clip (snapshot, &GRAPHENE_RECT_INIT (0, 0, MAX (width - END_PAD * self->t, 0),
                                                         gtk_widget_get_height (widget)));
  gtk_snapshot_push_opacity (snapshot, CLAMP ((self->t - 0.3) / 0.7, 0, 1));
  gtk_widget_snapshot_child (widget, self->label, snapshot);
  gtk_snapshot_pop (snapshot);
  gtk_snapshot_pop (snapshot);
}

static void
content_dispose (GObject *object)
{
  VibeosExtendedFabContent *self = VIBEOS_EXTENDED_FAB_CONTENT (object);

  g_clear_pointer (&self->icon, gtk_widget_unparent);
  g_clear_pointer (&self->label, gtk_widget_unparent);
  G_OBJECT_CLASS (vibeos_extended_fab_content_parent_class)->dispose (object);
}

static void
vibeos_extended_fab_content_class_init (VibeosExtendedFabContentClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = content_dispose;
  widget_class->measure = content_measure;
  widget_class->size_allocate = content_allocate;
  widget_class->snapshot = content_snapshot;
  gtk_widget_class_set_css_name (widget_class, "vibeos-extended-fab-content");
}

static void
vibeos_extended_fab_content_init (VibeosExtendedFabContent *self)
{
  self->t = 1;
}

/* ---- the button ------------------------------------------------------------------------------------------------ */

struct _VibeosExtendedFab {
  GtkWidget                 parent;
  GtkWidget                *button;
  VibeosExtendedFabContent *content;
  VibeosAnim               *anim;
  gboolean                  extended;
};

G_DEFINE_FINAL_TYPE (VibeosExtendedFab, vibeos_extended_fab, GTK_TYPE_WIDGET)

enum { CLICKED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
load_style (void)
{
  static gboolean loaded;

  if (loaded)
    return;
  loaded = TRUE;
  vibeos_css_load_string (".vibeos-extended-fab-label { font-size: 16sp; font-weight: 500;"
                          " color: @vibeos_on_primary_container; }\n"
                          ".vibeos-extended-fab-icon { color: @vibeos_on_primary_container; }\n",
                          GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

/* the fill in the theme's colour of now */
static void
update_fill (VibeosExtendedFab *self)
{
  GdkRGBA fill = vibeos_color (VIBEOS_PRIMARY_CONTAINER);

  vibeos_pressable_set_fill (VIBEOS_PRESSABLE (self->button), &fill, TRUE);
}

static void
on_theme_changed (VibeosTheme *theme, VibeosExtendedFab *self)
{
  update_fill (self);
}

static void
on_anim (VibeosAnim *anim, double value, gpointer data)
{
  VibeosExtendedFab *self = data;

  self->content->t = value;
  gtk_widget_queue_resize (GTK_WIDGET (self->content));
}

static void
on_clicked (VibeosPressable *button, VibeosExtendedFab *self)
{
  g_signal_emit (self, signals[CLICKED], 0);
}

void
vibeos_extended_fab_set_extended (VibeosExtendedFab *self, gboolean extended)
{
  g_return_if_fail (VIBEOS_IS_EXTENDED_FAB (self));

  if (!self->extended == !extended)
    return;
  self->extended = extended;
  vibeos_anim_start (self->anim, self->content->t, extended ? 1 : 0, ANIM_MS, 0, SPONGY_FAST_OUT_SLOW_IN);
}

gboolean
vibeos_extended_fab_get_extended (VibeosExtendedFab *self)
{
  g_return_val_if_fail (VIBEOS_IS_EXTENDED_FAB (self), TRUE);
  return self->extended;
}

static void
vibeos_extended_fab_dispose (GObject *object)
{
  VibeosExtendedFab *self = VIBEOS_EXTENDED_FAB (object);

  g_signal_handlers_disconnect_by_data (vibeos_theme_get_default (), self);
  g_clear_pointer (&self->anim, vibeos_anim_free);
  g_clear_pointer (&self->button, gtk_widget_unparent);
  G_OBJECT_CLASS (vibeos_extended_fab_parent_class)->dispose (object);
}

static void
vibeos_extended_fab_class_init (VibeosExtendedFabClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = vibeos_extended_fab_dispose;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "vibeos-extended-fab");
  signals[CLICKED] = g_signal_new ("clicked", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                   G_TYPE_NONE, 0);
}

static void
vibeos_extended_fab_init (VibeosExtendedFab *self)
{
  self->extended = TRUE;
}

GtkWidget *
vibeos_extended_fab_new (const char *icon_name, const char *label)
{
  VibeosExtendedFab *self = g_object_new (VIBEOS_TYPE_EXTENDED_FAB, NULL);
  VibeosExtendedFabContent *content = g_object_new (VIBEOS_TYPE_EXTENDED_FAB_CONTENT, NULL);

  load_style ();
  content->icon = gtk_image_new_from_icon_name (icon_name);
  gtk_image_set_pixel_size (GTK_IMAGE (content->icon), ICON);
  gtk_widget_add_css_class (content->icon, "vibeos-extended-fab-icon");
  gtk_widget_set_parent (content->icon, GTK_WIDGET (content));
  content->label = gtk_label_new (label);
  gtk_label_set_single_line_mode (GTK_LABEL (content->label), TRUE);
  gtk_widget_add_css_class (content->label, "vibeos-extended-fab-label");
  gtk_widget_set_parent (content->label, GTK_WIDGET (content));
  self->content = content;

  self->button = vibeos_pressable_new (GTK_WIDGET (content));
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (self->button), VIBEOS_RIPPLE_BOUNDED, CORNER);
  update_fill (self);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->button), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  g_signal_connect (self->button, "clicked", G_CALLBACK (on_clicked), self);
  gtk_widget_set_parent (self->button, GTK_WIDGET (self));
  gtk_widget_set_halign (GTK_WIDGET (self), GTK_ALIGN_END);
  gtk_widget_set_valign (GTK_WIDGET (self), GTK_ALIGN_END);

  self->anim = vibeos_anim_new (GTK_WIDGET (content), on_anim, self);
  g_signal_connect (vibeos_theme_get_default (), "changed", G_CALLBACK (on_theme_changed), self);
  return GTK_WIDGET (self);
}
