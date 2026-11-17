/* A .deb file's fields, name and icon (inst-deb.h).
 *
 * The data archive is read as dpkg-deb --fsys-tarfile streams it, whatever it is compressed with: a tar stream, of
 * which only the desktop files and the icons are kept, in memory and within limits. Nothing in it is written to disk
 * or followed outside the archive (its symlinks are resolved among its own members). */
#include "inst-deb.h"

#include <stdlib.h>
#include <string.h>

G_DEFINE_QUARK (inst-deb-error-quark, inst_deb_error)

#define DESKTOP_MAX      (256 * 1024)        /* a desktop file's size, at most */
#define DESKTOP_COUNT    64                  /* desktop files kept, at most */
#define ICON_MAX         (2 * 1024 * 1024)   /* an icon file's size, at most */
#define ICONS_TOTAL      (48 * 1024 * 1024)  /* all the icons kept, at most (an icon theme has thousands) */
#define HEADER_MAX       (1024 * 1024)       /* a long name's or a pax header's size, at most */
#define LINK_DEPTH       8

void
inst_deb_free (InstDeb *deb)
{
  if (!deb)
    return;
  g_free (deb->package);
  g_free (deb->version);
  g_free (deb->architecture);
  g_free (deb->maintainer);
  g_free (deb->homepage);
  g_free (deb->summary);
  g_free (deb->description);
  g_free (deb->app_name);
  g_free (deb->desktop_path);
  g_clear_pointer (&deb->icon, g_bytes_unref);
  g_free (deb->icon_type);
  g_free (deb);
}

/* ---- the control fields --------------------------------------------------------------------------------------- */

/* " ." (or an empty line): a paragraph break */
static gboolean
is_blank_line (const char *line)
{
  while (*line == ' ' || *line == '\t')
    line++;
  return !*line || (line[0] == '.' && !line[1]);
}

void
inst_deb_split_description (const char *field, char **summary, char **description)
{
  g_auto (GStrv) lines = g_strsplit (field ? field : "", "\n", -1);
  GString *text = g_string_new (NULL);
  gboolean paragraph_open = FALSE;

  *summary = g_strstrip (g_strdup (lines[0] ? lines[0] : ""));
  for (guint i = 1; lines[0] && lines[i]; i++)
    {
      const char *line = lines[i];

      /* each extended line starts with a space: one more, and it is shown as it is (a list, a table) */
      if (*line == ' ' || *line == '\t')
        line++;
      if (is_blank_line (line))
        {
          if (text->len)
            g_string_append (text, "\n\n");
          paragraph_open = FALSE;
          continue;
        }
      if (*line == ' ')
        {
          if (paragraph_open)
            g_string_append_c (text, '\n');
          g_string_append (text, line + 1);
          paragraph_open = TRUE;
          continue;
        }
      if (paragraph_open)
        g_string_append_c (text, ' ');
      g_string_append (text, line);
      paragraph_open = TRUE;
    }
  /* a trailing blank line */
  while (text->len && text->str[text->len - 1] == '\n')
    g_string_truncate (text, text->len - 1);
  *description = g_string_free (text, FALSE);
}

/* "Field: value" lines with continuation lines (starting with a space), as dpkg-deb --field prints the control file */
static GHashTable *
parse_control (const char *text)
{
  GHashTable *fields = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_auto (GStrv) lines = g_strsplit (text, "\n", -1);
  char *key = NULL;
  GString *value = NULL;

  for (guint i = 0;; i++)
    {
      const char *line = lines[i];
      const char *colon;

      if (line && (*line == ' ' || *line == '\t') && key)
        {
          g_string_append_c (value, '\n');
          g_string_append (value, line);
          continue;
        }
      if (key)
        {
          g_hash_table_replace (fields, key, g_strstrip (g_string_free (value, FALSE)));
          key = NULL;
        }
      if (!line)
        break;
      colon = strchr (line, ':');
      if (!colon || colon == line)
        continue;
      key = g_ascii_strdown (line, colon - line);
      value = g_string_new (colon + 1);
    }
  return fields;
}

