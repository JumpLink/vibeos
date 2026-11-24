/* vibeos-dictate: Dictate, speech to text on the phone itself; with --agent, Agent: a spoken conversation with a
 * language model on the phone, heard by Dictate's recognisers and answered in a voice (dictate-talk.h).
 *
 * A start button and a document: what is said appears in the document as it is said, recognised on the phone by
 * Moonshine or Vosk, by the language (dictate-engine.c), with nothing sent anywhere. The document is plain text,
 * edited as any other.
 *
 * Agent also watches the phone's calls (dictate-calls.h): one that rings is said aloud while Agent's window is open,
 * or always with Announce calls on, when Agent keeps running with no window (--background starts it so, with the
 * session, data/vibeos.Agent-announce.desktop).
 *
 * The shell has Agent listen from when the power key is held (its "hold" action: extension/keylaunch.js) until the
 * speaker pauses after it is let go, its window opened, or on a locked phone with no window, answering aloud and only
 * reading until the phone is unlocked. A hold the shell gives up ("hold-cancel": the key became part of a chord)
 * sends nothing. */
#include "dictate-calls.h"
#include "dictate-device.h"
#include "dictate-window.h"

#include <glib/gi18n.h>
#include <gst/gst.h>
#include <locale.h>
#include <string.h>
#include <vibeos.h>

#define APP_ID          "vibeos.Dictate"
#define AGENT_APP_ID    "vibeos.Agent"

static gboolean agent;
static gboolean background;     /* --background: Agent for Announce calls, no window until a call rings */
static DictateCallWatch *watch;
static GSettings *settings;
static gboolean held;           /* the app kept running with no window: Announce calls is on */

/* Agent's window, shown or not */
static GtkWidget *
agent_window (GtkApplication *app)
{
  GList *windows = gtk_application_get_windows (app);

  return windows ? windows->data : NULL;
}

static void
on_ringing (DictateCallWatch *w, guint id, GtkApplication *app)
{
  GtkWidget *window = agent_window (app);
  gboolean open = window && gtk_widget_get_visible (window);

  if (!dictate_calls_may_announce (settings, open))
    return;
  /* not open: a window that is never shown, to say it and to listen */
  if (!window)
    window = dictate_window_new (app, TRUE);
  dictate_window_announce_call (window, id);
}

static void
on_stopped (DictateCallWatch *w, guint id, GtkApplication *app)
{
  GtkWidget *window = agent_window (app);

  if (window)
    dictate_window_call_stopped (window, id);
}

static void
on_calls_changed (DictateCallWatch *w, GtkApplication *app)
{
  GtkWidget *window = agent_window (app);

  if (window)
    dictate_window_calls_changed (window, dictate_call_watch_calls (w));
}

/* Announce calls on: Agent stays when its window closes; off, it goes with it */
static void
on_announce_changed (GSettings *s, const char *key, GApplication *app)
{
  gboolean on = g_settings_get_boolean (s, "announce-calls");

  if (on && !held)
    g_application_hold (app);
  else if (!on && held)
    g_application_release (app);
  held = on;
}

static void
on_startup (GApplication *app, gpointer data)
{
  /* the text follows Font size and Bold text; the stylesheet in sp and dp, over libvibeos's */
  vibeos_init_full (VIBEOS_INIT_SCALED_TEXT);
  /* told to the Agent's gateway when the phone enrolls */
  dictate_device_set_release (vibeos_release ());
  vibeos_css_load_resource ("/vibeos/Dictate/dictate.css", GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  if (!agent)
    return;
  settings = g_settings_new ("vibeos.Dictate");
  g_signal_connect (settings, "changed::announce-calls", G_CALLBACK (on_announce_changed), app);
  on_announce_changed (settings, "announce-calls", app);
  watch = dictate_call_watch_new ();
  g_signal_connect (watch, "ringing", G_CALLBACK (on_ringing), app);
  g_signal_connect (watch, "stopped", G_CALLBACK (on_stopped), app);
  g_signal_connect (watch, "changed", G_CALLBACK (on_calls_changed), app);
}

static void
on_shutdown (GApplication *app, gpointer data)
{
  if (watch)
    g_signal_handlers_disconnect_by_data (watch, app);
  g_clear_object (&watch);
  if (settings)
    g_signal_handlers_disconnect_by_data (settings, app);
  g_clear_object (&settings);
}

static void
on_activate (GApplication *app, gpointer data)
{
  GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (app));

  /* started for Announce calls: no window yet */
  if (background)
    {
      background = FALSE;
      return;
    }
  if (!window && agent)
    window = GTK_WINDOW (agent_window (GTK_APPLICATION (app)));
  if (!window)
    window = GTK_WINDOW (dictate_window_new (GTK_APPLICATION (app), agent));
  /* opened as an app: only on an unlocked phone (the shell asks for the unlock first) */
  if (agent)
    dictate_window_unlocked (GTK_WIDGET (window));
  gtk_window_present (window);
}

