/* The saved remotes (spec §6, §12): a server's address and user, never a password. They are kept in
 * $XDG_CONFIG_HOME/neo-files/remotes.ini, a GKeyFile with one group per remote. No GTK: the tests link it alone. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum {
  FM_PROTO_SMB,
  FM_PROTO_FTP,
  FM_PROTO_FTPS,
  FM_PROTO_SFTP,
  FM_PROTO_DAV,                 /* WebDAV over http */
  FM_PROTO_DAVS,                /* WebDAV over https */
  FM_N_PROTOS
} FmProto;

typedef struct {
  char *id;                     /* the key file's group */
  char *name;                   /* the display name; empty for the host */
  FmProto proto;
  char *host;
  int port;                     /* 0: the protocol's own */
  char *path;                   /* the share or folder under the host, without the first '/'; may be empty */
  char *user;                   /* empty for none */
  char *domain;                 /* SMB only; empty for none */
  gboolean anonymous;           /* SMB and FTP: log in without a user */
} FmRemote;

FmRemote   *fm_remote_new        (FmProto proto);
FmRemote   *fm_remote_copy       (const FmRemote *remote);
void        fm_remote_free       (FmRemote *remote);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FmRemote, fm_remote_free)

/* "smb", "ftp", ... (the URI scheme and the key file's type) */
const char *fm_proto_scheme      (FmProto proto);
/* SMB, FTP, FTPS, SFTP, WebDAV */
const char *fm_proto_label       (FmProto proto);
int         fm_proto_default_port (FmProto proto);
gboolean    fm_proto_from_scheme (const char *scheme, FmProto *proto);

/* What the user typed in Host: "192.168.1.101/folder" or "example.com/webdav" splits into the host and the path;
 * a whole URI ("smb://server/share") is taken apart too, its scheme setting @proto when it names one. */
void        fm_remote_set_address (FmRemote *remote, const char *text);
/* Host as the dialog shows it again: the host and the path */
char       *fm_remote_get_address (const FmRemote *remote);

/* The GVfs URI: smb://[domain;][user@]host[:port]/share, sftp://user@host:port/path, dav[s]://..., with a '/'
 * at the end of the path. The user part is left out for an anonymous login. */
char       *fm_remote_uri        (const FmRemote *remote);
/* the name to show: the display name, else the host */
const char *fm_remote_label      (const FmRemote *remote);

/* whether @uri is this remote or under it (the same scheme, host, port and user; the path inside); @rest gets the
 * part of the path below the remote's own, starting with '/' ("/" at the root) */
gboolean    fm_remote_contains   (const FmRemote *remote, const char *uri, char **rest);

/* the list (FmRemote) from and to a key file; a missing file is an empty list */
GPtrArray  *fm_remotes_load      (const char *path);
gboolean    fm_remotes_save      (GPtrArray *remotes, const char *path, GError **error);
/* a group name not used in @remotes yet */
char       *fm_remotes_new_id    (GPtrArray *remotes);

G_END_DECLS
