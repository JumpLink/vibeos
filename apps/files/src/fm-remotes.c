/* The saved remotes (fm-remotes.h). */
#include "fm-remotes.h"

#include <string.h>

static const struct {
  const char *scheme, *label;
  int port;
} protos[FM_N_PROTOS] = {
  [FM_PROTO_SMB] = { "smb", "SMB", 445 },
  [FM_PROTO_FTP] = { "ftp", "FTP", 21 },
  [FM_PROTO_FTPS] = { "ftps", "FTPS", 21 },
  [FM_PROTO_SFTP] = { "sftp", "SFTP", 22 },
  [FM_PROTO_DAV] = { "dav", "WebDAV", 80 },
  [FM_PROTO_DAVS] = { "davs", "WebDAV", 443 },
};

const char *
fm_proto_scheme (FmProto proto)
{
  return protos[proto].scheme;
}

const char *
fm_proto_label (FmProto proto)
{
  return protos[proto].label;
}

int
fm_proto_default_port (FmProto proto)
{
  return protos[proto].port;
}

gboolean
fm_proto_from_scheme (const char *scheme, FmProto *proto)
{
  for (int i = 0; i < FM_N_PROTOS; i++)
    if (scheme && !g_ascii_strcasecmp (scheme, protos[i].scheme))
      {
        *proto = i;
        return TRUE;
      }
  /* the web's names for WebDAV */
  if (scheme && (!g_ascii_strcasecmp (scheme, "http") || !g_ascii_strcasecmp (scheme, "webdav")))
    return *proto = FM_PROTO_DAV, TRUE;
  if (scheme && (!g_ascii_strcasecmp (scheme, "https") || !g_ascii_strcasecmp (scheme, "webdavs")))
    return *proto = FM_PROTO_DAVS, TRUE;
  return FALSE;
}

FmRemote *
fm_remote_new (FmProto proto)
{
  FmRemote *remote = g_new0 (FmRemote, 1);

  remote->proto = proto;
  remote->id = g_strdup ("");
  remote->name = g_strdup ("");
  remote->host = g_strdup ("");
  remote->path = g_strdup ("");
  remote->user = g_strdup ("");
  remote->domain = g_strdup ("");
  return remote;
}

FmRemote *
fm_remote_copy (const FmRemote *remote)
{
  FmRemote *copy = g_new0 (FmRemote, 1);

  *copy = *remote;
  copy->id = g_strdup (remote->id);
  copy->name = g_strdup (remote->name);
  copy->host = g_strdup (remote->host);
  copy->path = g_strdup (remote->path);
  copy->user = g_strdup (remote->user);
  copy->domain = g_strdup (remote->domain);
  return copy;
}

void
fm_remote_free (FmRemote *remote)
{
  if (!remote)
    return;
  g_free (remote->id);
  g_free (remote->name);
  g_free (remote->host);
  g_free (remote->path);
  g_free (remote->user);
  g_free (remote->domain);
  g_free (remote);
}

static void
replace (char **field, const char *value)
{
  g_free (*field);
  *field = g_strdup (value ? value : "");
}

/* the path without the slashes at its ends */
static char *
trim_path (const char *path)
{
  const char *start = path ? path : "";
  size_t len;

  while (*start == '/')
    start++;
  len = strlen (start);
  while (len > 0 && start[len - 1] == '/')
    len--;
  return g_strndup (start, len);
}

