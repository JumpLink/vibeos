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
dictate_access_prompt (GSettings *settings)
{
  g_autoptr (GPtrArray) on = g_ptr_array_new (), off = g_ptr_array_new ();
  GString *out = g_string_new (NULL);

  for (guint i = 0; i < G_N_ELEMENTS (ACCESS); i++)
    g_ptr_array_add (g_settings_get_boolean (settings, ACCESS[i].key) ? on : off, (gpointer) ACCESS[i].named);
  if (on->len)
    {
      g_string_append (out, off->len ? "You have tools for these things on the phone: "
                                     : "You have tools for everything on the phone: ");
      append_list (out, on);
      g_string_append (out, ". When the user asks you to do one of them, do it with your tools and confirm it in one "
                            "short sentence, as done, for example: ");
      /* the example is one of the things it can do */
      g_string_append (out, g_settings_get_boolean (settings, "access-alarms")
                            ? "\"Okay, your alarm is set for seven.\"" : "\"Okay, that's done.\"");
      g_string_append (out, " If you need a detail to do it, such as a time or a name, ask for it first.");
    }
  if (off->len)
    {
      g_string_append (out, on->len ? " You have no access to " : "You have no access to anything on the phone: ");
      append_list (out, off);
      g_string_append (out, ", because the user has turned that off in your settings. When the user asks for "
                            "something that needs one of them, do not do it and do not pretend to have done it: "
                            "say that you have no access to it, and that it can be turned on in Agent's settings.");
    }
  return g_string_free (out, FALSE);
}
