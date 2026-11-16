/* The in-call window: see incall-window.h. Which screen shows, what Back does, and the window's needs: over the lock
 * screen (which the desktop file asks of the shell), and the screen kept on while the answer screen shows. */
#include "incall-window.h"
#include "answer-view.h"
#include "call-types.h"
#include "caller.h"
#include "calls-client.h"
#include "contacts.h"
#include "incall-view.h"
#include "phone.h"

#include <glib/gi18n.h>
#include <vibeos.h>

static struct
{
  GtkApplication *app;
  GtkWidget *window, *stack, *answer, *incall, *snackbar;
  Contacts *contacts;
  guint inhibit;                /* the screen stays on while the answer screen shows */
  gboolean volume_keys;         /* the shell passes the volume keys on to us */
  gboolean locked;
  guint locked_signal;
  guint post_dial_call;         /* the call whose "Send the following tones?" is up */
} incall;

static gboolean
is_ringing (const CallInfo *call)
{
  return call->state == CALL_STATE_INCOMING || call->state == CALL_STATE_CALL_WAITING;
}

static gboolean
answer_shown (void)
{
  return incall.stack && gtk_stack_get_visible_child (GTK_STACK (incall.stack)) == incall.answer;
}

/* ---- what the window asks of the shell and the service while it is in front ---------------------------------- */

/* The volume keys are the shell's: it passes them on as the "volume-key" action to the app in front that asked
 * for them (extension/service.js TakeVolumeKeys). A ringing call takes them, as the phone's ringer stops on them. */
