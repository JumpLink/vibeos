/* Call recordings' files (call-recording.h). */
#include "call-recording.h"

#include <glib/gi18n.h>
#include <string.h>

#define PARTY_MAX       100     /* bytes of the name's other-side part: a file name holds 255 */
#define UNKNOWN         "unknown"       /* Lineage's, for a call with no number */
#define SOUNDS_DIR      "vibeos-dialer/sounds"
#define FALLBACK        "en"

char *
call_recording_default_folder (void)
{
  return g_build_filename (g_get_home_dir (), "Recordings", "Call recordings", NULL);
}

char *
call_recording_folder (GSettings *settings)
{
  g_autofree char *folder = g_settings_get_string (settings, "recordings-folder");

  /* only a whole path: a relative one would depend on where the service was started */
  if (!folder[0] || !g_path_is_absolute (folder))
    return call_recording_default_folder ();
  return g_steal_pointer (&folder);
}

char *
call_recording_folder_display (const char *folder)
{
  const char *home = g_get_home_dir ();
  size_t len = strlen (home);

  /* the home folder as Files calls it */
  if (len > 1 && g_str_has_prefix (folder, home) && (folder[len] == '/' || !folder[len]))
    /* TRANSLATORS: the home folder, the start of a folder's path ("Home/Recordings"), as Files names it */
    return g_strconcat (C_("folder", "Home"), folder + len, NULL);
  return g_strdup (folder);
}

/* what a file name may not hold: the path separator, what FAT and exFAT memory cards refuse, control characters,
 * and the invisible format characters (a right-to-left override would make a name read as another) */
static gboolean
is_unfit (gunichar c)
{
  return (c < 0x80 && strchr ("/\\:*?\"<>|", (int) c)) || g_unichar_iscntrl (c)
         || g_unichar_type (c) == G_UNICODE_FORMAT;
}

/* no leading dots or spaces (a hidden file, or a name that starts blank), no trailing ones (FAT drops them) */
static void
trim (GString *name)
{
  gsize start = 0;

  while (start < name->len && (name->str[start] == '.' || name->str[start] == ' '))
    start++;
  g_string_erase (name, 0, start);
  while (name->len && (name->str[name->len - 1] == '.' || name->str[name->len - 1] == ' '))
    g_string_truncate (name, name->len - 1);
}

static char *
party_part (const char *party)
{
  g_autofree char *valid = g_utf8_make_valid (party ? party : "", -1);
  GString *name = g_string_new (NULL);

  for (const char *p = valid; *p; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (is_unfit (c))
        g_string_append_c (name, '_');
      else
        g_string_append_unichar (name, c);
    }
  trim (name);
  if (name->len > PARTY_MAX)
    {
      /* cut at a character's start */
      const char *end = g_utf8_find_prev_char (name->str, name->str + PARTY_MAX + 1);

      g_string_truncate (name, end ? (gsize) (end - name->str) : 0);
      trim (name);
    }
  if (!name->len)
    g_string_assign (name, UNKNOWN);
  return g_string_free (name, FALSE);
}

char *
call_recording_file_name (const char *party, GDateTime *when)
{
  g_autofree char *name = party_part (party);
  g_autofree char *stamp = g_date_time_format (when, "%y%m%d_%H%M%S");

  return g_strdup_printf ("%s_%s%03d%s", name, stamp, g_date_time_get_microsecond (when) / 1000,
                          CALL_RECORDING_EXTENSION);
}

static char *
announcement_in (const char *language, const char * const *data_dirs)
{
  g_autofree char *name = g_strdup_printf ("call-recording-%s.opus", language);

  for (guint i = 0; data_dirs[i]; i++)
    {
      g_autofree char *path = g_build_filename (data_dirs[i], SOUNDS_DIR, name, NULL);

      if (g_file_test (path, G_FILE_TEST_IS_REGULAR))
        return g_steal_pointer (&path);
    }
  return NULL;
}

char *
call_recording_announcement (const char * const *languages, const char * const *data_dirs)
{
  for (guint i = 0; languages[i]; i++)
    {
      /* the language alone: "pt_BR.UTF-8@euro" is "pt"; "C" and "POSIX" are none */
      g_autofree char *code = g_strndup (languages[i], strcspn (languages[i], "_.@"));
      char *path;

      if (!*code || g_str_equal (code, "C") || g_str_equal (code, "POSIX"))
        continue;
      if ((path = announcement_in (code, data_dirs)))
        return path;
    }
  return announcement_in (FALLBACK, data_dirs);
}
