/* Formatting and name rules (fm-util.h). */
#include "fm-util.h"

#include <glib/gi18n.h>
#include <string.h>
#include <vibeos-locale.h>

char *
fm_format_size (guint64 size)
{
  /* "0 B" as File Manager+ writes it (g_format_size says "0 bytes"); the number and unit kept together */
  if (size < 1000)
    return g_strdup_printf ("%u\u00a0B", (guint) size);
  return g_format_size (size);
}

char *
fm_format_date (gint64 unix_time)
{
  g_autoptr(GDateTime) date = g_date_time_new_from_unix_local (unix_time);

  /* the language's short date: "11/24/26", "24.11.26" */
  return date ? vibeos_format_date (date, "yMd") : g_strdup ("");
}

char *
fm_format_date_time (gint64 unix_time)
{
  g_autoptr(GDateTime) date = g_date_time_new_from_unix_local (unix_time);

  /* with the time by the clock setting: "11/24/26, 2:05:09 PM" */
  return date ? vibeos_format_date (date, "yMdjms") : g_strdup ("");
}

char *
fm_format_items (int count)
{
  if (count < 0)
    return g_strdup (_("- items"));
  return g_strdup_printf (ngettext ("%d item", "%d items", count), count);
}

char *
fm_format_duration (guint seconds)
{
  if (seconds >= 3600)
    return g_strdup_printf ("%u:%02u:%02u", seconds / 3600, seconds / 60 % 60, seconds % 60);
  return g_strdup_printf ("%u:%02u", seconds / 60, seconds % 60);
}

char *
fm_format_permissions (guint32 mode)
{
  static const char letters[] = "rwxrwxrwx";
  char text[10];

  for (int i = 0; i < 9; i++)
    text[i] = mode & (1u << (8 - i)) ? letters[i] : '-';
  text[9] = 0;
  return g_strdup (text);
}

FmNameCheck
fm_check_name (const char *name)
{
  g_autofree char *stripped = g_strstrip (g_strdup (name ? name : ""));

  if (!*stripped)
    return FM_NAME_EMPTY;
  if (strchr (name, '/') || !strcmp (name, ".") || !strcmp (name, ".."))
    return FM_NAME_BAD_CHARACTER;
  return FM_NAME_OK;
}

const char *
fm_name_check_message (FmNameCheck check)
{
  switch (check)
    {
    case FM_NAME_EMPTY:
      return _("File name cannot be empty.");
    case FM_NAME_BAD_CHARACTER:
      /* only '/' is forbidden on Linux; File Manager+'s longer list of characters is the FAT file system's rule */
      return _("A filename cannot contain the character /");
    case FM_NAME_OK:
    default:
      return NULL;
    }
}

/* where a file's extension starts: the last '.', unless it is the first character (".bashrc") or the last */
static const char *
extension_of (const char *name, gboolean folder)
{
  const char *dot;

  if (folder)
    return NULL;
  dot = strrchr (name, '.');
  if (!dot || dot == name || !dot[1])
    return NULL;
  /* "archive.tar.gz" keeps ".tar.gz" together */
  if (!strcmp (dot, ".gz") || !strcmp (dot, ".bz2") || !strcmp (dot, ".xz") || !strcmp (dot, ".zst"))
    {
      const char *before = g_strrstr_len (name, dot - name, ".");

      if (before && before != name && !strncmp (before, ".tar", 4))
        return before;
    }
  return dot;
}

char *
fm_copy_name (const char *name, gboolean folder, guint n)
{
  const char *ext = extension_of (name, folder);
  g_autofree char *stem = ext ? g_strndup (name, ext - name) : g_strdup (name);

  if (n <= 1)
    return g_strdup_printf ("%s%s%s", stem, _(" - Copy"), ext ? ext : "");
  return g_strdup_printf ("%s%s (%u)%s", stem, _(" - Copy"), n, ext ? ext : "");
}

void
fm_stem_range (const char *name, gboolean folder, int *end)
{
  const char *ext = extension_of (name, folder);

  *end = (int) g_utf8_strlen (name, ext ? ext - name : -1);
}

/* a URI with the '/' at its end taken off (but not the root's) */
static char *
uri_without_slash (GFile *file)
{
  char *uri = g_file_get_uri (file);
  size_t len = strlen (uri);
  const char *authority_end = strstr (uri, "://");

  authority_end = authority_end ? strchr (authority_end + 3, '/') : NULL;
  while (len > 1 && uri[len - 1] == '/' && authority_end && uri + len - 1 > authority_end)
    uri[--len] = '\0';
  return uri;
}