InstDeb *
inst_deb_read_control (const char *path, GCancellable *cancellable, GError **error)
{
  g_autoptr (GSubprocess) proc = NULL;
  g_autofree char *out = NULL, *err = NULL;
  g_autoptr (GHashTable) fields = NULL;
  g_autoptr (GError) local = NULL;
  g_autoptr (GFile) file = g_file_new_for_path (path);
  g_autoptr (GFileInfo) info = NULL;
  InstDeb *deb;
  const char *size;

  proc = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &local,
                           "dpkg-deb", "--field", "--", path, NULL);
  if (!proc)
    {
      g_set_error (error, INST_DEB_ERROR, INST_DEB_ERROR_NO_DPKG, "dpkg-deb: %s", local->message);
      return NULL;
    }
  if (!g_subprocess_communicate_utf8 (proc, NULL, cancellable, &out, &err, error))
    return NULL;
  if (!g_subprocess_get_successful (proc))
    {
      g_set_error (error, INST_DEB_ERROR, INST_DEB_ERROR_NOT_A_PACKAGE, "%s",
                   err && *g_strstrip (err) ? err : "dpkg-deb cannot read it");
      return NULL;
    }
  fields = parse_control (out ? out : "");
  if (!g_hash_table_lookup (fields, "package") || !g_hash_table_lookup (fields, "version"))
    {
      g_set_error (error, INST_DEB_ERROR, INST_DEB_ERROR_NOT_A_PACKAGE, "It has no package name or version");
      return NULL;
    }

  deb = g_new0 (InstDeb, 1);
  deb->package = g_strdup (g_hash_table_lookup (fields, "package"));
  deb->version = g_strdup (g_hash_table_lookup (fields, "version"));
  deb->architecture = g_strdup (g_hash_table_lookup (fields, "architecture"));
  deb->maintainer = g_strdup (g_hash_table_lookup (fields, "maintainer"));
  deb->homepage = g_strdup (g_hash_table_lookup (fields, "homepage"));
  inst_deb_split_description (g_hash_table_lookup (fields, "description"), &deb->summary, &deb->description);
  size = g_hash_table_lookup (fields, "installed-size");
  deb->installed_size = size ? g_ascii_strtoull (size, NULL, 10) * 1024 : 0;
  info = g_file_query_info (file, G_FILE_ATTRIBUTE_STANDARD_SIZE, G_FILE_QUERY_INFO_NONE, cancellable, NULL);
  deb->file_size = info ? (guint64) g_file_info_get_size (info) : 0;
  return deb;
}

/* ---- the data archive: a tar stream --------------------------------------------------------------------------- */

typedef struct {
  GBytes *data;     /* a regular file's contents, or NULL (a link, or a file over the limits) */
  char   *link;     /* a symlink's or hard link's target, as a member name, or NULL */
} Member;

static void
member_free (gpointer data)
{
  Member *m = data;

  g_clear_pointer (&m->data, g_bytes_unref);
  g_free (m->link);
  g_free (m);
}

typedef struct {
  GInputStream *in;
  GCancellable *cancellable;
  GHashTable   *members;      /* name -> Member: the desktop files and the icons */
  guint         desktops;
  gsize         icon_bytes;
  const char   *want;         /* one more file to keep, whatever its name (the target of an icon's link) */
} Scan;

static gboolean
read_block (Scan *scan, void *buffer, gsize size, gboolean *eof, GError **error)
{
  gsize got = 0;

  if (!g_input_stream_read_all (scan->in, buffer, size, &got, scan->cancellable, error))
    return FALSE;
  if (got == 0 && eof)
    {
      *eof = TRUE;
      return TRUE;
    }
  if (got != size)
    {
      g_set_error (error, INST_DEB_ERROR, INST_DEB_ERROR_NOT_A_PACKAGE, "The package's files end too soon");
      return FALSE;
    }
  return TRUE;
}

static gboolean
skip (Scan *scan, guint64 size, GError **error)
{
  while (size > 0)
    {
      gssize n = g_input_stream_skip (scan->in, MIN (size, 1 << 20), scan->cancellable, error);

      if (n < 0)
        return FALSE;
      if (n == 0)
        {
          g_set_error (error, INST_DEB_ERROR, INST_DEB_ERROR_NOT_A_PACKAGE, "The package's files end too soon");
          return FALSE;
        }
      size -= n;
    }
  return TRUE;
}

