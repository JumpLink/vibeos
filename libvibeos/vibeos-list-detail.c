/* VibeosListDetail: see vibeos-list-detail.h.
 *
 * Both stacks are always its children. In two panes the list stack holds the list (and whatever the list itself
 * opens) and the detail stack the detail pages; in one pane the detail stack is empty and hidden, and the detail
 * pages sit on the list stack over the list. A change between the two moves the pages across. */
#include "vibeos-list-detail.h"
#include "vibeos-layout.h"

#include <math.h>

#define LIST_RATIO 0.3636

struct _VibeosListDetail {
  GtkWidget        parent;
  VibeosPageStack *list, *detail;
  GtkWidget       *placeholder;
  VibeosLayout    *layout;           /* the window's, while it is in one */

  double           ratio;
  int              list_width;       /* or 0: by the ratio */
  int              min_split_width;  /* or 0: on a tablet */
  gboolean         split;
  gboolean         back_closes;
};

G_DEFINE_FINAL_TYPE (VibeosListDetail, vibeos_list_detail, GTK_TYPE_WIDGET)

enum { PROP_0, PROP_SPLIT, N_PROPS };
static GParamSpec *props[N_PROPS];

enum { DETAIL_CLOSED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
sync_placeholder (VibeosListDetail *self)
{
  gtk_widget_set_child_visible (GTK_WIDGET (self->detail), self->split);
  if (self->placeholder)
    gtk_widget_set_child_visible (self->placeholder,
                                  self->split && !vibeos_page_stack_get_depth (self->detail));
}

/* two panes or one, from the window's layout; the open pages move to the stack they now belong on */
static void
update_split (VibeosListDetail *self)
{
  gboolean split = FALSE;

  if (self->layout)
    split = self->min_split_width > 0 ? vibeos_layout_get_width (self->layout) >= self->min_split_width
                                      : vibeos_layout_is_tablet (self->layout);
  if (split == self->split)
    return;
  self->split = split;
  if (split)
    vibeos_page_stack_move_pages (self->list, 1, self->detail);
  else
    vibeos_page_stack_move_pages (self->detail, 0, self->list);
  /* a page in a pane moves the short way; a page over the whole window the long one */
  vibeos_page_stack_set_transition (self->list, split ? VIBEOS_PAGE_TRANSITION_SHARED_AXIS_PANE
                                                      : VIBEOS_PAGE_TRANSITION_SHARED_AXIS);
  sync_placeholder (self);
  gtk_widget_queue_resize (GTK_WIDGET (self));
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SPLIT]);
}

static void
on_layout_changed (VibeosLayout *layout, VibeosListDetail *self)
{
  update_split (self);
}

static void
on_detail_top_changed (VibeosPageStack *detail, VibeosListDetail *self)
{
  sync_placeholder (self);
}

/* ---- the pages ------------------------------------------------------------------------------------------------- */

void
vibeos_list_detail_set_list (VibeosListDetail *self, GtkWidget *list)
{
  g_return_if_fail (VIBEOS_IS_LIST_DETAIL (self));

  if (!vibeos_page_stack_get_depth (self->list))
    vibeos_page_stack_push (self->list, list);
  else
    {
      /* a new list under whatever is open over the old one */
      VibeosPageStack *open = g_object_ref_sink (VIBEOS_PAGE_STACK (vibeos_page_stack_new ()));

      vibeos_page_stack_move_pages (self->list, 1, open);
      vibeos_page_stack_clear (self->list);
      vibeos_page_stack_push (self->list, list);
      vibeos_page_stack_move_pages (open, 0, self->list);
      g_object_unref (open);
    }
}

void
vibeos_list_detail_set_placeholder (VibeosListDetail *self, GtkWidget *placeholder)
{
  g_return_if_fail (VIBEOS_IS_LIST_DETAIL (self));

  g_clear_pointer (&self->placeholder, gtk_widget_unparent);
  self->placeholder = placeholder;
  if (placeholder)
    gtk_widget_insert_before (placeholder, GTK_WIDGET (self), GTK_WIDGET (self->detail));
  sync_placeholder (self);
}

