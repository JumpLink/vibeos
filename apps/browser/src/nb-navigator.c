/* The browser's pages and the transitions between them (nb-navigator.h, a copy of Clock's clock-navigator.c). */
#include "nb-navigator.h"

#include <neo.h>

/* the open and close transitions: a slide, and a short fade a little after it starts */
#define SLIDE 96                /* px */
#define SLIDE_MS 450            /* on the emphasized curve */
#define FADE_MS 83
#define OPEN_FADE_DELAY 50
#define CLOSE_FADE_DELAY 35

struct _NbNavigator {
  GtkWidget parent;
  GPtrArray *pages;             /* bottom first */
  GtkWidget *leaving;           /* a page on its way out (closed or replaced), drawn until the slide ends */
  NeoAnim *anim;
  double progress;              /* 0 -> 1 through the transition */
  gboolean closing;             /* the transition is a close (back), else an open */
};

G_DEFINE_FINAL_TYPE (NbNavigator, nb_navigator, GTK_TYPE_WIDGET)

enum { TOP_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

GtkWidget *
nb_navigator_get_top (NbNavigator *self)
{
  return self->pages->len ? self->pages->pdata[self->pages->len - 1] : NULL;
}

guint
nb_navigator_get_depth (NbNavigator *self)
{
  return self->pages->len;
}

static GtkWidget *
under_top (NbNavigator *self)
{
  return self->pages->len > 1 ? self->pages->pdata[self->pages->len - 2] : NULL;
}

/* the fade, as the anim files have it: 83 ms after a short delay, linear, over the 450 ms slide */
static double
fade (double progress, guint delay)
{
  double ms = progress * SLIDE_MS - delay;

  return CLAMP (ms / FADE_MS, 0, 1);
}

static void
finish_leaving (NbNavigator *self)
{
  if (!self->leaving)
    return;
  gtk_widget_unparent (self->leaving);
  g_clear_object (&self->leaving);
}

/* only the top page (and in a transition the one it covers or the one leaving) is drawn and gets input */
static void
sync_visibility (NbNavigator *self)
{
  GtkWidget *top = nb_navigator_get_top (self), *under = under_top (self);
  gboolean moving = neo_anim_is_running (self->anim);

  for (guint i = 0; i < self->pages->len; i++)
    {
      GtkWidget *page = self->pages->pdata[i];

      gtk_widget_set_child_visible (page, page == top || (moving && page == under && !self->leaving));
      gtk_widget_set_can_target (page, page == top);
    }
}

static void
on_anim (NeoAnim *anim, double value, gpointer data)
{
  NbNavigator *self = data;

  self->progress = value;
  if (!neo_anim_is_running (anim))
    {
      finish_leaving (self);
      sync_visibility (self);
    }
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

static void
start (NbNavigator *self, gboolean closing)
{
  self->closing = closing;
  self->progress = 0;
  sync_visibility (self);
  neo_anim_start (self->anim, 0, 1, SLIDE_MS, 0, SPONGY_EMPHASIZED);
  sync_visibility (self);
  if (nb_navigator_get_top (self))
    gtk_widget_grab_focus (nb_navigator_get_top (self));
  g_signal_emit (self, signals[TOP_CHANGED], 0);
}

void
nb_navigator_push (NbNavigator *self, GtkWidget *page)
{
  finish_leaving (self);
  neo_anim_finish (self->anim);
  g_ptr_array_add (self->pages, g_object_ref_sink (page));
  gtk_widget_set_parent (page, GTK_WIDGET (self));
  start (self, FALSE);
}

gboolean
nb_navigator_pop (NbNavigator *self)
{
  if (self->pages->len < 2)
    return FALSE;
  finish_leaving (self);
  neo_anim_finish (self->anim);
  self->leaving = g_ptr_array_steal_index (self->pages, self->pages->len - 1);
  start (self, TRUE);
  return TRUE;
}

void
nb_navigator_replace (NbNavigator *self, GtkWidget *page)
{
  finish_leaving (self);
  neo_anim_finish (self->anim);
  if (self->pages->len > 1)
    self->leaving = g_ptr_array_steal_index (self->pages, self->pages->len - 1);
  g_ptr_array_add (self->pages, g_object_ref_sink (page));
  gtk_widget_set_parent (page, GTK_WIDGET (self));
  start (self, FALSE);
}

void
nb_navigator_pop_to_root (NbNavigator *self)
{
  /* the ones in between go at once; the top one slides away */
  while (self->pages->len > 2)
    {
      GtkWidget *page = g_ptr_array_steal_index (self->pages, self->pages->len - 2);

      gtk_widget_unparent (page);
      g_object_unref (page);
    }
  nb_navigator_pop (self);
}

/* --- layout */

static void
nb_navigator_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                       int *min_baseline, int *nat_baseline)
{
  *min = *nat = 0;
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child; child = gtk_widget_get_next_sibling (child))
    {
      int child_min, child_nat;

      gtk_widget_measure (child, orientation, for_size, &child_min, &child_nat, NULL, NULL);
      *min = MAX (*min, child_min);
      *nat = MAX (*nat, child_nat);
    }
}

