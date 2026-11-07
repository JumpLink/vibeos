/* TimerPage: see timer-page.h. Measured on the reference phone: a card 16 dp in from the sides and 8 dp under the
 * top of the tab. */
#include "timer-page.h"
#include "clock-tab.h"
#include "elapsed.h"
#include "timer-card.h"
#include "timer-setup.h"
#include "timers-client.h"

#include <glib/gi18n.h>
#include <neo.h>
#include <spongy-gtk.h>

#define CARD_MARGIN  16         /* MaterialCardViewStyle: marginStart and marginEnd */
#define CARD_GAP     8          /* ... marginTop, and timer_item's marginBottom */
#define UPDATE_MS    20         /* TimeUpdateRunnable */
#define CHANGE_MS    400        /* MEDIUM: the slide; each fade takes half of it */

struct _TimerPage
{
  GtkWidget parent;
  TimersClient *client;

  GtkWidget *setup, *scroller, *list;
  GtkWidget *probe;             /* a card never shown, to know how tall a card's ring is before there is a card */
  GArray *ids;                  /* the timers the cards show, in their order */
  gboolean loaded;              /* the timers came: the tab knows which view to open on (onStart) */
  gboolean list_when_loaded;    /* a notification asked for the list before that */
  gboolean on_list;             /* the view on screen is the list (mCurrentView == mTimersView) */
  guint update;

  /* the change of view (animateToView) */
  NeoAnim *change;
  gboolean to_list, switched;
  double distance;              /* how far the old view slides; the new one comes from as far the other way */
  double old_y, new_y;
};