static void
show (VibeosListDetail *self, GtkWidget *page, GtkWidget *row)
{
  if (self->split)
    {
      /* another item: the detail pane starts again from its page */
      if (vibeos_page_stack_get_depth (self->detail))
        vibeos_page_stack_set_root (self->detail, page);
      else
        vibeos_page_stack_push (self->detail, page);
      return;
    }
  if (vibeos_page_stack_get_depth (self->list) > 1)
    vibeos_page_stack_pop_to_root (self->list);
  if (row)
    vibeos_page_stack_push_from (self->list, page, row);
  else
    vibeos_page_stack_push (self->list, page);
}

void
vibeos_list_detail_show (VibeosListDetail *self, GtkWidget *page)
{
  g_return_if_fail (VIBEOS_IS_LIST_DETAIL (self));
  show (self, page, NULL);
}

void
vibeos_list_detail_show_from (VibeosListDetail *self, GtkWidget *page, GtkWidget *row)
{
  g_return_if_fail (VIBEOS_IS_LIST_DETAIL (self));
  show (self, page, row);
}

void
vibeos_list_detail_push (VibeosListDetail *self, GtkWidget *page)
{
  g_return_if_fail (VIBEOS_IS_LIST_DETAIL (self));
  vibeos_page_stack_push (self->split ? self->detail : self->list, page);
}

gboolean
vibeos_list_detail_has_detail (VibeosListDetail *self)
{
  return self->split ? vibeos_page_stack_get_depth (self->detail) > 0
                     : vibeos_page_stack_get_depth (self->list) > 1;
}

void
vibeos_list_detail_close_detail (VibeosListDetail *self)
{
  g_return_if_fail (VIBEOS_IS_LIST_DETAIL (self));

  if (!vibeos_list_detail_has_detail (self))
    return;
  if (self->split)
    vibeos_page_stack_clear (self->detail);
  else
    vibeos_page_stack_pop_to_root (self->list);
  g_signal_emit (self, signals[DETAIL_CLOSED], 0);
}

gboolean
vibeos_list_detail_back (VibeosListDetail *self)
{
  guint depth;

  g_return_val_if_fail (VIBEOS_IS_LIST_DETAIL (self), FALSE);

  if (!self->split)
    {
      depth = vibeos_page_stack_get_depth (self->list);
      if (depth < 2)
        return FALSE;
      vibeos_page_stack_pop (self->list);
      if (depth == 2)
        g_signal_emit (self, signals[DETAIL_CLOSED], 0);
      return TRUE;
    }
  depth = vibeos_page_stack_get_depth (self->detail);
  if (depth > 1)
    return vibeos_page_stack_pop (self->detail);
  if (depth == 1)
    {
      if (self->back_closes)
        return FALSE;
      vibeos_list_detail_close_detail (self);
      return TRUE;
    }
  /* nothing in the detail: Back goes on in the list pane */
  return vibeos_page_stack_pop (self->list);
}

/* ---- settings -------------------------------------------------------------------------------------------------- */

gboolean
vibeos_list_detail_is_split (VibeosListDetail *self)
{
  return self->split;
}

void
vibeos_list_detail_set_list_ratio (VibeosListDetail *self, double ratio)
{
  self->ratio = CLAMP (ratio, 0, 1);
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

void
vibeos_list_detail_set_list_width (VibeosListDetail *self, int width)
{
  self->list_width = MAX (width, 0);
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

void
vibeos_list_detail_set_min_split_width (VibeosListDetail *self, int width)
{
  self->min_split_width = MAX (width, 0);
  update_split (self);
}

void
vibeos_list_detail_set_back_closes (VibeosListDetail *self, gboolean closes)
{
  self->back_closes = closes;
}

VibeosPageStack *
vibeos_list_detail_get_list_stack (VibeosListDetail *self)
{
  return self->list;
}

VibeosPageStack *
vibeos_list_detail_get_detail_stack (VibeosListDetail *self)
{
  return self->detail;
}

GtkWidget *
vibeos_list_detail_get_detail_pane (VibeosListDetail *self)
{
  return self->split ? GTK_WIDGET (self->detail) : GTK_WIDGET (self);
}

/* ---- layout ---------------------------------------------------------------------------------------------------- */

static int
list_pane_width (VibeosListDetail *self, int width)
{
  int list = self->list_width > 0 ? self->list_width : (int) round (width * self->ratio);

  return CLAMP (list, 0, width);
}

static void
vibeos_list_detail_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                            int *min_baseline, int *nat_baseline)
{
  *min = *nat = 0;
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child; child = gtk_widget_get_next_sibling (child))
    {
      int child_min, child_nat;

      if (!gtk_widget_get_child_visible (child))
        continue;
      gtk_widget_measure (child, orientation, -1, &child_min, &child_nat, NULL, NULL);
      *min = MAX (*min, child_min);
      *nat = MAX (*nat, child_nat);
    }
}