/* "hold" (bb): the power key held (pressed) and let go, the phone locked or not (the shell's extension/keylaunch.js).
 * Held, Agent listens, and let go, listens on until the speaker pauses: its window opened on an unlocked phone (the
 * shell opens it too), none on a locked one, where Agent answers aloud and only reads */
static void
on_hold (GSimpleAction *action, GVariant *parameter, gpointer data)
{
  GtkApplication *app = data;
  GtkWidget *window = agent_window (app);
  gboolean pressed, locked;

  if (!agent)
    return;
  g_variant_get (parameter, "(bb)", &pressed, &locked);
  if (!window && !pressed)
    return;
  if (!window)
    window = dictate_window_new (app, TRUE);
  if (pressed && !locked)
    gtk_window_present (GTK_WINDOW (window));
  dictate_window_hold (window, pressed, locked);
}

/* "hold-cancel": the hold given up while the key is still down (power and volume up for the power menu): Agent
 * stops listening and sends nothing; no window is made for it */
static void
on_hold_cancel (GSimpleAction *action, GVariant *parameter, gpointer data)
{
  GtkWidget *window = agent_window (data);

  if (agent && window)
    dictate_window_hold_cancel (window);
}

/* "dictate": the FAB's start and stop, for other programs over D-Bus (org.gtk.Actions on /vibeos/Dictate) */
static void
on_dictate (GSimpleAction *action, GVariant *parameter, gpointer data)
{
  GtkApplication *app = data;
  GtkWindow *window = gtk_application_get_active_window (app);

  if (!window)
    window = GTK_WINDOW (dictate_window_new (app, agent));
  gtk_window_present (window);
  dictate_window_toggle_dictation (GTK_WIDGET (window));
}

int
main (int argc, char **argv)
{
  g_autoptr (GtkApplication) app = NULL;

  setlocale (LC_ALL, "");
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  /* Moonshine's ONNX Runtime on one thread: on both reference phones that recognises faster than on all eight
   * cores (0.22 against 0.32 s of work per second of speech on the S10e), and leaves the others idle. Set before
   * any thread starts. */
  g_setenv ("MOONSHINE_ORT_SINGLE_THREAD", "1", FALSE);
  gst_init (NULL, NULL);

  /* --agent: Agent, an app of its own (its id, its window, its icon); --background: Agent with no window, for
   * Announce calls. Both taken out before GApplication parses */
  for (int i = 1; i < argc;)
    if (g_str_equal (argv[i], "--agent") || g_str_equal (argv[i], "--background"))
      {
        if (g_str_equal (argv[i], "--agent"))
          agent = TRUE;
        else
          background = TRUE;
        memmove (argv + i, argv + i + 1, (argc - i) * sizeof *argv);
        argc--;
      }
    else
      i++;
  background = background && agent;
  app = gtk_application_new (agent ? AGENT_APP_ID : APP_ID, G_APPLICATION_DEFAULT_FLAGS);
  /* Agent's icons and stylesheet are Dictate's: GtkApplication finds icons under this path + /icons */
  g_application_set_resource_base_path (G_APPLICATION (app), "/vibeos/Dictate");
  {
    static const GActionEntry actions[] = {
      { .name = "dictate", .activate = on_dictate },
      { .name = "hold", .activate = on_hold, .parameter_type = "(bb)" },
      { .name = "hold-cancel", .activate = on_hold_cancel },
    };

    g_action_map_add_action_entries (G_ACTION_MAP (app), actions, G_N_ELEMENTS (actions), app);
  }
  g_signal_connect (app, "startup", G_CALLBACK (on_startup), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  g_signal_connect (app, "shutdown", G_CALLBACK (on_shutdown), NULL);
  /* --background while Agent runs already (the session starting it as the setting turned on): nothing to do, and
   * no window brought up */
  if (background)
    {
      g_autoptr (GError) error = NULL;

      if (!g_application_register (G_APPLICATION (app), NULL, &error))
        {
          g_warning ("%s", error->message);
          return 1;
        }
      if (g_application_get_is_remote (G_APPLICATION (app)))
        return 0;
    }
  return g_application_run (G_APPLICATION (app), argc, argv);
}
