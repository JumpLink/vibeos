/* VibeosTopBar: see vibeos-top-bar.h. */
#include "vibeos-top-bar.h"

#include <glib/gi18n-lib.h>
#include "vibeos-fit-label.h"
#include "vibeos-layout.h"
#include "vibeos-pressable.h"
#include "vibeos-style.h"

#include <math.h>

#define BAR_H          64
#define COMPACT_H      56
#define BACK           40     /* the back button's circle */
#define TABLET_BACK    44     /* and on a tablet: 56 filled the 64 bar and touched the card under it (owner, 11-21) */
#define BACK_CENTRE_X  44     /* the circle's centre, whatever its size */
#define TITLE_GAP      20     /* from the circle to the title */
#define TITLE_X        24     /* the title without a back button */
#define BUTTON         48     /* an icon button's touch target */
#define END_PAD        4

struct _VibeosTopBar {
  GtkWidget      parent;
  GtkWidget     *back, *close, *title, *actions, *selection_actions;
  GtkWidget     *large_title;     /* a VibeosFitLabel, put in the content by the app (a reference of the bar's own) */
  VibeosLayout  *layout;          /* the window's, while it is in one: a tablet's back button is larger */
  GtkAdjustment *scroll;
  GdkRGBA        fill;
  gboolean       has_back, compact, large, selecting;
};

G_DEFINE_FINAL_TYPE (VibeosTopBar, vibeos_top_bar, GTK_TYPE_WIDGET)

enum { BACK_SIGNAL, SELECTION_CLOSED, N_SIGNALS };
static guint signals[N_SIGNALS];

static int
bar_height (VibeosTopBar *self)
{
  return self->compact ? COMPACT_H : BAR_H;
}

static int
back_size (VibeosTopBar *self)
{
  return self->layout && vibeos_layout_is_tablet (self->layout) ? TABLET_BACK : BACK;
}

static GtkLabel *
large_label (VibeosTopBar *self)
{
  return GTK_LABEL (vibeos_fit_label_get_label (VIBEOS_FIT_LABEL (self->large_title)));
}

/* 0 expanded .. 1 collapsed: how much of the large title has scrolled under the bar. A large title the app has not
 * put anywhere is no title: the bar is a small one */
static double
collapsed (VibeosTopBar *self)
{
  int height = gtk_widget_get_height (self->large_title);

  if (!self->large || !gtk_widget_get_parent (self->large_title))
    return 1;
  if (!self->scroll || height <= 0)
    return 0;
  return CLAMP (gtk_adjustment_get_value (self->scroll) / height, 0, 1);
}

static double
title_x (VibeosTopBar *self)
{
  if (self->selecting)
    return BACK_CENTRE_X + BACK / 2.0 + TITLE_GAP;
  return self->has_back ? BACK_CENTRE_X + back_size (self) / 2.0 + TITLE_GAP : TITLE_X;
}

/* ---- layout and drawing ---------------------------------------------------------------------------------------- */

static void
natural (GtkWidget *widget, int *width, int *height)
{
  int unused;

  gtk_widget_measure (widget, GTK_ORIENTATION_HORIZONTAL, -1, &unused, width, NULL, NULL);
  gtk_widget_measure (widget, GTK_ORIENTATION_VERTICAL, *width, &unused, height, NULL, NULL);
}

static void
put (GtkWidget *child, double x, double y, int width, int height)
{
  gtk_widget_allocate (child, width, height, -1, gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (x, y)));
}

static void
vibeos_top_bar_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                        int *min_baseline, int *nat_baseline)
{
  VibeosTopBar *self = VIBEOS_TOP_BAR (widget);

  if (orientation == GTK_ORIENTATION_VERTICAL)
    *min = *nat = bar_height (self);
  else
    *min = *nat = (int) title_x (self) + BUTTON;
}

static void
vibeos_top_bar_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  VibeosTopBar *self = VIBEOS_TOP_BAR (widget);
  int bar = bar_height (self), d = back_size (self), w, h;
  GtkWidget *actions = self->selecting ? self->selection_actions : self->actions;
  double x = title_x (self), end;

  put (self->back, BACK_CENTRE_X - d / 2.0, (bar - d) / 2.0, d, d);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (self->back), VIBEOS_RIPPLE_CIRCLE, d / 2.0);
  put (self->close, BACK_CENTRE_X - BUTTON / 2.0, (bar - BUTTON) / 2.0, BUTTON, BUTTON);

  natural (actions, &w, &h);
  end = width - END_PAD - w;
  put (actions, end, (bar - h) / 2.0, w, h);
  /* the other set of actions, hidden, keeps a place */
  put (self->selecting ? self->actions : self->selection_actions, end, 0, 0, 0);

  w = MAX ((int) (end - x - 8), 0);
  natural (self->title, &h, &h);
  gtk_widget_measure (self->title, GTK_ORIENTATION_VERTICAL, w, &h, &h, NULL, NULL);
  put (self->title, x, (bar - h) / 2.0, w, h);
}