/* a member's contents and the padding after it */
static GBytes *
read_data (Scan *scan, guint64 size, GError **error)
{
  guchar *buffer = g_malloc (size ? size : 1);

  if (!read_block (scan, buffer, size, NULL, error) || !skip (scan, (512 - size % 512) % 512, error))
    {
      g_free (buffer);
      return NULL;
    }
  return g_bytes_new_take (buffer, size);
}

/* a numeric header field: octal digits, or base-256 for a large size */
static guint64
number (const guchar *field, gsize length)
{
  guint64 value = 0;
  gsize i = 0;

  if (field[0] & 0x80)
    {
      value = field[0] & 0x7f;
      for (i = 1; i < length; i++)
        value = (value << 8) | field[i];
      return value;
    }
  while (i < length && (field[i] == ' ' || field[i] == '\0'))
    i++;
  for (; i < length && field[i] >= '0' && field[i] <= '7'; i++)
    value = value * 8 + (field[i] - '0');
  return value;
}

/* a member's name without "./" or "/" in front, as dpkg-deb writes it ("./usr/share/...") */
static char *
member_name (const char *name)
{
  while (g_str_has_prefix (name, "./") || *name == '/')
    name += *name == '/' ? 1 : 2;
  return g_strdup (name);
}

static gboolean
is_desktop (const char *name)
{
  return g_str_has_prefix (name, "usr/share/applications/") && g_str_has_suffix (name, ".desktop");
}

static const char *
icon_type (const char *name)
{
  if (g_str_has_suffix (name, ".png"))
    return "png";
  if (g_str_has_suffix (name, ".svg") || g_str_has_suffix (name, ".svgz"))
    return "svg";
  if (g_str_has_suffix (name, ".xpm"))
    return "xpm";
  return NULL;
}

static gboolean
is_icon (const char *name)
{
  return (g_str_has_prefix (name, "usr/share/icons/") || g_str_has_prefix (name, "usr/share/pixmaps/"))
         && icon_type (name) != NULL;
}

/* the records of a pax extended header ("LENGTH key=value\n"): the path, the link and the size it gives */
static void
parse_pax (GBytes *data, char **path, char **link, guint64 *size, gboolean *has_size)
{
  gsize length;
  const char *p = g_bytes_get_data (data, &length), *end = p + length;

  while (p < end)
    {
      char *after;
      guint64 record = g_ascii_strtoull (p, &after, 10);
      const char *eq, *stop = p + record;

      if (record == 0 || after == p || stop > end || *after != ' ')
        break;
      eq = memchr (after, '=', stop - after);
      if (eq)
        {
          g_autofree char *key = g_strndup (after + 1, eq - after - 1);
          g_autofree char *value = g_strndup (eq + 1, stop - eq - 2 >= 0 ? stop - eq - 2 : 0);

          if (!g_strcmp0 (key, "path"))
            {
              g_free (*path);
              *path = g_steal_pointer (&value);
            }
          else if (!g_strcmp0 (key, "linkpath"))
            {
              g_free (*link);
              *link = g_steal_pointer (&value);
            }
          else if (!g_strcmp0 (key, "size"))
            {
              *size = g_ascii_strtoull (value, NULL, 10);
              *has_size = TRUE;
            }
        }
      p = stop;
    }
}

static char *
bytes_to_string (GBytes *data)
{
  gsize length;
  const char *p = g_bytes_get_data (data, &length);

  return g_strndup (p, length);
}