static void
take_volume_keys (gboolean take)
{
  GApplication *app = G_APPLICATION (incall.app);
  GDBusConnection *bus = g_application_get_dbus_connection (app);

  if (take == incall.volume_keys)
    return;
  incall.volume_keys = take;
  if (bus)
    g_dbus_connection_call (bus, "vibeos.Shell", "/vibeos/Shell", "vibeos.Shell",
                            "TakeVolumeKeys", g_variant_new ("(sb)", g_application_get_application_id (app), take),
                            NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

/* the answer screen keeps the screen on and takes the volume keys while it is in front; the window
 * in front tells the service, which keeps a ringing call's heads-up from peeking over it */
static void
sync_window_state (void)
{
  gboolean active = incall.window && gtk_window_is_active (GTK_WINDOW (incall.window));
  gboolean ringing = incall.window && answer_shown ();

  if (ringing && !incall.inhibit)
    incall.inhibit = gtk_application_inhibit (incall.app, GTK_WINDOW (incall.window), GTK_APPLICATION_INHIBIT_IDLE,
                                              _("A call is ringing"));
  else if (!ringing && incall.inhibit)
    {
      gtk_application_uninhibit (incall.app, incall.inhibit);
      incall.inhibit = 0;
    }
  take_volume_keys (active && ringing);
  calls_client_set_incall_visible (calls_client_get (), active);
}

void
incall_window_volume_key (int direction)
{
  /* a volume key while it rings silences it; the call goes on ringing on the screen */
  if (answer_shown ())
    calls_client_silence_ringer (calls_client_get ());
}

/* ---- the calls ------------------------------------------------------------------------------------------------ */

/* a call at a wait in its number asks before the tones after it go out. Yes sends them, No
 * (or Back, or a tap beside) drops them */
static void
on_post_dial_response (VibeosDialog *dialog, int response, gpointer data)
{
  guint id = GPOINTER_TO_UINT (data);
  CallInfo *call = calls_client_find (calls_client_get (), id);

  if (incall.post_dial_call == id)
    incall.post_dial_call = 0;
  /* the call may have ended, or moved on, meanwhile */
  if (call && *call->post_dial_wait)
    calls_client_continue_post_dial (calls_client_get (), id, response == VIBEOS_RESPONSE_OK);
}

static void
sync_post_dial (void)
{
  GPtrArray *calls = calls_client_get_calls (calls_client_get ());

  for (guint i = 0; i < calls->len; i++)
    {
      CallInfo *call = calls->pdata[i];
      g_autofree char *message = NULL;
      VibeosDialog *dialog;

      if (!*call->post_dial_wait || call->id == incall.post_dial_call)
        continue;
      incall.post_dial_call = call->id;
      message = g_strconcat (_("Send the following tones?\n"), call->post_dial_wait, NULL);
      dialog = vibeos_dialog_new (NULL);
      vibeos_dialog_set_message (dialog, message);
      vibeos_dialog_add_button (dialog, _("No"), VIBEOS_RESPONSE_CANCEL);
      vibeos_dialog_add_button (dialog, _("Yes"), VIBEOS_RESPONSE_OK);
      g_signal_connect (dialog, "response", G_CALLBACK (on_post_dial_response), GUINT_TO_POINTER (call->id));
      vibeos_dialog_present (dialog, incall.stack);
      break;
    }
}

static void
sync_calls (void)
{
  CallsClient *client = calls_client_get ();
  CallInfo *primary = calls_client_get_primary (client), *secondary;
  const char *country = calls_client_get_country (client);
  g_autoptr(Caller) caller = NULL, secondary_caller = NULL;

  if (!incall.window)
    return;
  /* no call left (the last one ended and its "Call ended" has been shown): the window goes */
  if (!primary)
    {
      gtk_window_destroy (GTK_WINDOW (incall.window));
      return;
    }
  caller = caller_new (incall.contacts, primary->number, primary->presentation, country);
  if (is_ringing (primary))
    {
      GPtrArray *calls = calls_client_get_calls (client);
      gboolean active = FALSE, held = FALSE;

      /* another call is up; the hint then says what answering does to it */
      for (guint i = 0; i < calls->len; i++)
        {
          CallInfo *call = calls->pdata[i];

          active |= call->state == CALL_STATE_ACTIVE;
          held |= call->state == CALL_STATE_ONHOLD;
        }
      answer_view_set_call (ANSWER_VIEW (incall.answer), primary, caller);
      answer_view_set_other_call (ANSWER_VIEW (incall.answer), active, calls_client_has_feature (client, "hold"),
                                  held);
      gtk_stack_set_visible_child (GTK_STACK (incall.stack), incall.answer);
    }
  else
    {
      secondary = calls_client_get_secondary (client, primary);
      if (secondary)
        secondary_caller = caller_new (incall.contacts, secondary->number, secondary->presentation, country);
      incall_view_set_calls (INCALL_VIEW (incall.incall), primary, caller, secondary, secondary_caller);
      gtk_stack_set_visible_child (GTK_STACK (incall.stack), incall.incall);
    }
  sync_post_dial ();
  sync_window_state ();
}

static void
on_changed (CallsClient *client, gpointer data)
{
  sync_calls ();
}

/* the service said no: a refused answer or decline brings the puck back, and what went wrong shows below */
static void
on_failed (CallsClient *client, const char *method, guint id, const char *message, gpointer data)
{
  if (!incall.window)
    return;
  if (answer_shown () && answer_view_get_call (ANSWER_VIEW (incall.answer)) == id
      && (g_str_equal (method, "Answer") || g_str_equal (method, "Decline")
          || g_str_equal (method, "AnswerAndRelease")))
    answer_view_reset (ANSWER_VIEW (incall.answer));
  if (message && *message)
    vibeos_snackbar_show (VIBEOS_SNACKBAR (incall.snackbar), message, NULL, NULL, NULL, VIBEOS_SNACKBAR_LONG);
}

/* the contacts came in after the call (the address books are read in the background): the name can change */
static void
on_contacts_changed (Contacts *contacts, gpointer data)
{
  sync_calls ();
}

/* ---- the lock screen ------------------------------------------------------------------------------------------ */

static void
set_locked (gboolean locked)
{
  incall.locked = locked;
  if (incall.incall)
    incall_view_set_locked (INCALL_VIEW (incall.incall), locked);
}

static void
on_screensaver_active (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, NULL);
  gboolean active = FALSE;

  if (reply)
    g_variant_get (reply, "(b)", &active);
  set_locked (active);
}

static void
on_screensaver_changed (GDBusConnection *bus, const char *sender, const char *path, const char *interface,
                        const char *signal, GVariant *parameters, gpointer data)
{
  gboolean active;

  g_variant_get (parameters, "(b)", &active);
  set_locked (active);
}

/* GNOME Shell's lock screen is its screensaver: active means locked */
static void
watch_lock (void)
{
  GDBusConnection *bus = g_application_get_dbus_connection (G_APPLICATION (incall.app));

  if (!bus || incall.locked_signal)
    return;
  incall.locked_signal = g_dbus_connection_signal_subscribe (bus, "org.gnome.ScreenSaver", "org.gnome.ScreenSaver",
                                                             "ActiveChanged", "/org/gnome/ScreenSaver", NULL,
                                                             G_DBUS_SIGNAL_FLAGS_NONE, on_screensaver_changed,
                                                             NULL, NULL);
  g_dbus_connection_call (bus, "org.gnome.ScreenSaver", "/org/gnome/ScreenSaver", "org.gnome.ScreenSaver",
                          "GetActive", NULL, G_VARIANT_TYPE ("(b)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                          on_screensaver_active, NULL);
}

/* ---- the window ----------------------------------------------------------------------------------------------- */

/* Add call: the Phone app opens for the second number; once it is dialled, this window comes back with both */
static void
on_add_call (InCallView *view, gpointer data)
{
  GDBusConnection *bus = g_application_get_dbus_connection (G_APPLICATION (incall.app));

  if (bus)
    phone_open_app (bus, DIALER_APP_ID, "add-call", NULL);
}

static void
on_notice (InCallView *view, const char *text, gpointer data)
{
  vibeos_snackbar_show (VIBEOS_SNACKBAR (incall.snackbar), text, NULL, NULL, NULL, VIBEOS_SNACKBAR_LONG);
}

/* Back closes the keypad first; a ringing call ignores it; otherwise the screen goes, the call stays */
static gboolean
on_close_request (GtkWindow *window, gpointer data)
{
  if (answer_shown ())
    return TRUE;
  if (incall_view_get_dialpad_shown (INCALL_VIEW (incall.incall)))
    {
      incall_view_show_dialpad (INCALL_VIEW (incall.incall), FALSE);
      return TRUE;
    }
  return FALSE;
}

static void
on_active (GtkWindow *window, GParamSpec *pspec, gpointer data)
{
  sync_window_state ();
}

static void
on_destroy (GtkWidget *window, gpointer data)
{
  CallsClient *client = calls_client_get ();

  take_volume_keys (FALSE);
  calls_client_set_incall_visible (client, FALSE);
  if (incall.inhibit)
    gtk_application_uninhibit (incall.app, incall.inhibit);
  incall.inhibit = 0;
  g_signal_handlers_disconnect_by_func (client, on_changed, NULL);
  g_signal_handlers_disconnect_by_func (client, on_failed, NULL);
  g_signal_handlers_disconnect_by_func (incall.contacts, on_contacts_changed, NULL);
  incall.window = incall.stack = incall.answer = incall.incall = incall.snackbar = NULL;
}

void
incall_window_present (GtkApplication *app)
{
  CallsClient *client = calls_client_get ();
  GtkOverlay *overlay;

  incall.app = app;
  if (!incall.window)
    {
      /* nothing to show: the app was started for a call that ended meanwhile */
      if (!calls_client_get_primary (client))
        return;
      if (!incall.contacts)
        incall.contacts = contacts_new ();
      incall.window = vibeos_window_new (app);
      gtk_window_set_title (GTK_WINDOW (incall.window), _("Phone"));
      gtk_widget_add_css_class (incall.window, "dialer");
      incall.stack = gtk_stack_new ();
      incall.answer = answer_view_new ();
      incall.incall = incall_view_new ();
      gtk_stack_add_child (GTK_STACK (incall.stack), incall.answer);
      gtk_stack_add_child (GTK_STACK (incall.stack), incall.incall);
      vibeos_window_set_content (GTK_WINDOW (incall.window), incall.stack);
      overlay = vibeos_window_get_overlay (incall.window);
      incall.snackbar = vibeos_snackbar_new ();
      gtk_overlay_add_overlay (overlay, incall.snackbar);
      incall_view_set_locked (INCALL_VIEW (incall.incall), incall.locked);
      g_signal_connect (incall.incall, "add-call", G_CALLBACK (on_add_call), NULL);
      g_signal_connect (incall.incall, "notice", G_CALLBACK (on_notice), NULL);
      g_signal_connect (incall.window, "close-request", G_CALLBACK (on_close_request), NULL);
      g_signal_connect (incall.window, "notify::is-active", G_CALLBACK (on_active), NULL);
      g_signal_connect (incall.window, "destroy", G_CALLBACK (on_destroy), NULL);
      g_signal_connect (client, "changed", G_CALLBACK (on_changed), NULL);
      g_signal_connect (client, "failed", G_CALLBACK (on_failed), NULL);
      g_signal_connect (incall.contacts, "changed", G_CALLBACK (on_contacts_changed), NULL);
      watch_lock ();
      sync_calls ();
      if (!incall.window)
        return;
    }
  gtk_window_present (GTK_WINDOW (incall.window));
}