void
fm_remote_set_address (FmRemote *remote, const char *text)
{
  g_autofree char *trimmed = g_strstrip (g_strdup (text ? text : ""));
  g_autoptr(GUri) uri = NULL;
  const char *slash;

  /* a password in the URI ("user:secret@") is split off and dropped: it is never a remote's (the dialog hands it to
   * GVfs) */
  if (strstr (trimmed, "://") && (uri = g_uri_parse (trimmed, G_URI_FLAGS_HAS_PASSWORD, NULL)))
    {
      FmProto proto;
      g_autofree char *path = NULL;

      if (fm_proto_from_scheme (g_uri_get_scheme (uri), &proto))
        remote->proto = proto;
      replace (&remote->host, g_uri_get_host (uri));
      if (g_uri_get_port (uri) > 0)
        remote->port = g_uri_get_port (uri);
      path = g_uri_unescape_string (g_uri_get_path (uri), NULL);
      g_free (remote->path);
      remote->path = trim_path (path ? path : g_uri_get_path (uri));
      if (g_uri_get_user (uri))
        {
          g_autofree char *user = g_uri_unescape_string (g_uri_get_user (uri), NULL);
          char *semicolon = user ? strchr (user, ';') : NULL;

          /* smb's domain;user */
          if (semicolon)
            {
              *semicolon = '\0';
              replace (&remote->domain, user);
              replace (&remote->user, semicolon + 1);
            }
          else
            replace (&remote->user, user);
        }
      return;
    }
  /* "host/path"; a bracketed IPv6 address keeps its colons */
  slash = strchr (trimmed, '/');
  g_free (remote->path);
  remote->path = trim_path (slash ? slash : "");
  if (slash)
    *(char *) slash = '\0';
  /* "host:port" (not an IPv6 address, which has more than one colon) */
  {
    char *colon = strrchr (trimmed, ':');

    if (colon && strchr (trimmed, ':') == colon && colon[1] && g_ascii_isdigit (colon[1]))
      {
        remote->port = atoi (colon + 1);
        *colon = '\0';
      }
  }
  if (trimmed[0] == '[' && trimmed[strlen (trimmed) - 1] == ']')
    {
      trimmed[strlen (trimmed) - 1] = '\0';
      replace (&remote->host, trimmed + 1);
    }
  else
    replace (&remote->host, trimmed);
}

char *
fm_remote_get_address (const FmRemote *remote)
{
  if (!*remote->path)
    return g_strdup (remote->host);
  return g_strconcat (remote->host, "/", remote->path, NULL);
}

char *
fm_remote_uri (const FmRemote *remote)
{
  g_autofree char *userinfo = NULL;
  g_autofree char *path = NULL;
  int port = remote->port == fm_proto_default_port (remote->proto) ? -1 : remote->port > 0 ? remote->port : -1;

  if (!remote->anonymous && *remote->user)
    {
      g_autofree char *user = g_uri_escape_string (remote->user, NULL, FALSE);

      if (remote->proto == FM_PROTO_SMB && *remote->domain)
        {
          g_autofree char *domain = g_uri_escape_string (remote->domain, NULL, FALSE);

          userinfo = g_strconcat (domain, ";", user, NULL);
        }
      else
        userinfo = g_steal_pointer (&user);
    }
  if (*remote->path)
    {
      g_autofree char *escaped = g_uri_escape_string (remote->path, "/", FALSE);

      path = g_strconcat ("/", escaped, "/", NULL);
    }
  else
    path = g_strdup ("/");
  /* the user and the path are escaped above */
  return g_uri_join (G_URI_FLAGS_ENCODED, fm_proto_scheme (remote->proto), userinfo, remote->host, port, path, NULL,
                     NULL);
}

const char *
fm_remote_label (const FmRemote *remote)
{
  return *remote->name ? remote->name : remote->host;
}

gboolean
fm_remote_contains (const FmRemote *remote, const char *uri, char **rest)
{
  g_autoptr(GUri) mine = NULL, other = NULL;
  g_autofree char *own_uri = fm_remote_uri (remote);
  g_autofree char *my_path = NULL, *other_path = NULL;
  int my_port, other_port;
  size_t len;
  FmProto proto;

  mine = g_uri_parse (own_uri, G_URI_FLAGS_NONE, NULL);
  other = g_uri_parse (uri, G_URI_FLAGS_NONE, NULL);
  if (!mine || !other || !fm_proto_from_scheme (g_uri_get_scheme (other), &proto))
    return FALSE;
  /* ftp and ftps, dav and davs are different servers */
  if (g_ascii_strcasecmp (g_uri_get_scheme (mine), g_uri_get_scheme (other)) ||
      g_ascii_strcasecmp (g_uri_get_host (mine) ? g_uri_get_host (mine) : "",
                          g_uri_get_host (other) ? g_uri_get_host (other) : ""))
    return FALSE;
  my_port = g_uri_get_port (mine) > 0 ? g_uri_get_port (mine) : fm_proto_default_port (remote->proto);
  other_port = g_uri_get_port (other) > 0 ? g_uri_get_port (other) : fm_proto_default_port (proto);
  if (my_port != other_port)
    return FALSE;
  /* GVfs leaves the user out of some URIs it hands back (smb://host/share for a share browsed into): a URI with no
   * user matches any, one with a user must match ours */
  if (g_uri_get_user (other) && g_strcmp0 (g_uri_get_user (mine), g_uri_get_user (other)))
    {
      g_autofree char *a = g_uri_unescape_string (g_uri_get_user (other), NULL);
      g_autofree char *b = g_uri_get_user (mine) ? g_uri_unescape_string (g_uri_get_user (mine), NULL) : NULL;
      const char *a_user = a && strchr (a, ';') ? strchr (a, ';') + 1 : a;
      const char *b_user = b && strchr (b, ';') ? strchr (b, ';') + 1 : b;

      if (g_strcmp0 (a_user, b_user))
        return FALSE;
    }
  my_path = g_uri_unescape_string (g_uri_get_path (mine), NULL);
  other_path = g_uri_unescape_string (g_uri_get_path (other), NULL);
  if (!my_path || !other_path)
    return FALSE;
  /* my_path ends in '/'; the remote's own root may come without it */
  len = strlen (my_path);
  if (strncmp (other_path, my_path, len) != 0 && !(strlen (other_path) == len - 1 &&
                                                    strncmp (other_path, my_path, len - 1) == 0))
    return FALSE;
  if (rest)
    {
      const char *tail = strlen (other_path) >= len ? other_path + len - 1 : "/";
      g_autofree char *trimmed = trim_path (tail);

      *rest = g_strconcat ("/", trimmed, NULL);
    }
  return TRUE;
}

