/* The Dialer's bottom navigation: see nav-bar.h. The geometry is bottom_nav_item.xml's, measured on
 * still:dialer-recents (the bar y 2013…2280 px, the pill 192 x 96 px at y 2061, the label box at y 2181). */
#include "nav-bar.h"
#include "text.h"

#include <glib/gi18n.h>
#include <neo.h>

#define BAR_HEIGHT      89      /* 16 + the 32 dp indicator frame + 8 + the 17 dp label + 16 */
#define PILL_TOP        16
#define PILL_WIDTH      64
#define PILL_HEIGHT     32
#define ICON            24
#define LABEL_TOP       (PILL_TOP + PILL_HEIGHT + 8)
#define INDICATOR_MS    400     /* BottomNavItem's ValueAnimator, FAST_OUT_SLOW_IN */
#define BADGE_TOP       2
#define BADGE_MIN       20
#define BADGE_PAD_X     6
#define BADGE_PAD_BOTTOM 1
#define BADGE_STROKE    2

#define BACKGROUND      0x2c3132        /* rounded_dialpad_bg_color */
#define PILL            0x324b4e        /* dialer_pill_color, night */
#define ICON_SELECTED   0xfdfdfd        /* textColorPrimary as an icon tint, inverted */
#define BADGE_FILL      0x1f2728        /* colorPrimary as a background */
#define BADGE_TEXT      0xdadce0        /* textColorPrimaryInverse */

#define LABEL_STYLE     ((TextStyle) { TEXT_GSF, 12, 800 })     /* OVERLINE at 12 sp */

typedef struct
{
  DialerNavBar *bar;
  char *icon, *selected_icon;
  PangoLayout *label, *badge;
  guint count;
  double progress;              /* the pill: 0 gone … 1 shown */
  NeoAnim *indicator;
} Item;

struct _DialerNavBar
{
  GtkWidget parent;
  GPtrArray *items;
  int selected;
  int pressed;
};

G_DEFINE_FINAL_TYPE (DialerNavBar, dialer_nav_bar, GTK_TYPE_WIDGET)

enum { SELECTED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
item_free (gpointer data)
{
  Item *item = data;

  neo_anim_free (item->indicator);
  g_clear_object (&item->label);
  g_clear_object (&item->badge);
  g_free (item->icon);
  g_free (item->selected_icon);
  g_free (item);
}

static double
column (DialerNavBar *self)
{
  return self->items->len ? gtk_widget_get_width (GTK_WIDGET (self)) / (double) self->items->len : 0;
}

/* ---- drawing -------------------------------------------------------------------------------------------------- */

/* notification_badge: a 20 dp-radius pill in colorPrimary with a 2 dp ring of the bar's colour, the count in
 * the middle; top|end of the indicator's frame, so its right edge is the pill's */
static void
snapshot_badge (DialerNavBar *self, Item *item, GtkSnapshot *snapshot, double pill_right)
{
  TextStyle style = LABEL_STYLE;
  double text_w = text_width (item->badge), text_h = text_box_height (item->badge, style);
  double w = MAX (BADGE_MIN, text_w + 2 * BADGE_PAD_X), h = MAX (BADGE_MIN, text_h + BADGE_PAD_BOTTOM);
  graphene_rect_t outer = GRAPHENE_RECT_INIT (pill_right - w, BADGE_TOP, w, h), inner;
  GskRoundedRect rounded;
  GdkRGBA ring = text_rgb (BACKGROUND), fill = text_rgb (BADGE_FILL), color = text_rgb (BADGE_TEXT);

  gsk_rounded_rect_init_from_rect (&rounded, &outer, MIN (h / 2, 20));
  gtk_snapshot_push_rounded_clip (snapshot, &rounded);
  gtk_snapshot_append_color (snapshot, &ring, &outer);
  gtk_snapshot_pop (snapshot);
  graphene_rect_inset_r (&outer, BADGE_STROKE, BADGE_STROKE, &inner);
  gsk_rounded_rect_init_from_rect (&rounded, &inner, MIN (inner.size.height / 2, 20 - BADGE_STROKE));
  gtk_snapshot_push_rounded_clip (snapshot, &rounded);
  gtk_snapshot_append_color (snapshot, &fill, &inner);
  gtk_snapshot_pop (snapshot);
  /* gravity center inside the padding (6 dp each side, 1 dp below) */
  text_draw (snapshot, item->badge, style, outer.origin.x + (w - text_w) / 2,
             outer.origin.y + (h - BADGE_PAD_BOTTOM - text_h) / 2, &color);
}

static void
dialer_nav_bar_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  DialerNavBar *self = DIALER_NAV_BAR (widget);
  double width = gtk_widget_get_width (widget), col = column (self);
  GdkRGBA background = text_rgb (BACKGROUND), pill = text_rgb (PILL);