static gboolean
scan_tar (Scan *scan, GError **error)
{
  guchar header[512];
  g_autofree char *long_name = NULL, *long_link = NULL, *pax_path = NULL, *pax_link = NULL;
  guint64 pax_size = 0;
  gboolean pax_has_size = FALSE;
  int zero_blocks = 0;

  for (;;)
    {
      gboolean eof = FALSE, ustar;
      guint64 size;
      char type;
      g_autofree char *raw_name = NULL, *raw_link = NULL, *name = NULL;
      g_autoptr (GBytes) data = NULL;
      int i;

      if (!read_block (scan, header, sizeof header, &eof, error))
        return FALSE;
      if (eof)
        return TRUE;
      for (i = 0; i < 512 && !header[i]; i++)
        ;
      if (i == 512)
        {
          /* two empty blocks end the archive */
          if (++zero_blocks == 2)
            return TRUE;
          continue;
        }
      zero_blocks = 0;

      type = header[156];
      size = pax_has_size ? pax_size : number (header + 124, 12);
      ustar = !memcmp (header + 257, "ustar\0", 6);
      if (long_name)
        raw_name = g_steal_pointer (&long_name);
      else if (pax_path)
        raw_name = g_steal_pointer (&pax_path);
      else if (ustar && header[345])
        raw_name = g_strdup_printf ("%.155s/%.100s", (char *) header + 345, (char *) header);
      else
        raw_name = g_strndup ((char *) header, 100);
      if (long_link)
        raw_link = g_steal_pointer (&long_link);
      else if (pax_link)
        raw_link = g_steal_pointer (&pax_link);
      else
        raw_link = g_strndup ((char *) header + 157, 100);
      pax_has_size = FALSE;
      g_clear_pointer (&pax_path, g_free);
      g_clear_pointer (&pax_link, g_free);

      switch (type)
        {
        case 'L':         /* GNU: the next member's long name */
        case 'K':         /* GNU: the next member's long link target */
        case 'x':         /* pax: the next member's extended header */
          if (size > HEADER_MAX)
            {
              g_set_error (error, INST_DEB_ERROR, INST_DEB_ERROR_NOT_A_PACKAGE, "A file name is too long");
              return FALSE;
            }
          data = read_data (scan, size, error);
          if (!data)
            return FALSE;
          if (type == 'L')
            long_name = bytes_to_string (data);
          else if (type == 'K')
            long_link = bytes_to_string (data);
          else
            parse_pax (data, &pax_path, &pax_link, &pax_size, &pax_has_size);
          continue;
        default:
          break;
        }

      name = member_name (raw_name);
      if ((type == '0' || type == '\0' || type == '7')
          && (is_desktop (name) || is_icon (name) || !g_strcmp0 (name, scan->want)))
        {
          gboolean desktop = is_desktop (name);
          gboolean keep = desktop ? size <= DESKTOP_MAX && scan->desktops < DESKTOP_COUNT
                                  : size <= ICON_MAX
                                    && (scan->icon_bytes + size <= ICONS_TOTAL || !g_strcmp0 (name, scan->want));

          if (keep)
            {
              Member *m = g_new0 (Member, 1);

              m->data = read_data (scan, size, error);
              if (!m->data)
                {
                  member_free (m);
                  return FALSE;
                }
              if (desktop)
                scan->desktops++;
              else
                scan->icon_bytes += size;
              g_hash_table_replace (scan->members, g_steal_pointer (&name), m);
              continue;
            }
        }
      else if ((type == '1' || type == '2') && (is_desktop (name) || is_icon (name)))
        {
          Member *m = g_new0 (Member, 1);
          g_autofree char *dir = g_path_get_dirname (name);
          g_autofree char *absolute = NULL;

          /* a symlink's target is relative to its own directory; a hard link's is a member name */
          if (type == '2' && !g_path_is_absolute (raw_link))
            {
              g_autofree char *base = g_strconcat ("/", dir, NULL);

              absolute = g_canonicalize_filename (raw_link, base);
            }
          else
            absolute = g_canonicalize_filename (raw_link, "/");
          m->link = member_name (absolute);
          g_hash_table_replace (scan->members, g_steal_pointer (&name), m);
        }
      /* anything else (and what is over the limits) is passed over */
      if (!skip (scan, size + (512 - size % 512) % 512, error))
        return FALSE;
    }
}

/* where a member's links lead: the first name that is no kept link */
static const char *
link_end (GHashTable *members, const char *name)
{
  for (int depth = 0; name && depth < LINK_DEPTH; depth++)
    {
      Member *m = g_hash_table_lookup (members, name);

      if (!m || !m->link)
        return name;
      name = m->link;
    }
  return NULL;
}