static void
vibeos_list_detail_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  VibeosListDetail *self = VIBEOS_LIST_DETAIL (widget);
  int list = self->split ? list_pane_width (self, width) : width;
  GskTransform *right = gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (list, 0));

  gtk_widget_allocate (GTK_WIDGET (self->list), list, height, -1, NULL);
  if (self->placeholder)
    gtk_widget_allocate (self->placeholder, width - list, height, -1, gsk_transform_ref (right));
  gtk_widget_allocate (GTK_WIDGET (self->detail), width - list, height, -1, right);
}

/* ---- GObject --------------------------------------------------------------------------------------------------- */

static void
vibeos_list_detail_root (GtkWidget *widget)
{
  VibeosListDetail *self = VIBEOS_LIST_DETAIL (widget);

  GTK_WIDGET_CLASS (vibeos_list_detail_parent_class)->root (widget);
  self->layout = vibeos_layout_get (widget);
  if (self->layout)
    {
      g_object_ref (self->layout);
      g_signal_connect (self->layout, "changed", G_CALLBACK (on_layout_changed), self);
    }
  update_split (self);
}

static void
vibeos_list_detail_unroot (GtkWidget *widget)
{
  VibeosListDetail *self = VIBEOS_LIST_DETAIL (widget);

  if (self->layout)
    {
      g_signal_handlers_disconnect_by_data (self->layout, self);
      g_clear_object (&self->layout);
    }
  GTK_WIDGET_CLASS (vibeos_list_detail_parent_class)->unroot (widget);
}

static void
vibeos_list_detail_dispose (GObject *object)
{
  VibeosListDetail *self = VIBEOS_LIST_DETAIL (object);

  g_clear_pointer (&self->placeholder, gtk_widget_unparent);
  if (self->list)
    {
      gtk_widget_unparent (GTK_WIDGET (self->list));
      gtk_widget_unparent (GTK_WIDGET (self->detail));
      self->list = self->detail = NULL;
    }
  G_OBJECT_CLASS (vibeos_list_detail_parent_class)->dispose (object);
}

static void
vibeos_list_detail_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  VibeosListDetail *self = VIBEOS_LIST_DETAIL (object);

  switch (prop_id)
    {
    case PROP_SPLIT:
      g_value_set_boolean (value, self->split);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
vibeos_list_detail_class_init (VibeosListDetailClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = vibeos_list_detail_dispose;
  object_class->get_property = vibeos_list_detail_get_property;
  widget_class->measure = vibeos_list_detail_measure;
  widget_class->size_allocate = vibeos_list_detail_size_allocate;
  widget_class->root = vibeos_list_detail_root;
  widget_class->unroot = vibeos_list_detail_unroot;
  gtk_widget_class_set_css_name (widget_class, "vibeos-list-detail");

  props[PROP_SPLIT] = g_param_spec_boolean ("split", NULL, NULL, FALSE,
                                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  g_object_class_install_properties (object_class, N_PROPS, props);

  signals[DETAIL_CLOSED] = g_signal_new ("detail-closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                         NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
vibeos_list_detail_init (VibeosListDetail *self)
{
  self->ratio = LIST_RATIO;
  self->list = VIBEOS_PAGE_STACK (vibeos_page_stack_new ());
  self->detail = VIBEOS_PAGE_STACK (vibeos_page_stack_new ());
  gtk_widget_add_css_class (GTK_WIDGET (self->list), "vibeos-list-pane");
  gtk_widget_add_css_class (GTK_WIDGET (self->detail), "vibeos-detail-pane");
  vibeos_page_stack_set_transition (self->detail, VIBEOS_PAGE_TRANSITION_SHARED_AXIS_PANE);
  gtk_widget_set_parent (GTK_WIDGET (self->list), GTK_WIDGET (self));
  gtk_widget_set_parent (GTK_WIDGET (self->detail), GTK_WIDGET (self));
  g_signal_connect (self->detail, "top-changed", G_CALLBACK (on_detail_top_changed), self);
  sync_placeholder (self);
}

GtkWidget *
vibeos_list_detail_new (void)
{
  return g_object_new (VIBEOS_TYPE_LIST_DETAIL, NULL);
}