static void timer_page_tab_init (ClockTabInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (TimerPage, timer_page, GTK_TYPE_WIDGET,
                               G_IMPLEMENT_INTERFACE (CLOCK_TYPE_TAB, timer_page_tab_init))

static gboolean
has_timers (TimerPage *self)
{
  return self->ids->len > 0;
}

/* --- updating the cards */

static gboolean
on_update (gpointer data)
{
  TimerPage *self = data;
  gint64 start = elapsed_now ();
  gboolean running = FALSE;

  self->update = 0;
  for (GtkWidget *card = gtk_widget_get_first_child (self->list); card; card = gtk_widget_get_next_sibling (card))
    {
      Timer *timer = timers_client_find (self->client, timer_card_get_id (TIMER_CARD (card)));

      if (!timer)
        continue;
      timer_card_update (TIMER_CARD (card), timer, start);
      running |= timer->state == TIMER_RUNNING || timer->state == TIMER_EXPIRED || timer->state == TIMER_MISSED;
    }
  /* every 20 ms while a timer counts or blinks (DeskClock goes on with paused ones too, which never change; any
   * change of state comes through "changed") */
  if (running)
    self->update = g_timeout_add (MAX (0, start + UPDATE_MS - elapsed_now ()), on_update, self);
  return G_SOURCE_REMOVE;
}

static void
stop_updating (TimerPage *self)
{
  g_clear_handle_id (&self->update, g_source_remove);
}

static void
start_updating (TimerPage *self)
{
  stop_updating (self);
  if (self->on_list && gtk_widget_get_mapped (GTK_WIDGET (self)))
    on_update (self);
}

/* the cards in the client's order: the ones still there kept, new ones made, the others gone. `added` and
 * `removed` say whether any came or went (timerAdded, timerRemoved). */
static void
sync_cards (TimerPage *self, GPtrArray *timers, gboolean *added, gboolean *removed)
{
  g_autoptr(GHashTable) cards = g_hash_table_new_full (NULL, NULL, NULL, g_object_unref);
  g_autoptr(GHashTable) now = g_hash_table_new (NULL, NULL), before = g_hash_table_new (NULL, NULL);
  gboolean same = timers->len == self->ids->len;
  GtkWidget *card;

  *added = *removed = FALSE;
  for (guint i = 0; i < timers->len; i++)
    g_hash_table_add (now, GUINT_TO_POINTER (((Timer *) timers->pdata[i])->id));
  for (guint j = 0; j < self->ids->len; j++)
    {
      gpointer id = GUINT_TO_POINTER (g_array_index (self->ids, guint, j));

      g_hash_table_add (before, id);
      *removed |= !g_hash_table_contains (now, id);
    }
  for (guint i = 0; i < timers->len; i++)
    *added |= !g_hash_table_contains (before, GUINT_TO_POINTER (((Timer *) timers->pdata[i])->id));
  for (guint i = 0; same && i < timers->len; i++)
    same = g_array_index (self->ids, guint, i) == ((Timer *) timers->pdata[i])->id;
  if (same)
    return;

  while ((card = gtk_widget_get_first_child (self->list)))
    {
      g_hash_table_insert (cards, GUINT_TO_POINTER (timer_card_get_id (TIMER_CARD (card))), g_object_ref (card));
      gtk_box_remove (GTK_BOX (self->list), card);
    }
  g_array_set_size (self->ids, 0);
  for (guint i = 0; i < timers->len; i++)
    {
      guint id = ((Timer *) timers->pdata[i])->id;

      card = g_hash_table_lookup (cards, GUINT_TO_POINTER (id));
      if (!card)
        {
          card = timer_card_new (id, FALSE);
          gtk_widget_set_margin_start (card, CARD_MARGIN);
          gtk_widget_set_margin_end (card, CARD_MARGIN);
          gtk_widget_set_margin_top (card, CARD_GAP);
          gtk_widget_set_margin_bottom (card, CARD_GAP);
        }
      gtk_box_append (GTK_BOX (self->list), card);
      g_array_append_val (self->ids, id);
    }
}

/* --- the two views */

static void
apply_offsets (TimerPage *self)
{
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

/* one view or the other at once (showTimersView, showCreateTimerView) */
static void
show_view (TimerPage *self, gboolean list)
{
  neo_anim_stop (self->change);
  self->on_list = list;
  self->old_y = self->new_y = 0;
  gtk_widget_set_opacity (self->setup, 1);
  gtk_widget_set_opacity (self->scroller, 1);
  gtk_widget_set_child_visible (self->scroller, list);
  gtk_widget_set_child_visible (self->setup, !list);
  if (list)
    start_updating (self);
  else
    stop_updating (self);
  apply_offsets (self);
}

/* The old view slides `distance` along the standard curve and fades out over the first half; the new one slides in
 * from as far the other way and fades in over the second half. The buttons shrink with the fade-out; when it ends
 * the old view goes, and they grow back as the new view's. */
static void
on_change (NeoAnim *anim, double value, gpointer data)
{
  TimerPage *self = data;
  GtkWidget *old = self->to_list ? self->setup : self->scroller, *new = self->to_list ? self->scroller : self->setup;
  double slide = spongy_curve (SPONGY_FAST_OUT_SLOW_IN, value / CHANGE_MS);
  double half = CHANGE_MS / 2.0;

  self->old_y = self->distance * slide;
  self->new_y = -self->distance * (1 - slide);
  gtk_widget_set_opacity (old, 1 - spongy_curve (SPONGY_ACCELERATE_DECELERATE, CLAMP (value / half, 0, 1)));
  gtk_widget_set_opacity (new, spongy_curve (SPONGY_ACCELERATE_DECELERATE, CLAMP ((value - half) / half, 0, 1)));
  if (!self->switched && value >= half)
    {
      self->switched = TRUE;
      self->on_list = self->to_list;
      gtk_widget_set_child_visible (old, FALSE);
      if (self->to_list)
        {
          timer_setup_reset (TIMER_SETUP (self->setup));
          start_updating (self);
        }
      else
        stop_updating (self);
      clock_tab_update_buttons (CLOCK_TAB (self), CLOCK_ALL_IMMEDIATE | CLOCK_ALL_EXPAND);
    }
  if (!neo_anim_is_running (anim))
    {
      self->old_y = self->new_y = 0;
      gtk_widget_set_opacity (self->setup, 1);
      gtk_widget_set_opacity (self->scroller, 1);
    }
  apply_offsets (self);
}

/* animateToView: `down` for "+" and Start, up for Cancel and the last timer going */
static void
change_view (TimerPage *self, gboolean to_list, gboolean down)
{
  int width = gtk_widget_get_width (GTK_WIDGET (self)) - 2 * CARD_MARGIN;

  if (self->on_list == to_list || neo_anim_is_running (self->change))
    return;
  self->to_list = to_list;
  self->switched = FALSE;
  /* as far as a card's ring is tall; nothing to measure once the last card is gone */
  self->distance = to_list || has_timers (self) ? timer_card_get_ring_height (TIMER_CARD (self->probe), width) : 0;
  if (!down)
    self->distance = -self->distance;
  gtk_widget_set_child_visible (to_list ? self->scroller : self->setup, TRUE);
  gtk_widget_set_opacity (to_list ? self->scroller : self->setup, 0);
  clock_tab_update_buttons (CLOCK_TAB (self), CLOCK_ALL_SHRINK);
  neo_anim_start (self->change, 0, CHANGE_MS, CHANGE_MS, 0, SPONGY_LINEAR);
}

static gboolean
changing (TimerPage *self)
{
  return neo_anim_is_running (self->change);
}

/* --- the timers changing */

static void
on_changed (TimersClient *client, TimerPage *self)
{
  GPtrArray *timers = timers_client_get_timers (client);
  gboolean added, removed;

  if (!timers)
    return;
  sync_cards (self, timers, &added, &removed);
  if (!self->loaded)
    {
      /* onStart: the keypad when there are no timers, the list when there are */
      self->loaded = TRUE;
      show_view (self, self->list_when_loaded || has_timers (self));
      clock_tab_update_buttons (CLOCK_TAB (self), CLOCK_ALL_IMMEDIATE);
      return;
    }
  /* (TimerWatcher) a timer coming or going may change Cancel; not in the middle of a change of view, whose end sets
   * the buttons anyway. The list with its last timer gone gives way to the keypad. */
  if ((added || removed) && !changing (self))
    clock_tab_update_buttons (CLOCK_TAB (self), CLOCK_ALL_IMMEDIATE);
  if (removed && self->on_list && !has_timers (self))
    change_view (self, FALSE, FALSE);
  start_updating (self);
}

void
timer_page_show_timer (TimerPage *self, guint id)
{
  if (!self->loaded)
    {
      self->list_when_loaded = TRUE;
      return;
    }
  show_view (self, TRUE);
  clock_tab_update_buttons (CLOCK_TAB (self), CLOCK_ALL_IMMEDIATE);
}

/* --- the buttons */

static void
on_input_changed (TimerSetup *setup, TimerPage *self)
{
  clock_tab_update_buttons (CLOCK_TAB (self), CLOCK_FAB_SHRINK_AND_EXPAND);
}

static void
on_added (guint id, gpointer data)
{
  timers_client_start (timers_client_get (), id);
}

/* onUpdateFab, onUpdateFabButtons: on the list "+"; on the keypad Start once something is typed, and Cancel when
 * there are timers to go back to */
static void
timer_page_get_buttons (ClockTab *tab, ClockButton *buttons)
{
  TimerPage *self = TIMER_PAGE (tab);

  if (!self->loaded)
    return;
  if (self->on_list)
    {
      buttons[CLOCK_FAB] = (ClockButton) { "clock-add-symbolic", _("Add Timer") };
      return;
    }
  if (timer_setup_has_input (TIMER_SETUP (self->setup)))
    buttons[CLOCK_FAB] = (ClockButton) { "clock-pause-play-symbolic", _("Start") };
  if (has_timers (self))
    buttons[CLOCK_LEFT] = (ClockButton) { "clock-delete-symbolic", _("Cancel") };
}

/* getFabTargetVisibility: the keypad counts as having a FAB when it has input or there are timers */
static gboolean
timer_page_has_fab (ClockTab *tab)
{
  TimerPage *self = TIMER_PAGE (tab);

  if (!self->loaded)
    return FALSE;
  return self->on_list || timer_setup_has_input (TIMER_SETUP (self->setup)) || has_timers (self);
}

static void
timer_page_clicked (ClockTab *tab, ClockButtonPosition button)
{
  TimerPage *self = TIMER_PAGE (tab);
  TimerSetup *setup = TIMER_SETUP (self->setup);

  /* (BUTTONS_DISABLE) not while the view changes */
  if (changing (self) || !self->loaded)
    return;
  if (button == CLOCK_FAB && self->on_list)
    change_view (self, FALSE, TRUE);
  else if (button == CLOCK_FAB && timer_setup_has_input (setup))
    {
      /* a timer of that length, no label, started at once */
      timers_client_add (self->client, timer_setup_get_length (setup), "", on_added, NULL);
      change_view (self, TRUE, TRUE);
    }
  else if (button == CLOCK_LEFT && !self->on_list)
    {
      timer_setup_reset (setup);
      change_view (self, TRUE, FALSE);
    }
}

static gboolean
timer_page_key_pressed (ClockTab *tab, guint keyval)
{
  TimerPage *self = TIMER_PAGE (tab);

  return self->loaded && !self->on_list && !changing (self)
         && timer_setup_key_pressed (TIMER_SETUP (self->setup), keyval);
}

static void
timer_page_tab_init (ClockTabInterface *iface)
{
  iface->get_buttons = timer_page_get_buttons;
  iface->clicked = timer_page_clicked;
  iface->has_fab = timer_page_has_fab;
  iface->key_pressed = timer_page_key_pressed;
}

/* --- GtkWidget */

static void
place (GtkWidget *child, int width, int height, double y)
{
  gtk_widget_allocate (child, width, height, -1, gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (0, y)));
}

static void
timer_page_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  TimerPage *self = TIMER_PAGE (widget);
  gboolean moving = changing (self);
  /* during a change the old view is the one sliding by old_y */
  gboolean list_is_old = moving ? !self->to_list : FALSE;

  if (gtk_widget_get_child_visible (self->setup))
    place (self->setup, width, height, moving ? (list_is_old ? self->new_y : self->old_y) : 0);
  if (gtk_widget_get_child_visible (self->scroller))
    place (self->scroller, width, height, moving ? (list_is_old ? self->old_y : self->new_y) : 0);
}