/* a kept member's contents, its links followed within the archive */
static GBytes *
member_data (GHashTable *members, const char *name)
{
  for (int depth = 0; name && depth < LINK_DEPTH; depth++)
    {
      Member *m = g_hash_table_lookup (members, name);

      if (!m)
        return NULL;
      if (m->data)
        return m->data;
      name = m->link;
    }
  return NULL;
}

/* how good an icon file is for showing large: a scalable one first, then the largest bitmap; hicolor before other
 * themes; an XPM last */
static int
icon_score (const char *name, const char *type)
{
  g_auto (GStrv) parts = g_strsplit (name, "/", -1);
  int score = 0;

  if (!g_strcmp0 (type, "xpm"))
    return 1;
  if (g_str_has_prefix (name, "usr/share/icons/") && g_strv_length (parts) >= 6)
    {
      const char *size = parts[4];

      if (!g_strcmp0 (size, "scalable"))
        score = 10000;
      else
        {
          int n = atoi (size), scale = strchr (size, '@') ? atoi (strchr (size, '@') + 1) : 1;

          score = MIN (n * MAX (scale, 1), 4096);
        }
      if (!g_strcmp0 (parts[3], "hicolor"))
        score += 1;
    }
  else
    score = !g_strcmp0 (type, "svg") ? 9000 : 48;   /* /usr/share/pixmaps */
  return score;
}

/* The icon file a desktop file's Icon= names, of the package's own files, by path or by name: the best one, and its
 * contents when they were kept. Its name is returned, NULL for none: a link to a file that was not kept (one outside
 * the icon directories) leaves deb->icon NULL, for a second look (inst_deb_read_files). `kept_only` passes over such
 * links. */
static char *
pick_icon (InstDeb *deb, GHashTable *members, const char *icon, gboolean kept_only)
{
  GHashTableIter iter;
  gpointer key, value;
  g_autofree char *base = NULL;
  const char *best = NULL;
  int best_score = -1;

  if (!icon || !*icon)
    return NULL;
  if (g_path_is_absolute (icon))
    {
      g_autofree char *name = member_name (icon);
      GBytes *data = member_data (members, name);

      if (!icon_type (name))
        return NULL;
      if (data)
        {
          deb->icon = g_bytes_ref (data);
          deb->icon_type = g_strdup (icon_type (name));
        }
      return kept_only && !data ? NULL : g_steal_pointer (&name);
    }
  /* an old desktop file may give the name with its extension */
  base = g_strdup (icon);
  if (icon_type (base))
    *strrchr (base, '.') = '\0';
  g_hash_table_iter_init (&iter, members);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *name = key, *slash = strrchr (name, '/'), *type = icon_type (name);
      g_autofree char *stem = NULL;
      int score;

      if (!type || !slash || !is_icon (name))
        continue;
      stem = g_strdup (slash + 1);
      *strrchr (stem, '.') = '\0';
      if (g_strcmp0 (stem, base) || (kept_only && !member_data (members, name)))
        continue;
      score = icon_score (name, type);
      if (score > best_score || (score == best_score && g_strcmp0 (name, best) < 0))
        {
          best = name;
          best_score = score;
        }
    }
  if (best && member_data (members, best))
    {
      deb->icon = g_bytes_ref (member_data (members, best));
      deb->icon_type = g_strdup (icon_type (best));
    }
  return g_strdup (best);
}

/* the app among the package's desktop files: one the app drawer would show (an application, not hidden, with a
 * command), the one named after the package first */
