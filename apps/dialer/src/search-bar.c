/* The search bar: see search-bar.h. */
#include "search-bar.h"
#include "text.h"

#include <glib/gi18n.h>
#include <math.h>
#include <neo.h>

#define TOOLBAR_HEIGHT  64      /* the expanded bar's row */
#define COLLAPSED_HEIGHT 48
#define MARGIN          8       /* round the collapsed bar; none once expanded */
#define CORNER          28
#define BUTTON          48
#define ICON            24
#define START           8       /* the magnifier's and the back arrow's inset */
#define TEXT_START      (START + BUTTON + 8)
#define EXPAND_MS       200
#define MARGIN_FRACTION 0.8     /* where the expand animation starts the margins from */

#define BACKGROUND      0x2c3132
#define BAR_ICON        0xfefefe

#define HINT_STYLE      ((TextStyle) { TEXT_GSF, 16, 700 })     /* the collapsed bar's "Search contacts" */

struct _DialerSearchBar
{
  GtkWidget parent;
  GtkWidget *mic, *menu;                /* the collapsed row's buttons */
  GtkWidget *back, *field, *clear;      /* the expanded row */
  PangoLayout *hint;
  NeoRipple ripple;
  NeoAnim *anim;
  double progress;              /* the cross-fade: 0 the collapsed row … 1 the expanded one */
  double fraction;              /* the margins and the row's height, 0 expanded to 1 collapsed */
  gboolean expanded;
  gboolean rounded;             /* the rounded background; the expanded bar is square once it is open */
  gboolean focus;               /* the field takes the focus when the expand ends */
  gboolean skip_change;
};

G_DEFINE_FINAL_TYPE (DialerSearchBar, dialer_search_bar, GTK_TYPE_WIDGET)

enum { SEARCH_CLICKED, BACK_CLICKED, QUERY_CHANGED, VOICE_CLICKED, MENU_CLICKED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* ---- geometry ------------------------------------------------------------------------------------------------- */

/* the margins and the expanded row's height are rounded down to whole device pixels. The row
 * starts at COLLAPSED_HEIGHT with MARGIN margins (which fraction 1 gives too); after a collapse they stay where the
 * animation left them, at MARGIN_FRACTION, so a bar that has been opened once is a little taller (the expanded row
 * is invisible, not gone, and still sizes the bar). */
static double
px_floor (GtkWidget *widget, double dp)
{
  int scale = neo_device_scale (widget);

  return floor (dp * scale) / scale;
}

static graphene_rect_t
bar_rect (DialerSearchBar *self, double *row_height)
{
  GtkWidget *widget = GTK_WIDGET (self);
  double margin = px_floor (widget, MARGIN * self->fraction);
  double row = px_floor (widget, TOOLBAR_HEIGHT - (TOOLBAR_HEIGHT - COLLAPSED_HEIGHT) * self->fraction);

  *row_height = row;
  return GRAPHENE_RECT_INIT (margin, margin, gtk_widget_get_width (widget) - 2 * margin, MAX (COLLAPSED_HEIGHT, row));
}

static void
dialer_search_bar_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  DialerSearchBar *self = DIALER_SEARCH_BAR (widget);
  double row;
  graphene_rect_t bar = bar_rect (self, &row);
  double x = bar.origin.x, right = bar.origin.x + bar.size.width;
  double collapsed_y = bar.origin.y + (bar.size.height - BUTTON) / 2;
  double expanded_y = bar.origin.y + (row - BUTTON) / 2;
  double field_end = gtk_widget_get_visible (self->clear) ? right - BUTTON : right;
  int field_h;