static void
timer_page_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                    int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 0;
}

static void
timer_page_map (GtkWidget *widget)
{
  GTK_WIDGET_CLASS (timer_page_parent_class)->map (widget);
  start_updating (TIMER_PAGE (widget));
}

static void
timer_page_unmap (GtkWidget *widget)
{
  stop_updating (TIMER_PAGE (widget));
  GTK_WIDGET_CLASS (timer_page_parent_class)->unmap (widget);
}

static void
timer_page_dispose (GObject *object)
{
  TimerPage *self = TIMER_PAGE (object);

  stop_updating (self);
  if (self->client)
    g_signal_handlers_disconnect_by_data (self->client, self);
  g_clear_pointer (&self->change, neo_anim_free);
  g_clear_pointer (&self->setup, gtk_widget_unparent);
  g_clear_pointer (&self->scroller, gtk_widget_unparent);
  g_clear_pointer (&self->probe, gtk_widget_unparent);
  g_clear_pointer (&self->ids, g_array_unref);
  G_OBJECT_CLASS (timer_page_parent_class)->dispose (object);
}

static void
timer_page_class_init (TimerPageClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = timer_page_dispose;
  widget_class->measure = timer_page_measure;
  widget_class->size_allocate = timer_page_size_allocate;
  widget_class->map = timer_page_map;
  widget_class->unmap = timer_page_unmap;
  gtk_widget_class_set_css_name (widget_class, "timer-page");
}

