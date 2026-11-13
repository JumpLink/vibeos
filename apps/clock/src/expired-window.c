/* The expired-timers window: see expired-window.h. There is only ever one, so its state is the file's. */
#include "expired-window.h"
#include "elapsed.h"
#include "timer-card.h"
#include "timers-client.h"

#include <glib/gi18n.h>
#include <neo.h>
#include <spongy-gtk.h>

#define CARD_MARGIN  16
#define CARD_GAP     8
#define UPDATE_MS    100        /* between updates of the time */
#define LOAD_MS      10000      /* how long to wait for the service before giving up */

static struct {
  GtkApplication *app;
  gboolean listening;           /* the client's "changed" is connected, for the rest of the process */
  guint loading;                /* waiting for the timers, the application held */
  gboolean asked;               /* ... to show the window then */
  GHashTable *raised;           /* the ringing timers the window was opened or raised for */
  GtkWidget *window, *list;
  guint update, inhibit;
} expired;

static GPtrArray *
ringing_timers (void)
{
  GPtrArray *timers = timers_client_get_timers (timers_client_get ());
  GPtrArray *ringing = g_ptr_array_new ();

  for (guint i = 0; timers && i < timers->len; i++)
    if (((Timer *) timers->pdata[i])->state == TIMER_EXPIRED)
      g_ptr_array_add (ringing, timers->pdata[i]);
  return ringing;
}

void
expired_window_stop_all (void)
{
  timers_client_reset_expired (timers_client_get ());
}

void
expired_window_volume_key (void)
{
  if (expired.window)
    expired_window_stop_all ();
}

/* --- the window */

static gboolean
on_update (gpointer data)
{
  gint64 start = elapsed_now ();
  TimersClient *client = timers_client_get ();

  for (GtkWidget *card = gtk_widget_get_first_child (expired.list); card; card = gtk_widget_get_next_sibling (card))
    {
      Timer *timer = timers_client_find (client, timer_card_get_id (TIMER_CARD (card)));

      if (timer)
        timer_card_update (TIMER_CARD (card), timer, start);
    }
  expired.update = g_timeout_add (MAX (0, start + UPDATE_MS - elapsed_now ()), on_update, NULL);
  return G_SOURCE_REMOVE;
}

/* volume, mute and camera stop every ringing timer, on the key's release; the press is taken too */
static gboolean
stops_timers (guint keyval)
{
  return keyval == GDK_KEY_AudioRaiseVolume || keyval == GDK_KEY_AudioLowerVolume || keyval == GDK_KEY_AudioMute
         || keyval == GDK_KEY_WebCam;
}

static gboolean
on_key_pressed (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, gpointer data)
{
  return stops_timers (keyval);
}

static void
on_key_released (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, gpointer data)
{
  if (stops_timers (keyval))
    expired_window_stop_all ();
}

/* The volume keys are the shell's: it passes them on as the "volume-key" action to the app in front that asked for them
 * (extension/service.js TakeVolumeKeys), so the window asks while it has the focus. */
static void
take_volume_keys (gboolean take)
{
  GApplication *app = G_APPLICATION (expired.app);
  GDBusConnection *bus = g_application_get_dbus_connection (app);

  if (bus)
    g_dbus_connection_call (bus, "neo.GnomePhone", "/neo/GnomePhone", "neo.GnomePhone",
                            "TakeVolumeKeys", g_variant_new ("(sb)", g_application_get_application_id (app), take),
                            NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void
on_active (GtkWindow *window, GParamSpec *pspec, gpointer data)
{
  take_volume_keys (gtk_window_is_active (window));
}

static void
on_destroy (GtkWidget *window, gpointer data)
{
  if (gtk_window_is_active (GTK_WINDOW (window)))
    take_volume_keys (FALSE);
  g_clear_handle_id (&expired.update, g_source_remove);
  if (expired.inhibit)
    gtk_application_uninhibit (expired.app, expired.inhibit);
  expired.inhibit = 0;
  expired.window = expired.list = NULL;
}

static void
open_window (void)
{
  GtkWidget *scroller = spongy_scroll_view_new ();
  GtkEventController *keys = gtk_event_controller_key_new ();

  expired.window = neo_window_new (expired.app);
  gtk_window_set_title (GTK_WINDOW (expired.window), _("Timer"));
  expired.list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroller), expired.list);
  neo_window_set_content (GTK_WINDOW (expired.window), scroller);
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key_pressed), NULL);
  g_signal_connect (keys, "key-released", G_CALLBACK (on_key_released), NULL);
  gtk_widget_add_controller (expired.window, keys);
  /* KEEP_SCREEN_ON */
  expired.inhibit = gtk_application_inhibit (expired.app, GTK_WINDOW (expired.window), GTK_APPLICATION_INHIBIT_IDLE,
                                             _("A timer is ringing"));
  g_signal_connect (expired.window, "notify::is-active", G_CALLBACK (on_active), NULL);
  g_signal_connect (expired.window, "destroy", G_CALLBACK (on_destroy), NULL);
  on_update (NULL);
}

