/* Locations (fm-places.h). */
#include "fm-places.h"
#include "fm-util.h"

#include <glib/gi18n.h>
#include <string.h>

void
fm_place_free (FmPlace *place)
{
  if (!place)
    return;
  g_clear_object (&place->root);
  g_clear_object (&place->mount);
  g_free (place->name);
  g_free (place->remote_id);
  g_free (place);
}

FmPlace *
fm_place_copy (const FmPlace *place)
{
  FmPlace *copy = g_new0 (FmPlace, 1);

  copy->kind = place->kind;
  copy->root = g_object_ref (place->root);
  copy->name = g_strdup (place->name);
  copy->glyph = place->glyph;
  copy->remote_id = g_strdup (place->remote_id);
  copy->mount = place->mount ? g_object_ref (place->mount) : NULL;
  return copy;
}

static FmPlace *
place_new (FmPlaceKind kind, GFile *root, const char *name, FmGlyph glyph)
{
  FmPlace *place = g_new0 (FmPlace, 1);

  place->kind = kind;
  place->root = g_object_ref (root);
  place->name = g_strdup (name);
  place->glyph = glyph;
  return place;
}

gboolean
fm_scheme_is_server (const char *scheme)
{
  static const char *const servers[] = { "smb", "sftp", "ssh", "ftp", "ftps", "dav", "davs", "afp", "nfs", NULL };

  return scheme && g_strv_contains (servers, scheme);
}

FmGlyph
fm_glyph_for_proto (FmProto proto)
{
  switch (proto)
    {
    case FM_PROTO_FTP:
      return FM_GLYPH_REMOTE_FTP;
    case FM_PROTO_FTPS:
      return FM_GLYPH_REMOTE_FTPS;
    case FM_PROTO_SFTP:
      return FM_GLYPH_REMOTE_SFTP;
    case FM_PROTO_DAV:
    case FM_PROTO_DAVS:
      return FM_GLYPH_REMOTE_DAV;
    case FM_PROTO_SMB:
    default:
      return FM_GLYPH_REMOTE_SMB;
    }
}

FmGlyph
fm_glyph_for_scheme (const char *scheme)
{
  FmProto proto;

  if (scheme && !strcmp (scheme, "ssh"))
    return FM_GLYPH_REMOTE_SFTP;
  if (fm_proto_from_scheme (scheme, &proto))
    return fm_glyph_for_proto (proto);
  return FM_GLYPH_REMOTE;
}

gboolean
fm_mount_is_media (GMount *mount)
{
  g_autoptr(GFile) root = g_mount_get_root (mount);

  /* a server's share GVfs mounted (smb, sftp ...) belongs to its remote, not to the media */
  if (g_mount_is_shadowed (mount))
    return FALSE;
  if (!g_file_is_native (root))
    {
      g_autofree char *scheme = g_file_get_uri_scheme (root);

      return !fm_scheme_is_server (scheme) && g_strcmp0 (scheme, "trash") && g_strcmp0 (scheme, "recent");
    }
  return TRUE;
}

char *
fm_mount_label (GMount *mount, FmGlyph *glyph)
{
  g_autoptr(GDrive) drive = g_mount_get_drive (mount);
  g_autofree char *device = drive ? g_drive_get_identifier (drive, G_DRIVE_IDENTIFIER_KIND_UNIX_DEVICE) : NULL;
  FmGlyph shown = FM_GLYPH_USB;
  char *label;

  if (device && g_str_has_prefix (device, "/dev/mmcblk"))
    {
      shown = FM_GLYPH_SD;
      label = g_strdup (_("SD card"));
    }
  else if (drive && (g_drive_is_removable (drive) || g_drive_is_media_removable (drive)))
    label = g_strdup (_("USB storage"));
  else
    {
      shown = FM_GLYPH_STORAGE;
      label = g_mount_get_name (mount);
    }
  if (glyph)
    *glyph = shown;
  return label;
}

/* the root of a server not saved: scheme://[user@]host[:port]/, its share added for smb would split the server */
static GFile *
server_root (GFile *file, char **host)
{
  g_autofree char *uri = g_file_get_uri (file);
  g_autoptr(GUri) parsed = g_uri_parse (uri, G_URI_FLAGS_ENCODED, NULL);
  g_autofree char *root = NULL;

  if (!parsed || !g_uri_get_host (parsed))
    return NULL;
  root = g_uri_join (G_URI_FLAGS_ENCODED, g_uri_get_scheme (parsed), g_uri_get_userinfo (parsed),
                     g_uri_get_host (parsed), g_uri_get_port (parsed), "/", NULL, NULL);
  *host = g_strdup (g_uri_get_host (parsed));
  return g_file_new_for_uri (root);
}

