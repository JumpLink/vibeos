/* VibeosSearchBar and VibeosSearchView: see vibeos-search.h. */
#include "vibeos-search.h"
#include "vibeos-anim.h"
#include "vibeos-pressable.h"
#include "vibeos-style.h"

#include <math.h>

#define BAR_H        56
#define BAR_SIDE     16
#define BAR_ICON_X   16
#define BAR_TEXT_X   52
#define ICON         24
#define END_PAD      8       /* the end widget's inset */
#define VIEW_BAR_H   72      /* the view's row with the back button and the entry */
#define VIEW_BACK_X  4
#define BUTTON       48
#define VIEW_TEXT_X  68
#define FADE_IN_MS   250
#define FADE_OUT_MS  150

/* ---- the bar --------------------------------------------------------------------------------------------------- */

struct _VibeosSearchBar {
  GtkWidget  parent;
  GtkWidget *target;           /* the pill's tap target, with the ripple */
  GtkWidget *hint, *end;
  GdkRGBA    fill;
  gboolean   has_fill;
  int        height, side, icon_x, text_x;
};

G_DEFINE_FINAL_TYPE (VibeosSearchBar, vibeos_search_bar, GTK_TYPE_WIDGET)

enum { BAR_ACTIVATED, N_BAR_SIGNALS };
static guint bar_signals[N_BAR_SIGNALS];

static void
put (GtkWidget *child, double x, double y, int width, int height)
{
  gtk_widget_allocate (child, width, height, -1, gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (x, y)));
}

static void
natural (GtkWidget *widget, int for_width, int *width, int *height)
{
  int unused;

  gtk_widget_measure (widget, GTK_ORIENTATION_HORIZONTAL, -1, &unused, width, NULL, NULL);
  gtk_widget_measure (widget, GTK_ORIENTATION_VERTICAL, for_width >= 0 ? for_width : *width, &unused, height,
                      NULL, NULL);
}

static graphene_rect_t
pill_rect (VibeosSearchBar *self)
{
  return GRAPHENE_RECT_INIT (self->side, 0, MAX (gtk_widget_get_width (GTK_WIDGET (self)) - 2 * self->side, 0),
                             self->height);
}

static void
vibeos_search_bar_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                           int *min_baseline, int *nat_baseline)
{
  VibeosSearchBar *self = VIBEOS_SEARCH_BAR (widget);

  if (orientation == GTK_ORIENTATION_VERTICAL)
    *min = *nat = self->height;
  else
    *min = *nat = 2 * self->side + self->text_x + BUTTON;
}

static void
vibeos_search_bar_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  VibeosSearchBar *self = VIBEOS_SEARCH_BAR (widget);
  graphene_rect_t pill = pill_rect (self);
  double end = pill.origin.x + pill.size.width;
  int w, h;

  put (self->target, pill.origin.x, 0, (int) pill.size.width, self->height);
  if (self->end)
    {
      natural (self->end, -1, &w, &h);
      end -= END_PAD + w;
      put (self->end, end, (self->height - h) / 2.0, w, h);
    }
  w = MAX ((int) (end - pill.origin.x - self->text_x - 8), 0);
  gtk_widget_measure (self->hint, GTK_ORIENTATION_VERTICAL, w, &h, &h, NULL, NULL);
  put (self->hint, pill.origin.x + self->text_x, (self->height - h) / 2.0, w, h);
}

static void
vibeos_search_bar_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  VibeosSearchBar *self = VIBEOS_SEARCH_BAR (widget);
  graphene_rect_t pill = pill_rect (self);
  GskRoundedRect round;
  GdkRGBA fill = self->has_fill ? self->fill : vibeos_color (VIBEOS_SURFACE_CONTAINER_HIGH);
  GdkRGBA icon = vibeos_color (VIBEOS_ON_SURFACE_VARIANT);

  gsk_rounded_rect_init_from_rect (&round, &pill, self->height / 2.0);
  gtk_snapshot_push_rounded_clip (snapshot, &round);
  gtk_snapshot_append_color (snapshot, &fill, &pill);
  gtk_widget_snapshot_child (widget, self->target, snapshot);
  gtk_snapshot_pop (snapshot);
  vibeos_snapshot_icon (snapshot, widget, "system-search-symbolic", pill.origin.x + self->icon_x,
                        (self->height - ICON) / 2.0, ICON, &icon);
  gtk_widget_snapshot_child (widget, self->hint, snapshot);
  if (self->end)
    gtk_widget_snapshot_child (widget, self->end, snapshot);
}