  /* collapsed: centred in the bar; ⋮ at the end, the mic before it */
  gtk_widget_allocate (self->menu, BUTTON, BUTTON, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (right - BUTTON, collapsed_y)));
  gtk_widget_allocate (self->mic, BUTTON, BUTTON, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (right - 2 * BUTTON, collapsed_y)));
  /* expanded: the row at the bar's top, its views centred in it */
  gtk_widget_allocate (self->back, BUTTON, BUTTON, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (x + START, expanded_y)));
  gtk_widget_allocate (self->clear, BUTTON, BUTTON, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (right - BUTTON, expanded_y)));
  gtk_widget_measure (self->field, GTK_ORIENTATION_VERTICAL, -1, NULL, &field_h, NULL, NULL);
  field_h = MAX (field_h, BUTTON);
  gtk_widget_allocate (self->field, MAX (0, (int) (field_end - x - TEXT_START)), field_h, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (x + TEXT_START,
                                                                            bar.origin.y + (row - field_h) / 2)));
}

static void
dialer_search_bar_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                           int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = orientation == GTK_ORIENTATION_VERTICAL ? TOOLBAR_HEIGHT : 0;
}

/* ---- drawing -------------------------------------------------------------------------------------------------- */

static void
dialer_search_bar_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  DialerSearchBar *self = DIALER_SEARCH_BAR (widget);
  double row;
  graphene_rect_t bar = bar_rect (self, &row);
  GskRoundedRect shape;
  GdkRGBA background = text_rgb (BACKGROUND);

  /* the radius clamped to half the height: fully round ends */
  gsk_rounded_rect_init_from_rect (&shape, &bar, self->rounded ? MIN (CORNER, bar.size.height / 2) : 0);
  gtk_snapshot_push_rounded_clip (snapshot, &shape);
  gtk_snapshot_append_color (snapshot, &background, &bar);
  /* the bar's ripple, faint white */
  if (neo_ripple_is_active (&self->ripple))
    neo_ripple_snapshot (&self->ripple, snapshot, &bar, &(GdkRGBA) { 1, 1, 1, 0.06 });
  gtk_snapshot_pop (snapshot);

  if (self->progress < 1)
    {
      GdkRGBA icon = text_rgb (BAR_ICON), hint = text_rgb (COLOR_TEXT_SECONDARY);
      double cy = bar.origin.y + bar.size.height / 2;

      gtk_snapshot_push_opacity (snapshot, 1 - self->progress);
      neo_snapshot_icon (snapshot, widget, "dialer-search-symbolic", bar.origin.x + START + (BUTTON - ICON) / 2.0,
                         cy - ICON / 2.0, ICON, &icon);
      text_draw (snapshot, self->hint, HINT_STYLE, bar.origin.x + TEXT_START,
                 cy - text_box_height (self->hint, HINT_STYLE) / 2, &hint);
      gtk_widget_snapshot_child (widget, self->mic, snapshot);
      gtk_widget_snapshot_child (widget, self->menu, snapshot);
      gtk_snapshot_pop (snapshot);
    }
  if (self->progress > 0)
    {
      gtk_snapshot_push_opacity (snapshot, self->progress);
      gtk_widget_snapshot_child (widget, self->back, snapshot);
      gtk_widget_snapshot_child (widget, self->field, snapshot);
      gtk_widget_snapshot_child (widget, self->clear, snapshot);
      gtk_snapshot_pop (snapshot);
    }
}

/* ---- expanding and collapsing --------------------------------------------------------------------------------- */

static void
set_rows (DialerSearchBar *self)
{
  /* a row that has faded out takes no touches (View.GONE / INVISIBLE at the end of the fade) */
  gtk_widget_set_child_visible (self->mic, self->progress < 1);
  gtk_widget_set_child_visible (self->menu, self->progress < 1);
  gtk_widget_set_child_visible (self->back, self->progress > 0);
  gtk_widget_set_child_visible (self->field, self->progress > 0);
  gtk_widget_set_child_visible (self->clear, self->progress > 0);
}