/* the cards for the ringing timers, the ones there kept; a single one in the middle of the screen, more from the top */
static void
sync_cards (GPtrArray *ringing)
{
  g_autoptr(GHashTable) cards = g_hash_table_new_full (NULL, NULL, NULL, g_object_unref);
  gint64 now = elapsed_now ();
  GtkWidget *card;

  while ((card = gtk_widget_get_first_child (expired.list)))
    {
      g_hash_table_insert (cards, GUINT_TO_POINTER (timer_card_get_id (TIMER_CARD (card))), g_object_ref (card));
      gtk_box_remove (GTK_BOX (expired.list), card);
    }
  for (guint i = 0; i < ringing->len; i++)
    {
      Timer *timer = ringing->pdata[i];

      card = g_hash_table_lookup (cards, GUINT_TO_POINTER (timer->id));
      if (!card)
        {
          card = timer_card_new (timer->id, TRUE);
          gtk_widget_set_margin_start (card, CARD_MARGIN);
          gtk_widget_set_margin_end (card, CARD_MARGIN);
          gtk_widget_set_margin_top (card, CARD_GAP);
          gtk_widget_set_margin_bottom (card, CARD_GAP);
        }
      gtk_widget_set_vexpand (card, ringing->len == 1);
      gtk_widget_set_valign (card, ringing->len == 1 ? GTK_ALIGN_CENTER : GTK_ALIGN_START);
      gtk_box_append (GTK_BOX (expired.list), card);
      timer_card_update (TIMER_CARD (card), timer, now);
    }
}

/* the window as the timers are now: gone with the last ringing one, opened or raised for one that started ringing.
 * `asked`: the app was activated (the "Time's up" notification tapped, say), which opens a window closed with Back. */
static void
update_window (gboolean asked)
{
  g_autoptr(GPtrArray) ringing = ringing_timers ();
  gboolean new_one = FALSE;

  if (!expired.raised)
    expired.raised = g_hash_table_new (NULL, NULL);
  for (guint i = 0; i < ringing->len; i++)
    new_one |= !g_hash_table_contains (expired.raised, GUINT_TO_POINTER (((Timer *) ringing->pdata[i])->id));
  g_hash_table_remove_all (expired.raised);
  for (guint i = 0; i < ringing->len; i++)
    g_hash_table_add (expired.raised, GUINT_TO_POINTER (((Timer *) ringing->pdata[i])->id));

  if (!ringing->len)
    {
      if (expired.window)
        gtk_window_destroy (GTK_WINDOW (expired.window));
      return;
    }
  if (!expired.window)
    {
      /* closed with Back while they ring: it comes back with the next one, or when asked */
      if (!new_one && !asked)
        return;
      open_window ();
      new_one = TRUE;
    }
  sync_cards (ringing);
  if (new_one)
    gtk_window_present (GTK_WINDOW (expired.window));
}

/* --- waiting for the timers */

static void
stop_loading (void)
{
  if (!expired.loading)
    return;
  g_clear_handle_id (&expired.loading, g_source_remove);
  g_application_release (G_APPLICATION (expired.app));
}

static gboolean
on_load_timeout (gpointer data)
{
  expired.loading = 0;
  expired.asked = FALSE;
  g_application_release (G_APPLICATION (expired.app));
  return G_SOURCE_REMOVE;
}

static void
on_changed (TimersClient *client, gpointer data)
{
  update_window (expired.asked);
  expired.asked = FALSE;
  stop_loading ();
}

void
expired_window_show (GtkApplication *app)
{
  TimersClient *client = timers_client_get ();

  expired.app = app;
  if (!expired.listening)
    {
      expired.listening = TRUE;
      g_signal_connect (client, "changed", G_CALLBACK (on_changed), NULL);
    }
  if (timers_client_get_timers (client))
    update_window (TRUE);
  else if (!expired.loading)
    {
      expired.asked = TRUE;
      /* the first answer comes as "changed"; with no window the application would end before it */
      g_application_hold (G_APPLICATION (app));
      expired.loading = g_timeout_add (LOAD_MS, on_load_timeout, NULL);
    }
}