static void
vibeos_top_bar_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  VibeosTopBar *self = VIBEOS_TOP_BAR (widget);
  double c = collapsed (self);
  graphene_rect_t area = GRAPHENE_RECT_INIT (0, 0, gtk_widget_get_width (widget), gtk_widget_get_height (widget));

  if (self->fill.alpha > 0)
    gtk_snapshot_append_color (snapshot, &self->fill, &area);
  if (self->selecting)
    gtk_widget_snapshot_child (widget, self->close, snapshot);
  else if (self->has_back)
    gtk_widget_snapshot_child (widget, self->back, snapshot);
  /* a large bar shows its small title only once the large one has mostly scrolled under it */
  if (!self->large || self->selecting || c >= 1)
    gtk_widget_snapshot_child (widget, self->title, snapshot);
  else if (c > 0.5)
    {
      gtk_snapshot_push_opacity (snapshot, (c - 0.5) / 0.5);
      gtk_widget_snapshot_child (widget, self->title, snapshot);
      gtk_snapshot_pop (snapshot);
    }
  gtk_widget_snapshot_child (widget, self->selecting ? self->selection_actions : self->actions, snapshot);
}

/* the large title fades as it goes under the bar, and the small one comes in */
static void
sync_collapsed (VibeosTopBar *self)
{
  gtk_widget_set_opacity (self->large_title, self->large ? 1 - collapsed (self) : 1);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* ---- the parts ------------------------------------------------------------------------------------------------- */

static void
sync_parts (VibeosTopBar *self)
{
  gtk_widget_set_child_visible (self->back, self->has_back && !self->selecting);
  gtk_widget_set_child_visible (self->close, self->selecting);
  gtk_widget_set_child_visible (self->actions, !self->selecting);
  gtk_widget_set_child_visible (self->selection_actions, self->selecting);
  gtk_widget_set_visible (self->large_title, self->large);
  if (self->selecting)
    gtk_widget_add_css_class (GTK_WIDGET (self), "selection");
  else
    gtk_widget_remove_css_class (GTK_WIDGET (self), "selection");
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

static void
on_back_clicked (VibeosPressable *button, VibeosTopBar *self)
{
  g_signal_emit (self, signals[BACK_SIGNAL], 0);
}

static void
on_close_clicked (VibeosPressable *button, VibeosTopBar *self)
{
  g_signal_emit (self, signals[SELECTION_CLOSED], 0);
}

static void
on_scrolled (GtkAdjustment *adjustment, VibeosTopBar *self)
{
  sync_collapsed (self);
}

static void
on_layout_changed (VibeosLayout *layout, VibeosTopBar *self)
{
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

void
vibeos_top_bar_set_title (VibeosTopBar *self, const char *title)
{
  gtk_label_set_text (GTK_LABEL (self->title), title ? title : "");
  gtk_label_set_text (large_label (self), title ? title : "");
}

void
vibeos_top_bar_set_back (VibeosTopBar *self, gboolean back)
{
  self->has_back = !!back;
  sync_parts (self);
}

static GtkWidget *
action (GtkWidget *box, const char *icon_name)
{
  GtkWidget *button = vibeos_icon_button_new (icon_name);

  gtk_box_append (GTK_BOX (box), button);
  return button;
}

GtkWidget *
vibeos_top_bar_add_action (VibeosTopBar *self, const char *icon_name)
{
  GtkWidget *button = action (self->actions, icon_name);

  gtk_widget_queue_resize (GTK_WIDGET (self));
  return button;
}

GtkWidget *
vibeos_top_bar_add_selection_action (VibeosTopBar *self, const char *icon_name)
{
  GtkWidget *button = action (self->selection_actions, icon_name);

  gtk_widget_queue_resize (GTK_WIDGET (self));
  return button;
}

void
vibeos_top_bar_set_compact (VibeosTopBar *self, gboolean compact)
{
  self->compact = !!compact;
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

void
vibeos_top_bar_set_fill (VibeosTopBar *self, const GdkRGBA *fill)
{
  self->fill = fill ? *fill : (GdkRGBA) { 0, 0, 0, 0 };
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
vibeos_top_bar_set_large (VibeosTopBar *self, gboolean large)
{
  self->large = !!large;
  sync_parts (self);
  sync_collapsed (self);
}

void
vibeos_top_bar_set_scroll (VibeosTopBar *self, GtkAdjustment *vadjustment)
{
  if (self->scroll)
    {
      g_signal_handlers_disconnect_by_data (self->scroll, self);
      g_clear_object (&self->scroll);
    }
  if (vadjustment)
    {
      self->scroll = g_object_ref (vadjustment);
      g_signal_connect (vadjustment, "value-changed", G_CALLBACK (on_scrolled), self);
    }
  sync_collapsed (self);
}

GtkWidget *
vibeos_top_bar_get_large_title (VibeosTopBar *self)
{
  return self->large_title;
}

void
vibeos_top_bar_set_selection (VibeosTopBar *self, const char *title)
{
  self->selecting = title != NULL;
  gtk_label_set_text (GTK_LABEL (self->title), title ? title : gtk_label_get_text (large_label (self)));
  sync_parts (self);
}

/* ---- GObject --------------------------------------------------------------------------------------------------- */

static void
vibeos_top_bar_root (GtkWidget *widget)
{
  VibeosTopBar *self = VIBEOS_TOP_BAR (widget);

  GTK_WIDGET_CLASS (vibeos_top_bar_parent_class)->root (widget);
  self->layout = vibeos_layout_get (widget);
  if (self->layout)
    {
      g_object_ref (self->layout);
      g_signal_connect (self->layout, "changed", G_CALLBACK (on_layout_changed), self);
    }
}

static void
vibeos_top_bar_unroot (GtkWidget *widget)
{
  VibeosTopBar *self = VIBEOS_TOP_BAR (widget);

  if (self->layout)
    {
      g_signal_handlers_disconnect_by_data (self->layout, self);
      g_clear_object (&self->layout);
    }
  GTK_WIDGET_CLASS (vibeos_top_bar_parent_class)->unroot (widget);
}

static void
vibeos_top_bar_dispose (GObject *object)
{
  VibeosTopBar *self = VIBEOS_TOP_BAR (object);

  vibeos_top_bar_set_scroll (self, NULL);
  g_clear_pointer (&self->back, gtk_widget_unparent);
  g_clear_pointer (&self->close, gtk_widget_unparent);
  g_clear_pointer (&self->title, gtk_widget_unparent);
  g_clear_object (&self->large_title);
  g_clear_pointer (&self->actions, gtk_widget_unparent);
  g_clear_pointer (&self->selection_actions, gtk_widget_unparent);
  G_OBJECT_CLASS (vibeos_top_bar_parent_class)->dispose (object);
}

static void
vibeos_top_bar_class_init (VibeosTopBarClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = vibeos_top_bar_dispose;
  widget_class->measure = vibeos_top_bar_measure;
  widget_class->size_allocate = vibeos_top_bar_size_allocate;
  widget_class->snapshot = vibeos_top_bar_snapshot;
  widget_class->root = vibeos_top_bar_root;
  widget_class->unroot = vibeos_top_bar_unroot;
  gtk_widget_class_set_css_name (widget_class, "vibeos-top-bar");

  signals[BACK_SIGNAL] = g_signal_new ("back", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                       NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SELECTION_CLOSED] = g_signal_new ("selection-closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                            NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
vibeos_top_bar_init (VibeosTopBar *self)
{
  GtkWidget *widget = GTK_WIDGET (self);
  GtkWidget *arrow = gtk_image_new_from_icon_name ("go-previous-symbolic"), *label;

  gtk_widget_add_css_class (widget, "vibeos-top-bar");

  gtk_image_set_pixel_size (GTK_IMAGE (arrow), 24);
  self->back = vibeos_pressable_new (arrow);
  gtk_widget_add_css_class (self->back, "vibeos-top-bar-back");
  vibeos_pressable_set_fill_role (VIBEOS_PRESSABLE (self->back), VIBEOS_SURFACE_CONTAINER_HIGHEST, FALSE);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->back), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Back"), -1);
  g_signal_connect (self->back, "clicked", G_CALLBACK (on_back_clicked), self);
  gtk_widget_set_parent (self->back, widget);

  self->close = vibeos_icon_button_new ("window-close-symbolic");
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->close), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Close"), -1);
  g_signal_connect (self->close, "clicked", G_CALLBACK (on_close_clicked), self);
  gtk_widget_set_parent (self->close, widget);

  self->title = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (self->title), 0);
  gtk_label_set_ellipsize (GTK_LABEL (self->title), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode (GTK_LABEL (self->title), TRUE);
  gtk_widget_add_css_class (self->title, "vibeos-top-bar-title");
  gtk_widget_set_parent (self->title, widget);

  /* up to three lines, at the bottom of its band, smaller rather than with a word broken */
  label = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_lines (GTK_LABEL (label), 3);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_valign (label, GTK_ALIGN_END);
  self->large_title = g_object_ref_sink (vibeos_fit_label_new (label));
  gtk_widget_add_css_class (self->large_title, "vibeos-top-bar-large-title");

  self->actions = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_parent (self->actions, widget);
  self->selection_actions = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_parent (self->selection_actions, widget);
  sync_parts (self);
}

GtkWidget *
vibeos_top_bar_new (const char *title)
{
  VibeosTopBar *self = g_object_new (VIBEOS_TYPE_TOP_BAR, NULL);

  vibeos_top_bar_set_title (self, title);
  return GTK_WIDGET (self);
}