static void
on_anim (NeoAnim *anim, double value, gpointer data)
{
  DialerSearchBar *self = data;

  self->progress = value;
  self->fraction = MARGIN_FRACTION * (1 - value);
  set_rows (self);
  if (!neo_anim_is_running (anim))
    {
      if (self->expanded)
        {
          /* the animation over: the square background, and the focus if asked for */
          self->rounded = FALSE;
          if (self->focus)
            gtk_widget_grab_focus (self->field);
        }
      else
        {
          self->rounded = TRUE;
          dialer_search_bar_set_query (self, "");
        }
    }
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

void
dialer_search_bar_expand (DialerSearchBar *self, gboolean focus)
{
  if (self->expanded)
    return;
  self->expanded = TRUE;
  self->focus = focus;
  /* the margins jump to 0.8 of theirs as it starts, and shrink with the cross-fade (accelerate-decelerate) */
  neo_anim_start (self->anim, 0, 1, EXPAND_MS, 0, SPONGY_ACCELERATE_DECELERATE);
}

void
dialer_search_bar_collapse (DialerSearchBar *self)
{
  if (!self->expanded)
    return;
  self->expanded = FALSE;
  dialer_search_bar_hide_keyboard (self);
  neo_anim_start (self->anim, 1, 0, EXPAND_MS, 0, SPONGY_ACCELERATE_DECELERATE);
}

gboolean
dialer_search_bar_get_expanded (DialerSearchBar *self)
{
  return self->expanded;
}

const char *
dialer_search_bar_get_query (DialerSearchBar *self)
{
  return gtk_editable_get_text (GTK_EDITABLE (self->field));
}

void
dialer_search_bar_set_query (DialerSearchBar *self, const char *query)
{
  if (g_strcmp0 (query, dialer_search_bar_get_query (self)) == 0)
    return;
  self->skip_change = TRUE;
  gtk_editable_set_text (GTK_EDITABLE (self->field), query);
  gtk_editable_set_position (GTK_EDITABLE (self->field), -1);
  self->skip_change = FALSE;
}

/* the keyboard goes with the focus */
void
dialer_search_bar_hide_keyboard (DialerSearchBar *self)
{
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));

  if (root && gtk_widget_has_focus (self->field))
    gtk_root_set_focus (root, NULL);
}

GtkWidget *
dialer_search_bar_get_menu_button (DialerSearchBar *self)
{
  return self->menu;
}

/* ---- input ---------------------------------------------------------------------------------------------------- */

static void
on_changed (GtkEditable *field, DialerSearchBar *self)
{
  gboolean empty = !*gtk_editable_get_text (field);

  if (gtk_widget_get_visible (self->clear) == empty)
    {
      gtk_widget_set_visible (self->clear, !empty);
      gtk_widget_queue_allocate (GTK_WIDGET (self));
    }
  if (!self->skip_change)
    g_signal_emit (self, signals[QUERY_CHANGED], 0);
}

static void
on_clear (DialerSearchBar *self)
{
  gtk_editable_set_text (GTK_EDITABLE (self->field), "");
}

static void
on_voice (DialerSearchBar *self)
{
  g_signal_emit (self, signals[VOICE_CLICKED], 0);
}

static void
on_menu (DialerSearchBar *self)
{
  g_signal_emit (self, signals[MENU_CLICKED], 0);
}

static void
on_back (DialerSearchBar *self)
{
  if (self->expanded)
    g_signal_emit (self, signals[BACK_CLICKED], 0);
}

/* the bar is one button while collapsed; its own buttons take their taps */
static void
on_pressed (GtkGestureClick *click, int n_press, double x, double y, DialerSearchBar *self)
{
  if (self->expanded || neo_pick_is_tappable (GTK_WIDGET (self), x, y))
    {
      gtk_gesture_set_state (GTK_GESTURE (click), GTK_EVENT_SEQUENCE_DENIED);
      return;
    }
  neo_ripple_press (&self->ripple, x, y);
}

static void
on_released (GtkGestureClick *click, int n_press, double x, double y, DialerSearchBar *self)
{
  neo_ripple_release (&self->ripple);
  if (!self->expanded)
    g_signal_emit (self, signals[SEARCH_CLICKED], 0);
}

static void
on_stopped (GtkGestureClick *click, DialerSearchBar *self)
{
  neo_ripple_release (&self->ripple);
}

/* ---- GObject -------------------------------------------------------------------------------------------------- */

