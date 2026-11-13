/* neo-messaging: the Messaging app, LineageOS's Messaging (Bugle) rebuilt in GTK 4 (spec/messaging.md).
 *
 * neo.Messaging, D-Bus activatable: the launcher starts it, an sms: or smsto: link opens the conversation with
 * that number (and the link's body in its field), and the SMS service's notifications open it at a conversation
 * ("open-conversation", its id) or at the list ("show-conversations").
 *
 * The messages themselves are not here: the SMS service (neo-messaging-sms) receives and sends them, keeps them in
 * its database and posts the notifications, whether or not this app runs. The app reads the database and asks the
 * service for every change. */
#include "messaging-types.h"
#include "messaging-window.h"

#include <glib/gi18n.h>
#include <locale.h>
#include <string.h>
#include <neo.h>

static void
load_style (void)
{
  GtkCssProvider *provider = gtk_css_provider_new ();

  gtk_css_provider_load_from_resource (provider, "/neo/Messaging/messaging.css");
  /* over libneo's, which is at APPLICATION priority too: the later provider wins at equal priority */
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  g_object_unref (provider);
}

static void
on_startup (GApplication *app, gpointer data)
{
  neo_init ();
  load_style ();
}

static GtkWidget *
messaging_window (GtkApplication *app)
{
  GtkWindow *window = gtk_application_get_active_window (app);

  return window ? GTK_WIDGET (window) : messaging_window_new (app);
}

static void
on_activate (GApplication *app, gpointer data)
{
  gtk_window_present (GTK_WINDOW (messaging_window (GTK_APPLICATION (app))));
}

/* "sms:+49170123,+49171456?body=Hi": the numbers, and the text for the field (Intent.ACTION_SENDTO) */
static void
open_uri (GtkWidget *window, const char *uri)
{
  const char *rest = strchr (uri, ':') + 1;
  const char *query = strchr (rest, '?');
  g_autofree char *path = g_uri_unescape_segment (rest, query, NULL);
  g_autofree char *body = NULL;
  g_auto(GStrv) numbers = NULL;
  g_autoptr(GStrvBuilder) builder = g_strv_builder_new ();

  if (query)
    {
      g_autoptr(GHashTable) params = g_uri_parse_params (query + 1, -1, "&", G_URI_PARAMS_NONE, NULL);
      const char *text = params ? g_hash_table_lookup (params, "body") : NULL;

      body = g_strdup (text);
    }
  numbers = g_strsplit_set (path ? path : "", ",;", -1);
  for (guint i = 0; numbers[i]; i++)
    if (*g_strstrip (numbers[i]))
      g_strv_builder_add (builder, numbers[i]);
  g_strfreev (numbers);
  numbers = g_strv_builder_end (builder);
  if (numbers[0])
    messaging_window_open_numbers (window, (const char * const *) numbers, body);
}

static void
on_open (GApplication *app, GFile **files, int n_files, const char *hint, gpointer data)
{
  GtkWidget *window = messaging_window (GTK_APPLICATION (app));

  for (int i = 0; i < n_files; i++)
    {
      g_autofree char *uri = g_file_get_uri (files[i]);

      if (g_str_has_prefix (uri, "sms:") || g_str_has_prefix (uri, "smsto:"))
        {
          open_uri (window, uri);
          break;
        }
    }
  gtk_window_present (GTK_WINDOW (window));
}

/* a tap on the notification for one conversation */
static void
on_open_conversation (GSimpleAction *action, GVariant *parameter, gpointer data)
{
  GtkWidget *window = messaging_window (GTK_APPLICATION (data));

  messaging_window_open_conversation (window, g_variant_get_int64 (parameter), NULL);
  gtk_window_present (GTK_WINDOW (window));
}

/* a tap on the notification for several */
static void
on_show_conversations (GSimpleAction *action, GVariant *parameter, gpointer data)
{
  GtkWidget *window = messaging_window (GTK_APPLICATION (data));

  messaging_window_show_conversations (window);
  gtk_window_present (GTK_WINDOW (window));
}

int
main (int argc, char **argv)
{
  g_autoptr(GtkApplication) app = NULL;
  static const GActionEntry actions[] = {
    { .name = "open-conversation", .activate = on_open_conversation, .parameter_type = "x" },
    { .name = "show-conversations", .activate = on_show_conversations },
  };

  setlocale (LC_ALL, "");
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  app = gtk_application_new (MESSAGING_APP_ID, G_APPLICATION_HANDLES_OPEN);
  g_signal_connect (app, "startup", G_CALLBACK (on_startup), NULL);
  g_action_map_add_action_entries (G_ACTION_MAP (app), actions, G_N_ELEMENTS (actions), app);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  g_signal_connect (app, "open", G_CALLBACK (on_open), NULL);
  return g_application_run (G_APPLICATION (app), argc, argv);
}