GPtrArray *
fm_remotes_load (const char *path)
{
  GPtrArray *remotes = g_ptr_array_new_with_free_func ((GDestroyNotify) fm_remote_free);
  g_autoptr(GKeyFile) file = g_key_file_new ();
  g_auto(GStrv) groups = NULL;

  if (!g_key_file_load_from_file (file, path, G_KEY_FILE_NONE, NULL))
    return remotes;
  groups = g_key_file_get_groups (file, NULL);
  for (int i = 0; groups[i]; i++)
    {
      g_autofree char *type = g_key_file_get_string (file, groups[i], "type", NULL);
      g_autofree char *uri = g_key_file_get_string (file, groups[i], "uri", NULL);
      FmProto proto;
      FmRemote *remote;
      char *value;

      if (!type || !fm_proto_from_scheme (type, &proto) || !uri)
        continue;
      remote = fm_remote_new (proto);
      fm_remote_set_address (remote, uri);
      remote->proto = proto;
      replace (&remote->id, groups[i]);
#define STRING(key, field)                                                                                          \
      if ((value = g_key_file_get_string (file, groups[i], key, NULL)))                                             \
        g_free (remote->field), remote->field = value;
      STRING ("name", name);
      STRING ("user", user);
      STRING ("domain", domain);
#undef STRING
      remote->anonymous = g_key_file_get_boolean (file, groups[i], "anonymous", NULL);
      g_ptr_array_add (remotes, remote);
    }
  return remotes;
}

gboolean
fm_remotes_save (GPtrArray *remotes, const char *path, GError **error)
{
  g_autoptr(GKeyFile) file = g_key_file_new ();
  g_autofree char *dir = g_path_get_dirname (path);

  g_key_file_set_comment (file, NULL, NULL,
                          " The remotes of neo-files (de.yesman.Files). Passwords are not here: GVfs keeps them in "
                          "the keyring.", NULL);
  for (guint i = 0; i < remotes->len; i++)
    {
      FmRemote *remote = remotes->pdata[i];
      g_autofree char *uri = NULL;
      FmRemote bare = *remote;

      /* the URI without the user, who is a key of its own */
      bare.user = (char *) "";
      bare.domain = (char *) "";
      uri = fm_remote_uri (&bare);
      g_key_file_set_string (file, remote->id, "name", remote->name);
      g_key_file_set_string (file, remote->id, "type", fm_proto_scheme (remote->proto));
      g_key_file_set_string (file, remote->id, "uri", uri);
      g_key_file_set_string (file, remote->id, "user", remote->user);
      g_key_file_set_string (file, remote->id, "domain", remote->domain);
      g_key_file_set_boolean (file, remote->id, "anonymous", remote->anonymous);
    }
  if (g_mkdir_with_parents (dir, 0700) != 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "%s: %s", dir, g_strerror (errno));
      return FALSE;
    }
  return g_key_file_save_to_file (file, path, error);
}

char *
fm_remotes_new_id (GPtrArray *remotes)
{
  for (int n = 1;; n++)
    {
      g_autofree char *id = g_strdup_printf ("remote-%d", n);
      gboolean used = FALSE;

      for (guint i = 0; i < remotes->len && !used; i++)
        used = !g_strcmp0 (((FmRemote *) remotes->pdata[i])->id, id);
      if (!used)
        return g_steal_pointer (&id);
    }
}