char *
fm_relative_path (GFile *root, GFile *file)
{
  g_autofree char *root_uri = NULL, *file_uri = NULL, *prefix = NULL;
  size_t len;

  if (!root)
    return g_strdup ("/");
  if (g_file_is_native (root) && g_file_is_native (file))
    {
      g_autofree char *relative = g_file_get_relative_path (root, file);

      return g_strconcat ("/", relative ? relative : "", NULL);
    }
  /* by the URIs: GVfs keeps smb://host/ and smb://host/share/ apart as two mounts, so GFile says neither is under the
   * other */
  root_uri = uri_without_slash (root);
  file_uri = uri_without_slash (file);
  prefix = g_str_has_suffix (root_uri, "/") ? g_strdup (root_uri) : g_strconcat (root_uri, "/", NULL);
  len = strlen (prefix);
  if (strncmp (file_uri, prefix, len) == 0 && file_uri[len])
    {
      g_autofree char *unescaped = g_uri_unescape_string (file_uri + len, NULL);

      return g_strconcat ("/", unescaped ? unescaped : file_uri + len, NULL);
    }
  return g_strdup ("/");
}

GFile *
fm_parent (GFile *file)
{
  g_autofree char *uri = NULL;
  char *slash, *path;

  if (g_file_is_native (file))
    return g_file_get_parent (file);
  /* by the URI, for the same reason: smb://host/share/ has no parent to GVfs, but its server is it */
  uri = uri_without_slash (file);
  path = strstr (uri, "://");
  path = path ? strchr (path + 3, '/') : NULL;
  if (!path || !path[1])
    return NULL;
  slash = strrchr (path, '/');
  slash[1] = '\0';
  return g_file_new_for_uri (uri);
}

gboolean
fm_file_is_under (GFile *file, GFile *root)
{
  g_autofree char *relative = NULL;
  g_autofree char *root_uri = NULL, *file_uri = NULL;

  if (g_file_is_native (root) && g_file_is_native (file))
    return g_file_equal (file, root) || g_file_has_prefix (file, root);
  root_uri = uri_without_slash (root);
  file_uri = uri_without_slash (file);
  if (!strcmp (root_uri, file_uri))
    return TRUE;
  relative = fm_relative_path (root, file);
  return strcmp (relative, "/") != 0;
}

char *
fm_display_path (GFile *file)
{
  g_autofree char *uri = NULL;
  g_autoptr(GUri) parsed = NULL;

  if (g_file_is_native (file))
    {
      char *path = g_file_get_path (file);

      if (path)
        return path;
    }
  uri = g_file_get_uri (file);
  parsed = g_uri_parse (uri, G_URI_FLAGS_HAS_PASSWORD, NULL);
  if (parsed)
    {
      /* to_string_partial leaves the password out, and the path is shown unescaped */
      g_autofree char *safe = g_uri_to_string_partial (parsed, G_URI_HIDE_PASSWORD);
      char *shown = g_uri_unescape_string (safe, NULL);

      return shown ? shown : g_steal_pointer (&safe);
    }
  return g_steal_pointer (&uri);
}

gboolean
fm_name_matches (const char *name, const char *folded_needle)
{
  g_autofree char *folded = NULL;

  if (!folded_needle || !*folded_needle)
    return TRUE;
  folded = g_utf8_casefold (name, -1);
  return strstr (folded, folded_needle) != NULL;
}

void
fm_count_tree (GFile *file, GCancellable *cancellable, FmCount *count)
{
  g_autoptr(GFileInfo) info = g_file_query_info (file, G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, NULL);
  g_autoptr(GFileEnumerator) children = NULL;
  GFileInfo *child_info;
  GFile *child;

  if (!info)
    return;
  if (g_file_info_get_file_type (info) != G_FILE_TYPE_DIRECTORY)
    {
      count->files++;
      count->bytes += g_file_info_get_size (info);
      return;
    }
  count->folders++;
  children = g_file_enumerate_children (file, G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE ","
                                        G_FILE_ATTRIBUTE_STANDARD_SIZE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, NULL);
  while (children && !g_cancellable_is_cancelled (cancellable)
         && g_file_enumerator_iterate (children, &child_info, &child, cancellable, NULL) && child_info)
    {
      if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY)
        fm_count_tree (child, cancellable, count);
      else
        {
          count->files++;
          count->bytes += g_file_info_get_size (child_info);
        }
    }
}

gboolean
fm_info_is_hidden (GFileInfo *info)
{
  const char *name = g_file_info_get_name (info);
  const char *shown = g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME);
  GFileType type = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_STANDARD_TYPE);

  /* the attribute getters, not g_file_info_get_is_hidden, which is critical about a flag the backend left out */
  if (g_file_info_get_attribute_boolean (info, G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN) ||
      g_file_info_get_attribute_boolean (info, G_FILE_ATTRIBUTE_STANDARD_IS_BACKUP) || (name && name[0] == '.'))
    return TRUE;
  /* a server's hidden shares (IPC$, C$, ADMIN$): Windows' own convention, a '$' at the end */
  return (type == G_FILE_TYPE_MOUNTABLE || type == G_FILE_TYPE_SHORTCUT)
         && ((shown && g_str_has_suffix (shown, "$")) || (name && g_str_has_suffix (name, "$")));
}