static void
dialer_search_bar_dispose (GObject *object)
{
  DialerSearchBar *self = DIALER_SEARCH_BAR (object);
  GtkWidget *child;

  g_clear_pointer (&self->anim, neo_anim_free);
  neo_ripple_clear (&self->ripple);
  g_clear_object (&self->hint);
  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self))))
    gtk_widget_unparent (child);
  G_OBJECT_CLASS (dialer_search_bar_parent_class)->dispose (object);
}

static void
dialer_search_bar_class_init (DialerSearchBarClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = dialer_search_bar_dispose;
  widget_class->measure = dialer_search_bar_measure;
  widget_class->size_allocate = dialer_search_bar_size_allocate;
  widget_class->snapshot = dialer_search_bar_snapshot;
  gtk_widget_class_set_css_name (widget_class, "dialer-search-bar");
  signals[SEARCH_CLICKED] = g_signal_new ("search-clicked", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                          NULL, NULL, G_TYPE_NONE, 0);
  signals[BACK_CLICKED] = g_signal_new ("back-clicked", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                        NULL, NULL, G_TYPE_NONE, 0);
  signals[QUERY_CHANGED] = g_signal_new ("query-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                         NULL, NULL, G_TYPE_NONE, 0);
  signals[VOICE_CLICKED] = g_signal_new ("voice-clicked", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                         NULL, NULL, G_TYPE_NONE, 0);
  signals[MENU_CLICKED] = g_signal_new ("menu-clicked", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                        NULL, NULL, G_TYPE_NONE, 0);
}

static GtkWidget *
icon_button (DialerSearchBar *self, const char *icon, const char *css_class, const char *description)
{
  GtkWidget *button = neo_icon_button_new (icon);

  gtk_widget_add_css_class (button, css_class);
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, description, -1);
  gtk_widget_set_parent (button, GTK_WIDGET (self));
  return button;
}

static void
dialer_search_bar_init (DialerSearchBar *self)
{
  GtkGesture *click = gtk_gesture_click_new ();

  self->fraction = 1;
  self->rounded = TRUE;
  self->hint = text_layout (GTK_WIDGET (self), HINT_STYLE, _("Search contacts"), 0);

  self->mic = icon_button (self, "dialer-mic-symbolic", "search-bar-icon", _("Start voice search"));
  self->menu = icon_button (self, "dialer-more-symbolic", "search-bar-icon", _("More options"));
  /* named "Back", the button the shell's back gesture clicks; "stop searching" is its description */
  self->back = icon_button (self, "dialer-back-symbolic", "dialer-icon", _("Back"));
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->back), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
                                  _("stop searching"), -1);
  /* the field is not darkened with the rest: its colours are in dialer.css */
  self->field = gtk_text_new ();
  gtk_widget_add_css_class (self->field, "dialer-search-field");
  gtk_text_set_placeholder_text (GTK_TEXT (self->field), _("Search contacts"));
  gtk_widget_set_parent (self->field, GTK_WIDGET (self));
  self->clear = icon_button (self, "dialer-close-symbolic", "dialer-icon", _("Clear search"));
  gtk_widget_set_visible (self->clear, FALSE);
  g_signal_connect_swapped (self->mic, "clicked", G_CALLBACK (on_voice), self);
  g_signal_connect_swapped (self->menu, "clicked", G_CALLBACK (on_menu), self);
  g_signal_connect_swapped (self->back, "clicked", G_CALLBACK (on_back), self);
  g_signal_connect_swapped (self->clear, "clicked", G_CALLBACK (on_clear), self);
  g_signal_connect (self->field, "changed", G_CALLBACK (on_changed), self);

  neo_ripple_init (&self->ripple, GTK_WIDGET (self));
  self->anim = neo_anim_new (GTK_WIDGET (self), on_anim, self);
  g_signal_connect (click, "pressed", G_CALLBACK (on_pressed), self);
  g_signal_connect (click, "released", G_CALLBACK (on_released), self);
  g_signal_connect (click, "stopped", G_CALLBACK (on_stopped), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
  set_rows (self);
}

GtkWidget *
dialer_search_bar_new (void)
{
  return g_object_new (DIALER_TYPE_SEARCH_BAR, NULL);
}