static char *
pick_app (InstDeb *deb, GHashTable *members)
{
  g_autoptr (GList) names = g_list_sort (g_hash_table_get_keys (members), (GCompareFunc) g_strcmp0);
  g_autoptr (GKeyFile) best = NULL;
  const char *best_name = NULL;
  int best_score = -1;

  for (GList *l = names; l; l = l->next)
    {
      const char *name = l->data;
      g_autoptr (GKeyFile) kf = NULL;
      g_autofree char *type = NULL, *exec = NULL, *lower = NULL, *package = NULL;
      GBytes *data;
      int score = 1;

      if (!is_desktop (name) || !(data = member_data (members, name)))
        continue;
      kf = g_key_file_new ();
      if (!g_key_file_load_from_bytes (kf, data, G_KEY_FILE_KEEP_TRANSLATIONS, NULL))
        continue;
      type = g_key_file_get_string (kf, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_TYPE, NULL);
      exec = g_key_file_get_string (kf, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_EXEC, NULL);
      if (g_strcmp0 (type, "Application") || !exec || !*exec
          || g_key_file_get_boolean (kf, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_NO_DISPLAY, NULL)
          || g_key_file_get_boolean (kf, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_HIDDEN, NULL)
          || !g_key_file_has_key (kf, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_NAME, NULL))
        continue;
      lower = g_ascii_strdown (name, -1);
      package = g_ascii_strdown (deb->package, -1);
      if (strstr (lower, package))
        score = 2;
      if (score > best_score)
        {
          g_clear_pointer (&best, g_key_file_unref);
          best = g_steal_pointer (&kf);
          best_name = name;
          best_score = score;
        }
    }
  if (!best)
    return NULL;
  deb->app_name = g_key_file_get_locale_string (best, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_NAME, NULL,
                                                NULL);
  deb->desktop_path = g_strconcat ("/", best_name, NULL);
  /* its Icon= */
  return g_key_file_get_string (best, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_ICON, NULL);
}

/* one pass over the data archive, keeping what `scan` asks for in `members` */
static gboolean
scan_archive (const char *path, GHashTable *members, const char *want, GCancellable *cancellable, GError **error)
{
  g_autoptr (GSubprocess) proc = NULL;
  g_autoptr (GError) local = NULL;
  Scan scan = { 0 };

  proc = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &local,
                           "dpkg-deb", "--fsys-tarfile", "--", path, NULL);
  if (!proc)
    {
      g_set_error (error, INST_DEB_ERROR, INST_DEB_ERROR_NO_DPKG, "dpkg-deb: %s", local->message);
      return FALSE;
    }
  scan.in = g_subprocess_get_stdout_pipe (proc);
  scan.cancellable = cancellable;
  scan.members = members;
  scan.want = want;
  if (!scan_tar (&scan, error))
    {
      g_subprocess_force_exit (proc);
      g_subprocess_wait (proc, NULL, NULL);
      return FALSE;
    }
  /* the rest of the stream (what follows the end of the archive), then dpkg-deb's verdict */
  skip (&scan, G_MAXUINT64, NULL);
  if (!g_subprocess_wait (proc, cancellable, error))
    return FALSE;
  if (!g_subprocess_get_successful (proc))
    {
      g_set_error (error, INST_DEB_ERROR, INST_DEB_ERROR_NOT_A_PACKAGE, "dpkg-deb cannot read the package's files");
      return FALSE;
    }
  return TRUE;
}

gboolean
inst_deb_read_files (InstDeb *deb, const char *path, GCancellable *cancellable, GError **error)
{
  g_autoptr (GHashTable) members = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, member_free);
  g_autofree char *icon = NULL, *chosen = NULL;

  if (!scan_archive (path, members, NULL, cancellable, error))
    return FALSE;
  icon = pick_app (deb, members);
  chosen = pick_icon (deb, members, icon, FALSE);
  if (chosen && !deb->icon)
    {
      /* the best icon is a link to a file elsewhere in the package (or named by an absolute path outside the icon
       * directories): one more pass, for that file */
      g_autofree char *want = g_strdup (link_end (members, chosen));

      if (want && !scan_archive (path, members, want, cancellable, error))
        return FALSE;
      if (want && !g_hash_table_contains (members, chosen))
        {
          /* an absolute Icon= path: the file itself */
          GBytes *data = member_data (members, want);

          if (data)
            {
              deb->icon = g_bytes_ref (data);
              deb->icon_type = g_strdup (icon_type (chosen));
            }
        }
      else
        {
          g_clear_pointer (&deb->icon_type, g_free);
          g_free (pick_icon (deb, members, icon, TRUE));
        }
    }
  return TRUE;
}

InstDeb *
inst_deb_inspect (const char *path, GCancellable *cancellable, GError **error)
{
  InstDeb *deb = inst_deb_read_control (path, cancellable, error);

  if (deb && !inst_deb_read_files (deb, path, cancellable, error))
    g_clear_pointer (&deb, inst_deb_free);
  return deb;
}
