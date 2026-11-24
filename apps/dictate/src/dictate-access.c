/* dictate-access: see dictate-access.h. The prompt is the model's, in English whatever the user speaks. */
#include "dictate-access.h"

#include <glib/gi18n.h>

static const DictateAccess ACCESS[] = {
  { "access-calls", N_("Phone calls"), "calls" },
  { "access-messages", N_("Messages"), "messages" },
  { "access-contacts", N_("Contacts"), "contacts" },
  { "access-calendar", N_("Calendar"), "the calendar" },
  { "access-alarms", N_("Alarms and timers"), "alarms and timers" },
  { "access-settings", N_("Settings"), "the phone's settings" },
  { "access-apps", N_("Apps"), "apps" },
  { "access-files", N_("Files"), "files" },
  { "access-camera", N_("Camera"), "the camera" },
  { "access-location", N_("Location and maps"), "maps, navigation and the phone's location" },
  { "access-web", N_("Web"), "the web" },
};

const DictateAccess *
dictate_access_list (guint *n)
{
  *n = G_N_ELEMENTS (ACCESS);
  return ACCESS;
}

/* "a", "a and b", "a, b, and c" (the prompt's commas) */
static void
append_list (GString *out, GPtrArray *names)
{
  for (guint i = 0; i < names->len; i++)
    {
      if (i > 0)
        g_string_append (out, i < names->len - 1 ? ", " : names->len == 2 ? " and " : ", and ");
      g_string_append (out, names->pdata[i]);
    }
}

char *
dictate_access_prompt (GSettings *settings, DictateAccessHasTools has_tools)
{
  g_autoptr (GPtrArray) usable = g_ptr_array_new (), off = g_ptr_array_new ();
  GString *out = g_string_new (NULL);

  for (guint i = 0; i < G_N_ELEMENTS (ACCESS); i++)
    if (!g_settings_get_boolean (settings, ACCESS[i].key))
      g_ptr_array_add (off, (gpointer) ACCESS[i].named);
    else if (has_tools && has_tools (ACCESS[i].key))
      g_ptr_array_add (usable, (gpointer) ACCESS[i].named);

  if (!has_tools)
    {
      /* the model on the phone has no tools */
      g_string_append (out, "You cannot use anything on this phone yet. When the user asks you to do or to look up "
                            "something on the phone, such as a call, a message, an alarm or a setting, say plainly "
                            "that you can't do that yet, and never pretend to have done it.");
      return g_string_free (out, FALSE);
    }
  g_string_append (out, "You can look things up and do things on this phone with your tools: the time, the battery "
                        "and the storage");
  if (usable->len)
    {
      g_string_append (out, ", and ");
      append_list (out, usable);
    }
  g_string_append (out, ". Use a tool whenever the answer depends on the phone or on the user's data, and never "
                        "guess or make up such data: names, numbers, times, messages and settings come only from a "
                        "tool's result. When you tell the user what a message says, say it in your own words, in the "
                        "language you answer in, unless they ask for the exact words. If a detail you need is "
                        "missing, such as whom or when, ask for it in one short question; \"him\", \"her\" or "
                        "\"them\" when nobody has been named yet is such a missing detail: ask whom they mean, and "
                        "do not pick someone yourself. Calls and messages are only prepared: your tool shows the "
                        "user a card, and nothing happens until the user taps its button, so never say that a call "
                        "was made or a message was sent; say that it is ready for them to confirm. When a tool has "
                        "done something, confirm it in one short sentence, for example: \"Okay, your alarm is set "
                        "for seven.\" If you are asked for something on the phone that none of your tools can do, "
                        "say plainly that you can't do that yet.");
  if (off->len)
    {
      g_string_append (out, " You have no access to ");
      append_list (out, off);
      g_string_append (out, ", because the user has turned that off in your settings. When the user asks for "
                            "something that needs one of them, do not do it and do not pretend to: say that you "
                            "have no access to it, and that it can be turned on in Agent's settings.");
    }
  return g_string_free (out, FALSE);
}