FmPlace *
fm_place_find (GFile *file, GPtrArray *remotes, GList *mounts)
{
  g_autofree char *scheme = g_file_get_uri_scheme (file);
  g_autofree char *uri = g_file_get_uri (file);
  FmRemote *best = NULL;
  gsize best_len = 0;
  GMount *best_mount = NULL;
  g_autoptr(GFile) best_root = NULL;

  /* a saved remote, the longest that holds it */
  for (guint i = 0; remotes && i < remotes->len; i++)
    {
      FmRemote *remote = remotes->pdata[i];
      g_autofree char *remote_uri = fm_remote_uri (remote);

      if (fm_remote_contains (remote, uri, NULL) && strlen (remote_uri) > best_len)
        {
          best = remote;
          best_len = strlen (remote_uri);
        }
    }
  if (best)
    {
      g_autofree char *remote_uri = fm_remote_uri (best);
      g_autoptr(GUri) own = g_uri_parse (remote_uri, G_URI_FLAGS_ENCODED, NULL);
      g_autoptr(GUri) seen = g_uri_parse (uri, G_URI_FLAGS_ENCODED, NULL);
      g_autofree char *root_uri = NULL;
      g_autoptr(GFile) root = NULL;
      FmPlace *place;

      /* the root in the form GVfs gave the file (with or without the user), so that relative paths work */
      if (own && seen)
        root_uri = g_uri_join (G_URI_FLAGS_ENCODED, g_uri_get_scheme (seen), g_uri_get_userinfo (seen),
                               g_uri_get_host (seen), g_uri_get_port (seen), g_uri_get_path (own), NULL, NULL);
      root = g_file_new_for_uri (root_uri ? root_uri : remote_uri);
      place = place_new (FM_PLACE_REMOTE, root, fm_remote_label (best), fm_glyph_for_proto (best->proto));

      place->remote_id = g_strdup (best->id);
      return place;
    }

  if (!g_strcmp0 (scheme, "trash"))
    {
      g_autoptr(GFile) root = g_file_new_for_uri ("trash:///");

      return place_new (FM_PLACE_TRASH, root, _("Recycle Bin"), FM_GLYPH_TRASH);
    }
  if (!g_strcmp0 (scheme, "recent"))
    {
      g_autoptr(GFile) root = g_file_new_for_uri ("recent:///");

      return place_new (FM_PLACE_RECENT, root, _("Recent files"), FM_GLYPH_RECENT);
    }
  if (!g_strcmp0 (scheme, "network"))
    {
      g_autoptr(GFile) root = g_file_new_for_uri ("network:///");

      return place_new (FM_PLACE_NETWORK, root, _("Local Network"), FM_GLYPH_NETWORK);
    }

  if (fm_scheme_is_server (scheme))
    {
      g_autofree char *host = NULL;
      g_autoptr(GFile) root = server_root (file, &host);

      if (root)
        return place_new (FM_PLACE_SERVER, root, host, fm_glyph_for_scheme (scheme));
    }

  /* a medium (or a phone, a camera): the longest mount root that holds it */
  for (GList *l = mounts; l; l = l->next)
    {
      g_autoptr(GFile) root = g_mount_get_root (l->data);
      g_autofree char *root_uri = g_file_get_uri (root);

      if (!fm_mount_is_media (l->data) || !fm_file_is_under (file, root))
        continue;
      if (!best_root || strlen (root_uri) > best_len)
        {
          g_set_object (&best_root, root);
          best_mount = l->data;
          best_len = strlen (root_uri);
        }
    }
  if (best_mount)
    {
      FmGlyph glyph;
      g_autofree char *label = fm_mount_label (best_mount, &glyph);
      gboolean native = g_file_is_native (best_root);
      FmPlace *place = place_new (native ? FM_PLACE_REMOVABLE : FM_PLACE_OTHER, best_root, label, glyph);

      place->mount = g_object_ref (best_mount);
      return place;
    }

  if (g_file_is_native (file))
    {
      g_autoptr(GFile) home = g_file_new_for_path (g_get_home_dir ());
      g_autoptr(GFile) slash = g_file_new_for_path ("/");

      if (fm_file_is_under (file, home))
        return place_new (FM_PLACE_STORAGE, home, _("Main storage"), FM_GLYPH_STORAGE);
      return place_new (FM_PLACE_SYSTEM, slash, _("System"), FM_GLYPH_SYSTEM);
    }

  /* anything else: its own authority, or the file itself */
  {
    g_autofree char *host = NULL;
    g_autoptr(GFile) root = server_root (file, &host);

    if (root)
      return place_new (FM_PLACE_OTHER, root, host, FM_GLYPH_REMOTE);
    return place_new (FM_PLACE_OTHER, file, uri, FM_GLYPH_FOLDER);
  }
}