static void
on_bar_clicked (VibeosPressable *target, VibeosSearchBar *self)
{
  g_signal_emit (self, bar_signals[BAR_ACTIVATED], 0);
}

void
vibeos_search_bar_set_hint (VibeosSearchBar *self, const char *hint)
{
  gtk_label_set_text (GTK_LABEL (self->hint), hint ? hint : "");
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->target), GTK_ACCESSIBLE_PROPERTY_LABEL, hint, -1);
}

void
vibeos_search_bar_set_geometry (VibeosSearchBar *self, int height, int side, int icon_x, int text_x)
{
  self->height = height;
  self->side = side;
  self->icon_x = icon_x;
  self->text_x = text_x;
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (self->target), VIBEOS_RIPPLE_BOUNDED, height / 2.0);
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

void
vibeos_search_bar_set_fill (VibeosSearchBar *self, const GdkRGBA *fill)
{
  self->has_fill = fill != NULL;
  if (fill)
    self->fill = *fill;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
vibeos_search_bar_set_end (VibeosSearchBar *self, GtkWidget *end)
{
  g_clear_pointer (&self->end, gtk_widget_unparent);
  self->end = end;
  if (end)
    gtk_widget_set_parent (end, GTK_WIDGET (self));
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

static void
vibeos_search_bar_dispose (GObject *object)
{
  VibeosSearchBar *self = VIBEOS_SEARCH_BAR (object);

  g_clear_pointer (&self->end, gtk_widget_unparent);
  g_clear_pointer (&self->hint, gtk_widget_unparent);
  g_clear_pointer (&self->target, gtk_widget_unparent);
  G_OBJECT_CLASS (vibeos_search_bar_parent_class)->dispose (object);
}

static void
vibeos_search_bar_class_init (VibeosSearchBarClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = vibeos_search_bar_dispose;
  widget_class->measure = vibeos_search_bar_measure;
  widget_class->size_allocate = vibeos_search_bar_size_allocate;
  widget_class->snapshot = vibeos_search_bar_snapshot;
  gtk_widget_class_set_css_name (widget_class, "vibeos-search-bar");

  bar_signals[BAR_ACTIVATED] = g_signal_new ("activated", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                             NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
vibeos_search_bar_init (VibeosSearchBar *self)
{
  self->target = vibeos_pressable_new (NULL);
  gtk_widget_add_css_class (self->target, "vibeos-search-pill");
  gtk_widget_set_parent (self->target, GTK_WIDGET (self));
  g_signal_connect (self->target, "clicked", G_CALLBACK (on_bar_clicked), self);

  self->hint = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (self->hint), 0);
  gtk_label_set_ellipsize (GTK_LABEL (self->hint), PANGO_ELLIPSIZE_END);
  gtk_widget_set_can_target (self->hint, FALSE);
  gtk_widget_add_css_class (self->hint, "vibeos-search-hint");
  gtk_widget_set_parent (self->hint, GTK_WIDGET (self));

  vibeos_search_bar_set_geometry (self, BAR_H, BAR_SIDE, BAR_ICON_X, BAR_TEXT_X);
}

GtkWidget *
vibeos_search_bar_new (const char *hint)
{
  VibeosSearchBar *self = g_object_new (VIBEOS_TYPE_SEARCH_BAR, NULL);

  vibeos_search_bar_set_hint (self, hint);
  return GTK_WIDGET (self);
}

/* ---- the view -------------------------------------------------------------------------------------------------- */

struct _VibeosSearchView {
  GtkWidget   parent;
  GtkWidget  *back, *entry, *clear, *content;
  VibeosAnim *fade;
  double      opacity;
  gboolean    closing;
};

G_DEFINE_FINAL_TYPE (VibeosSearchView, vibeos_search_view, GTK_TYPE_WIDGET)

enum { VIEW_CHANGED, VIEW_CLOSED, N_VIEW_SIGNALS };
static guint view_signals[N_VIEW_SIGNALS];

static void
vibeos_search_view_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                            int *min_baseline, int *nat_baseline)
{
  *min = *nat = orientation == GTK_ORIENTATION_VERTICAL ? VIEW_BAR_H : VIEW_TEXT_X + 2 * BUTTON;
}

static void
vibeos_search_view_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  VibeosSearchView *self = VIBEOS_SEARCH_VIEW (widget);
  int w, h, entry_w;

  put (self->back, VIEW_BACK_X, (VIEW_BAR_H - BUTTON) / 2.0, BUTTON, BUTTON);
  put (self->clear, width - VIEW_BACK_X - BUTTON, (VIEW_BAR_H - BUTTON) / 2.0, BUTTON, BUTTON);
  entry_w = MAX (width - VIEW_TEXT_X - VIEW_BACK_X - BUTTON, 1);
  natural (self->entry, entry_w, &w, &h);
  put (self->entry, VIEW_TEXT_X, (VIEW_BAR_H - h) / 2.0, entry_w, h);
  if (self->content)
    put (self->content, 0, VIEW_BAR_H + 1, width, MAX (height - VIEW_BAR_H - 1, 0));
}

static void
vibeos_search_view_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  VibeosSearchView *self = VIBEOS_SEARCH_VIEW (widget);
  double width = gtk_widget_get_width (widget), height = gtk_widget_get_height (widget);
  GdkRGBA bar = vibeos_color (VIBEOS_SURFACE_CONTAINER_HIGH), page = vibeos_color (VIBEOS_SURFACE_CONTAINER);
  GdkRGBA line = vibeos_color (VIBEOS_OUTLINE);

  gtk_snapshot_push_opacity (snapshot, self->opacity);
  gtk_snapshot_append_color (snapshot, &page, &GRAPHENE_RECT_INIT (0, 0, width, height));
  gtk_snapshot_append_color (snapshot, &bar, &GRAPHENE_RECT_INIT (0, 0, width, VIEW_BAR_H));
  gtk_snapshot_append_color (snapshot, &line, &GRAPHENE_RECT_INIT (0, VIEW_BAR_H, width, 1));
  GTK_WIDGET_CLASS (vibeos_search_view_parent_class)->snapshot (widget, snapshot);
  gtk_snapshot_pop (snapshot);
}