static void
place (GtkWidget *page, int width, int height, double x)
{
  gtk_widget_allocate (page, width, height, -1, gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (x, 0)));
}

static void
nb_navigator_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  NbNavigator *self = NB_NAVIGATOR (widget);
  GtkWidget *top = nb_navigator_get_top (self), *under = under_top (self);
  gboolean moving = neo_anim_is_running (self->anim);
  double t = self->progress;

  for (guint i = 0; i < self->pages->len; i++)
    {
      GtkWidget *page = self->pages->pdata[i];
      double x = 0;

      if (moving && page == top)
        /* open: comes in from the right; close: comes back from the left */
        x = self->closing ? -SLIDE * (1 - t) : SLIDE * (1 - t);
      else if (moving && page == under && !self->leaving)
        x = -SLIDE * t;
      place (page, width, height, x);
    }
  if (self->leaving)
    /* closed: goes off to the right; replaced: to the left, as the page under an open */
    place (self->leaving, width, height, self->closing ? SLIDE * t : -SLIDE * t);
}

static void
nb_navigator_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  NbNavigator *self = NB_NAVIGATOR (widget);
  GtkWidget *top = nb_navigator_get_top (self), *under = under_top (self);
  gboolean moving = neo_anim_is_running (self->anim);

  if (!moving)
    {
      if (top)
        gtk_widget_snapshot_child (widget, top, snapshot);
      return;
    }
  if (self->closing)
    {
      /* the page coming back is under the one leaving, which fades out */
      gtk_widget_snapshot_child (widget, top, snapshot);
      gtk_snapshot_push_opacity (snapshot, 1 - fade (self->progress, CLOSE_FADE_DELAY));
      gtk_widget_snapshot_child (widget, self->leaving, snapshot);
      gtk_snapshot_pop (snapshot);
      return;
    }
  /* an open: the page it covers (or the one it replaces) under the new one, which fades in */
  if (self->leaving)
    gtk_widget_snapshot_child (widget, self->leaving, snapshot);
  else if (under)
    gtk_widget_snapshot_child (widget, under, snapshot);
  gtk_snapshot_push_opacity (snapshot, fade (self->progress, OPEN_FADE_DELAY));
  gtk_widget_snapshot_child (widget, top, snapshot);
  gtk_snapshot_pop (snapshot);
}

static gboolean
nb_navigator_grab_focus (GtkWidget *widget)
{
  GtkWidget *top = nb_navigator_get_top (NB_NAVIGATOR (widget));

  return top ? gtk_widget_grab_focus (top) : FALSE;
}

static void
nb_navigator_dispose (GObject *object)
{
  NbNavigator *self = NB_NAVIGATOR (object);

  g_clear_pointer (&self->anim, neo_anim_free);
  finish_leaving (self);
  if (self->pages)
    {
      for (guint i = 0; i < self->pages->len; i++)
        gtk_widget_unparent (self->pages->pdata[i]);
      g_clear_pointer (&self->pages, g_ptr_array_unref);
    }
  G_OBJECT_CLASS (nb_navigator_parent_class)->dispose (object);
}

static void
nb_navigator_class_init (NbNavigatorClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = nb_navigator_dispose;
  widget_class->measure = nb_navigator_measure;
  widget_class->size_allocate = nb_navigator_size_allocate;
  widget_class->snapshot = nb_navigator_snapshot;
  widget_class->grab_focus = nb_navigator_grab_focus;
  gtk_widget_class_set_css_name (widget_class, "nb-navigator");
  signals[TOP_CHANGED] = g_signal_new ("top-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                       NULL, G_TYPE_NONE, 0);
}

static void
nb_navigator_init (NbNavigator *self)
{
  self->pages = g_ptr_array_new_with_free_func (g_object_unref);
  self->anim = neo_anim_new (GTK_WIDGET (self), on_anim, self);
  self->progress = 1;
  gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);
}

GtkWidget *
nb_navigator_new (void)
{
  return g_object_new (NB_TYPE_NAVIGATOR, NULL);
}