static void
timer_page_init (TimerPage *self)
{
  self->ids = g_array_new (FALSE, FALSE, sizeof (guint));

  /* both views hidden until the timers come and say which one to open on */
  self->setup = timer_setup_new ();
  g_signal_connect (self->setup, "input-changed", G_CALLBACK (on_input_changed), self);
  gtk_widget_set_parent (self->setup, GTK_WIDGET (self));
  gtk_widget_set_child_visible (self->setup, FALSE);

  self->list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign (self->list, GTK_ALIGN_START);
  self->scroller = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (self->scroller), self->list);
  gtk_widget_set_parent (self->scroller, GTK_WIDGET (self));
  gtk_widget_set_child_visible (self->scroller, FALSE);

  self->probe = timer_card_new (0, FALSE);
  gtk_widget_set_parent (self->probe, GTK_WIDGET (self));
  gtk_widget_set_child_visible (self->probe, FALSE);

  self->change = neo_anim_new (GTK_WIDGET (self), on_change, self);
}

GtkWidget *
timer_page_new (void)
{
  TimerPage *self = g_object_new (TIMER_TYPE_PAGE, NULL);

  self->client = timers_client_get ();
  g_signal_connect (self->client, "changed", G_CALLBACK (on_changed), self);
  on_changed (self->client, self);
  return GTK_WIDGET (self);
}