/* out of the overlay once faded out: from an idle, since leaving it can drop the last reference, and with it the
 * animation running this */
static gboolean
leave_overlay (gpointer data)
{
  VibeosSearchView *self = data;
  GtkWidget *parent = gtk_widget_get_parent (GTK_WIDGET (self));

  if (self->closing && GTK_IS_OVERLAY (parent))
    {
      self->closing = FALSE;
      gtk_overlay_remove_overlay (GTK_OVERLAY (parent), GTK_WIDGET (self));
    }
  return G_SOURCE_REMOVE;
}

static void
on_fade (VibeosAnim *anim, double value, gpointer data)
{
  VibeosSearchView *self = data;

  self->opacity = value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  if (!vibeos_anim_is_running (anim) && self->closing)
    g_idle_add_full (G_PRIORITY_DEFAULT, leave_overlay, g_object_ref (self), g_object_unref);
}

static void
on_text_changed (GtkEditable *entry, VibeosSearchView *self)
{
  const char *text = gtk_editable_get_text (entry);

  gtk_widget_set_visible (self->clear, text && *text);
  g_signal_emit (self, view_signals[VIEW_CHANGED], 0, text);
}

static void
on_clear (VibeosPressable *button, VibeosSearchView *self)
{
  gtk_editable_set_text (GTK_EDITABLE (self->entry), "");
  gtk_widget_grab_focus (self->entry);
}

static void
on_back (VibeosPressable *button, VibeosSearchView *self)
{
  vibeos_search_view_close (self);
}

static gboolean
on_key (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, VibeosSearchView *self)
{
  if (keyval != GDK_KEY_Escape)
    return FALSE;
  vibeos_search_view_close (self);
  return TRUE;
}

void
vibeos_search_view_set_content (VibeosSearchView *self, GtkWidget *content)
{
  g_clear_pointer (&self->content, gtk_widget_unparent);
  self->content = content;
  if (content)
    gtk_widget_set_parent (content, GTK_WIDGET (self));
}

const char *
vibeos_search_view_get_text (VibeosSearchView *self)
{
  return gtk_editable_get_text (GTK_EDITABLE (self->entry));
}

