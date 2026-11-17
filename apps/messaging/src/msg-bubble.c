/* A message's bubble (msg-bubble.h). */
#include "msg-bubble.h"

#include <math.h>

#define ROUND 20                /* a corner out in the open */
#define SMALL 4                 /* a corner towards a neighbour in the run */
#define MIN_HEIGHT 40           /* a one-line message's */

struct _MsgBubble {
  GtkWidget parent;
  GtkWidget *child;
  GdkRGBA color;
  int max_width;
  MsgRunPosition position;
  gboolean outgoing;
};

G_DEFINE_FINAL_TYPE (MsgBubble, msg_bubble, GTK_TYPE_WIDGET)

void
msg_bubble_set_color (MsgBubble *self, const GdkRGBA *color)
{
  self->color = *color;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
msg_bubble_set_max_width (MsgBubble *self, int max_width)
{
  if (self->max_width == max_width)
    return;
  self->max_width = max_width;
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

void
msg_bubble_set_run (MsgBubble *self, MsgRunPosition position, gboolean outgoing)
{
  self->position = position;
  self->outgoing = outgoing;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
msg_bubble_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                    int *min_baseline, int *nat_baseline)
{
  MsgBubble *self = MSG_BUBBLE (widget);

  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    {
      int child_min, child_nat;

      gtk_widget_measure (self->child, orientation, for_size, &child_min, &child_nat, NULL, NULL);
      if (self->max_width > 0)
        child_nat = MIN (child_nat, self->max_width);
      *min = MIN (child_min, child_nat);
      *nat = MAX (*min, child_nat);
    }
  else
    {
      /* the bubble sits at its own width at the side of a wider column, which measures it for the column's width:
       * its height is the one for the width it will have */
      if (for_size >= 0)
        {
          int width_min, width_nat;

          msg_bubble_measure (widget, GTK_ORIENTATION_HORIZONTAL, -1, &width_min, &width_nat, NULL, NULL);
          for_size = MIN (for_size, width_nat);
        }
      gtk_widget_measure (self->child, orientation, for_size, min, nat, NULL, NULL);
      *min = MAX (*min, MIN_HEIGHT);
      *nat = MAX (*nat, MIN_HEIGHT);
    }
}

/* the text wraps: its height follows the width it gets */
static GtkSizeRequestMode
msg_bubble_get_request_mode (GtkWidget *widget)
{
  return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static void
msg_bubble_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  int min, nat;

  /* a short text centred in a one-line bubble */
  gtk_widget_measure (MSG_BUBBLE (widget)->child, GTK_ORIENTATION_VERTICAL, width, &min, &nat, NULL, NULL);
  nat = MIN (MAX (nat, min), height);
  gtk_widget_allocate (MSG_BUBBLE (widget)->child, width, nat, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (0, floor ((height - nat) / 2.0))));
}

static void
msg_bubble_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  MsgBubble *self = MSG_BUBBLE (widget);
  gboolean small_top = self->position == MSG_RUN_MIDDLE || self->position == MSG_RUN_LAST;
  gboolean small_bottom = self->position == MSG_RUN_FIRST || self->position == MSG_RUN_MIDDLE;
  double top = small_top ? SMALL : ROUND, bottom = small_bottom ? SMALL : ROUND;
  GskRoundedRect shape;
  graphene_rect_t box = GRAPHENE_RECT_INIT (0, 0, gtk_widget_get_width (widget), gtk_widget_get_height (widget));
  /* the small corners on the side the run's bubbles line up on */
  graphene_size_t tl = { self->outgoing ? ROUND : top, self->outgoing ? ROUND : top };
  graphene_size_t tr = { self->outgoing ? top : ROUND, self->outgoing ? top : ROUND };
  graphene_size_t br = { self->outgoing ? bottom : ROUND, self->outgoing ? bottom : ROUND };
  graphene_size_t bl = { self->outgoing ? ROUND : bottom, self->outgoing ? ROUND : bottom };

  gsk_rounded_rect_init (&shape, &box, &tl, &tr, &br, &bl);
  gtk_snapshot_push_rounded_clip (snapshot, &shape);
  gtk_snapshot_append_color (snapshot, &self->color, &box);
  gtk_snapshot_pop (snapshot);
  gtk_widget_snapshot_child (widget, self->child, snapshot);
}

static void
msg_bubble_dispose (GObject *object)
{
  g_clear_pointer (&MSG_BUBBLE (object)->child, gtk_widget_unparent);
  G_OBJECT_CLASS (msg_bubble_parent_class)->dispose (object);
}

static void
msg_bubble_class_init (MsgBubbleClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = msg_bubble_dispose;
  widget_class->get_request_mode = msg_bubble_get_request_mode;
  widget_class->measure = msg_bubble_measure;
  widget_class->size_allocate = msg_bubble_size_allocate;
  widget_class->snapshot = msg_bubble_snapshot;
  gtk_widget_class_set_css_name (widget_class, "msg-bubble");
}

static void
msg_bubble_init (MsgBubble *self)
{
}

GtkWidget *
msg_bubble_new (GtkWidget *child)
{
  MsgBubble *self = g_object_new (MSG_TYPE_BUBBLE, NULL);

  self->child = child;
  gtk_widget_set_parent (child, GTK_WIDGET (self));
  return GTK_WIDGET (self);
}