  gtk_snapshot_append_color (snapshot, &background, &GRAPHENE_RECT_INIT (0, 0, width, gtk_widget_get_height (widget)));
  for (guint i = 0; i < self->items->len; i++)
    {
      Item *item = self->items->pdata[i];
      gboolean selected = (int) i == self->selected;
      double cx = col * i + col / 2;
      GdkRGBA icon = text_rgb (selected ? ICON_SELECTED : COLOR_ICON);
      GdkRGBA label = text_rgb (selected ? COLOR_TEXT_PRIMARY : COLOR_TEXT_SECONDARY);

      /* ActiveIndicatorTransform: scaleX 0.4 → 1 and alpha 0 → 1 with the progress */
      if (item->progress > 0)
        {
          double scale = 0.4 + 0.6 * item->progress, w = PILL_WIDTH * scale;
          graphene_rect_t rect = GRAPHENE_RECT_INIT (cx - w / 2, PILL_TOP, w, PILL_HEIGHT);
          graphene_size_t corner = GRAPHENE_SIZE_INIT (PILL_HEIGHT / 2.0 * scale, PILL_HEIGHT / 2.0);
          GskRoundedRect rounded;

          /* pill.xml's round ends, drawn at 64 dp and squeezed by the view's scaleX: they go elliptical */
          gsk_rounded_rect_init (&rounded, &rect, &corner, &corner, &corner, &corner);
          pill.alpha = item->progress;
          gtk_snapshot_push_rounded_clip (snapshot, &rounded);
          gtk_snapshot_append_color (snapshot, &pill, &rect);
          gtk_snapshot_pop (snapshot);
        }
      neo_snapshot_icon (snapshot, widget, selected ? item->selected_icon : item->icon, cx - ICON / 2.0,
                         PILL_TOP + (PILL_HEIGHT - ICON) / 2.0, ICON, &icon);
      text_draw (snapshot, item->label, LABEL_STYLE, round (cx - text_width (item->label) / 2), LABEL_TOP, &label);
      if (item->count)
        snapshot_badge (self, item, snapshot, cx + PILL_WIDTH / 2.0);
    }
}

static void
dialer_nav_bar_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                        int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = orientation == GTK_ORIENTATION_VERTICAL ? BAR_HEIGHT : 0;
}

/* ---- selection ------------------------------------------------------------------------------------------------ */

static void
on_indicator (NeoAnim *anim, double value, gpointer data)
{
  Item *item = data;

  item->progress = value;
  gtk_widget_queue_draw (GTK_WIDGET (item->bar));
}

/* setSelected on every item: the new pill grows in, the old one shrinks out, both from where they are */
static void
select_item (DialerNavBar *self, int index, gboolean animate)
{
  self->selected = index;
  for (guint i = 0; i < self->items->len; i++)
    {
      Item *item = self->items->pdata[i];
      double to = (int) i == index;

      if (animate && gtk_widget_get_mapped (GTK_WIDGET (self)))
        neo_anim_start (item->indicator, item->progress, to, INDICATOR_MS, 0, SPONGY_FAST_OUT_SLOW_IN);
      else
        {
          neo_anim_stop (item->indicator);
          item->progress = to;
        }
    }
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static int
item_at (DialerNavBar *self, double x)
{
  double col = column (self);
  int index = col > 0 ? (int) (x / col) : -1;

  return index >= 0 && index < (int) self->items->len ? index : -1;
}

/* a plain click listener: the items have no ripple of their own */
static void
on_pressed (GtkGestureClick *click, int n_press, double x, double y, DialerNavBar *self)
{
  self->pressed = item_at (self, x);
}

static void
on_released (GtkGestureClick *click, int n_press, double x, double y, DialerNavBar *self)
{
  int index = self->pressed;

  self->pressed = -1;
  if (index < 0 || index != item_at (self, x) || index == self->selected)
    return;
  select_item (self, index, TRUE);
  g_signal_emit (self, signals[SELECTED], 0, index);
}

/* ---- GObject -------------------------------------------------------------------------------------------------- */

static void
dialer_nav_bar_dispose (GObject *object)
{
  DialerNavBar *self = DIALER_NAV_BAR (object);

  g_clear_pointer (&self->items, g_ptr_array_unref);
  G_OBJECT_CLASS (dialer_nav_bar_parent_class)->dispose (object);
}

static void
dialer_nav_bar_class_init (DialerNavBarClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = dialer_nav_bar_dispose;
  widget_class->measure = dialer_nav_bar_measure;
  widget_class->snapshot = dialer_nav_bar_snapshot;
  gtk_widget_class_set_css_name (widget_class, "dialer-nav-bar");
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_TAB_LIST);
  signals[SELECTED] = g_signal_new ("selected", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                    G_TYPE_NONE, 1, G_TYPE_INT);
}

static void
dialer_nav_bar_init (DialerNavBar *self)
{
  GtkGesture *click = gtk_gesture_click_new ();

  self->items = g_ptr_array_new_with_free_func (item_free);
  self->selected = -1;
  self->pressed = -1;
  g_signal_connect (click, "pressed", G_CALLBACK (on_pressed), self);
  g_signal_connect (click, "released", G_CALLBACK (on_released), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
}

GtkWidget *
dialer_nav_bar_new (void)
{
  return g_object_new (DIALER_TYPE_NAV_BAR, NULL);
}

void
dialer_nav_bar_append (DialerNavBar *self, const char *icon, const char *selected_icon, const char *label)
{
  Item *item = g_new0 (Item, 1);

  item->bar = self;
  item->icon = g_strdup (icon);
  item->selected_icon = g_strdup (selected_icon);
  item->label = text_layout (GTK_WIDGET (self), LABEL_STYLE, label, 0);
  item->indicator = neo_anim_new (GTK_WIDGET (self), on_indicator, item);
  g_ptr_array_add (self->items, item);
  if (self->selected < 0)
    select_item (self, 0, FALSE);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
dialer_nav_bar_set_selected (DialerNavBar *self, int index)
{
  select_item (self, index, FALSE);
}

int
dialer_nav_bar_get_selected (DialerNavBar *self)
{
  return self->selected;
}

void
dialer_nav_bar_set_count (DialerNavBar *self, int index, guint count)
{
  Item *item = self->items->pdata[index];

  if (count == item->count)
    return;
  item->count = count;
  g_clear_object (&item->badge);
  if (count)
    {
      g_autofree char *text = count > 9 ? g_strdup (C_("bottom_nav_count_9_plus", "9+"))
                                        : g_strdup_printf ("%u", count);

      item->badge = text_layout (GTK_WIDGET (self), LABEL_STYLE, text, 0);
    }
  gtk_widget_queue_draw (GTK_WIDGET (self));
}