void
vibeos_search_view_set_text (VibeosSearchView *self, const char *text)
{
  gtk_editable_set_text (GTK_EDITABLE (self->entry), text ? text : "");
}

void
vibeos_search_view_open (VibeosSearchView *self, GtkWidget *widget)
{
  GtkOverlay *overlay = vibeos_window_get_overlay (widget);

  g_return_if_fail (overlay != NULL);

  self->closing = FALSE;
  if (gtk_widget_get_parent (GTK_WIDGET (self)) != GTK_WIDGET (overlay))
    {
      gtk_widget_set_halign (GTK_WIDGET (self), GTK_ALIGN_FILL);
      gtk_widget_set_valign (GTK_WIDGET (self), GTK_ALIGN_FILL);
      gtk_overlay_add_overlay (overlay, GTK_WIDGET (self));
    }
  vibeos_anim_start (self->fade, self->opacity, 1, FADE_IN_MS, 0, SPONGY_LINEAR_OUT_SLOW_IN);
  gtk_widget_grab_focus (self->entry);
}

void
vibeos_search_view_close (VibeosSearchView *self)
{
  if (self->closing || !gtk_widget_get_parent (GTK_WIDGET (self)))
    return;
  self->closing = TRUE;
  g_signal_emit (self, view_signals[VIEW_CLOSED], 0);
  vibeos_anim_start (self->fade, self->opacity, 0, FADE_OUT_MS, 0, SPONGY_FAST_OUT_LINEAR_IN);
}

static void
vibeos_search_view_dispose (GObject *object)
{
  VibeosSearchView *self = VIBEOS_SEARCH_VIEW (object);

  g_clear_pointer (&self->fade, vibeos_anim_free);
  g_clear_pointer (&self->content, gtk_widget_unparent);
  g_clear_pointer (&self->back, gtk_widget_unparent);
  g_clear_pointer (&self->entry, gtk_widget_unparent);
  g_clear_pointer (&self->clear, gtk_widget_unparent);
  G_OBJECT_CLASS (vibeos_search_view_parent_class)->dispose (object);
}

static void
vibeos_search_view_class_init (VibeosSearchViewClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = vibeos_search_view_dispose;
  widget_class->measure = vibeos_search_view_measure;
  widget_class->size_allocate = vibeos_search_view_size_allocate;
  widget_class->snapshot = vibeos_search_view_snapshot;
  gtk_widget_class_set_css_name (widget_class, "vibeos-search-view");

  view_signals[VIEW_CHANGED] = g_signal_new ("search-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                             NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  view_signals[VIEW_CLOSED] = g_signal_new ("closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                            NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
vibeos_search_view_init (VibeosSearchView *self)
{
  GtkWidget *widget = GTK_WIDGET (self);
  GtkEventController *keys = gtk_event_controller_key_new ();

  self->back = vibeos_icon_button_new ("go-previous-symbolic");
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->back), GTK_ACCESSIBLE_PROPERTY_LABEL, "Back", -1);
  g_signal_connect (self->back, "clicked", G_CALLBACK (on_back), self);
  gtk_widget_set_parent (self->back, widget);

  self->entry = gtk_text_new ();
  gtk_widget_add_css_class (self->entry, "vibeos-search-entry");
  g_signal_connect (self->entry, "changed", G_CALLBACK (on_text_changed), self);
  gtk_widget_set_parent (self->entry, widget);

  self->clear = vibeos_icon_button_new ("edit-clear-symbolic");
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->clear), GTK_ACCESSIBLE_PROPERTY_LABEL, "Clear", -1);
  g_signal_connect (self->clear, "clicked", G_CALLBACK (on_clear), self);
  gtk_widget_set_parent (self->clear, widget);
  gtk_widget_set_visible (self->clear, FALSE);

  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key), self);
  gtk_widget_add_controller (widget, keys);
  self->fade = vibeos_anim_new (widget, on_fade, self);
}

GtkWidget *
vibeos_search_view_new (const char *hint)
{
  VibeosSearchView *self = g_object_new (VIBEOS_TYPE_SEARCH_VIEW, NULL);

  gtk_text_set_placeholder_text (GTK_TEXT (self->entry), hint);
  return GTK_WIDGET (self);
}