gboolean
fm_place_is_visitable (const FmPlace *place)
{
  switch (place->kind)
    {
    case FM_PLACE_REMOVABLE:
    case FM_PLACE_SYSTEM:
    case FM_PLACE_REMOTE:
    case FM_PLACE_SERVER:
    case FM_PLACE_OTHER:
      return TRUE;
    default:
      return FALSE;
    }
}

static gboolean
has_suffix (const char *name, const char *const *suffixes)
{
  g_autofree char *lower = name ? g_ascii_strdown (name, -1) : NULL;

  for (int i = 0; lower && suffixes[i]; i++)
    if (g_str_has_suffix (lower, suffixes[i]))
      return TRUE;
  return FALSE;
}

FmGlyph
fm_glyph_for_file (const char *content_type, const char *name)
{
  static const char *const archives[] = { ".zip", ".tar", ".gz", ".tgz", ".bz2", ".xz", ".zst", ".7z", ".rar", NULL };
  static const char *const code[] = { ".c", ".h", ".py", ".js", ".sh", ".json", ".xml", ".css", ".rs", ".java",
                                      ".kt", ".cpp", ".go", ".rb", ".pl", ".lua", ".ts", NULL };
  static const char *const packages[] = { ".deb", ".apk", ".rpm", ".flatpak", ".appimage", NULL };
  g_autofree char *mime = content_type ? g_content_type_get_mime_type (content_type) : NULL;

  if (has_suffix (name, packages))
    return FM_GLYPH_PACKAGE;
  if (!mime)
    return FM_GLYPH_FILE;
  if (g_str_has_prefix (mime, "image/"))
    return FM_GLYPH_IMAGE;
  if (g_str_has_prefix (mime, "audio/"))
    return FM_GLYPH_AUDIO;
  if (g_str_has_prefix (mime, "video/"))
    return FM_GLYPH_VIDEO;
  if (!strcmp (mime, "application/pdf"))
    return FM_GLYPH_PDF;
  if (!strcmp (mime, "text/html") || !strcmp (mime, "application/xhtml+xml"))
    return FM_GLYPH_WEB;
  if (has_suffix (name, archives) || strstr (mime, "zip") || strstr (mime, "compressed") ||
      strstr (mime, "-tar") || strstr (mime, "archive"))
    return FM_GLYPH_ARCHIVE;
  if (has_suffix (name, code) || strstr (mime, "script") || strstr (mime, "src"))
    return FM_GLYPH_CODE;
  if (g_str_has_prefix (mime, "text/") || strstr (mime, "document") || strstr (mime, "opendocument") ||
      strstr (mime, "msword"))
    return FM_GLYPH_TEXT;
  return FM_GLYPH_FILE;
}

FmGlyph
fm_glyph_for_folder (GFile *folder)
{
  static const struct {
    GUserDirectory dir;
    FmGlyph glyph;
  } dirs[] = {
    { G_USER_DIRECTORY_DOWNLOAD, FM_GLYPH_DOWNLOADS },
    { G_USER_DIRECTORY_PICTURES, FM_GLYPH_PICTURES },
    { G_USER_DIRECTORY_MUSIC, FM_GLYPH_MUSIC },
    { G_USER_DIRECTORY_VIDEOS, FM_GLYPH_VIDEOS },
    { G_USER_DIRECTORY_DOCUMENTS, FM_GLYPH_DOCUMENTS },
  };
  g_autofree char *path = g_file_is_native (folder) ? g_file_get_path (folder) : NULL;

  for (guint i = 0; path && i < G_N_ELEMENTS (dirs); i++)
    {
      const char *dir = g_get_user_special_dir (dirs[i].dir);

      /* an unset XDG directory is $HOME itself: that one is no badge's */
      if (dir && !g_str_equal (dir, g_get_home_dir ()) && g_str_equal (dir, path))
        return dirs[i].glyph;
    }
  return FM_GLYPH_FOLDER;
}
