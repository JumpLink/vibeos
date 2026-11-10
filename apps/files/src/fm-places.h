/* Locations: which root a folder is under, what it is called and which icon it has (spec §2, §4.1, §5).
 *
 * A root is what the path bar shows as its first crumb and the drawer lists: Main storage ($HOME), a removable
 * medium (a GMount of GVolumeMonitor), System (/), the Recycle Bin, Recent files, the Local Network, a saved remote
 * (its URI), or any other server (scheme://host/). No GTK: the tests link it alone. */
#pragma once

#include <gio/gio.h>

#include "fm-remotes.h"

G_BEGIN_DECLS

/* the places' and file kinds' icons: each stands for a theme icon (fm_glyph_get_icon in fm-icon.c) */
typedef enum {
  FM_GLYPH_FOLDER,
  FM_GLYPH_HOME,
  FM_GLYPH_STORAGE,
  FM_GLYPH_SD,
  FM_GLYPH_USB,
  FM_GLYPH_SYSTEM,
  FM_GLYPH_TRASH,
  FM_GLYPH_RECENT,
  FM_GLYPH_NETWORK,             /* Local Network (network-workgroup) */
  FM_GLYPH_REMOTE,              /* the Remote page; it and the remotes are folder-remote */
  FM_GLYPH_REMOTE_SMB,
  FM_GLYPH_REMOTE_FTP,
  FM_GLYPH_REMOTE_FTPS,
  FM_GLYPH_REMOTE_SFTP,
  FM_GLYPH_REMOTE_DAV,
  FM_GLYPH_SERVER,              /* a computer found on the network (network-server) */
  FM_GLYPH_DOWNLOADS,
  FM_GLYPH_PICTURES,
  FM_GLYPH_MUSIC,
  FM_GLYPH_VIDEOS,
  FM_GLYPH_DOCUMENTS,
  FM_GLYPH_FILE,
  FM_GLYPH_IMAGE,
  FM_GLYPH_AUDIO,
  FM_GLYPH_VIDEO,
  FM_GLYPH_TEXT,
  FM_GLYPH_PDF,
  FM_GLYPH_ARCHIVE,
  FM_GLYPH_CODE,
  FM_GLYPH_WEB,
  FM_GLYPH_PACKAGE,
  FM_N_GLYPHS
} FmGlyph;

typedef enum {
  FM_PLACE_STORAGE,
  FM_PLACE_REMOVABLE,
  FM_PLACE_SYSTEM,
  FM_PLACE_TRASH,
  FM_PLACE_RECENT,
  FM_PLACE_NETWORK,
  FM_PLACE_REMOTE,              /* a saved remote */
  FM_PLACE_SERVER,              /* a server not saved (browsed from the Local Network) */
  FM_PLACE_OTHER,               /* another mount: a phone over MTP, a camera */
} FmPlaceKind;

typedef struct {
  FmPlaceKind kind;
  GFile *root;
  char *name;
  FmGlyph glyph;
  char *remote_id;              /* FM_PLACE_REMOTE: the remote's id */
  GMount *mount;                /* a removable medium's (or another) mount, when there is one */
} FmPlace;

/* @remotes: FmRemote; @mounts: the user-visible GMounts (g_volume_monitor_get_mounts), may be NULL */
FmPlace    *fm_place_find        (GFile *file, GPtrArray *remotes, GList *mounts);
FmPlace    *fm_place_copy        (const FmPlace *place);
void        fm_place_free        (FmPlace *place);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FmPlace, fm_place_free)

/* whether the drawer lists the place among the last visited (remotes, media, servers, System) */
gboolean    fm_place_is_visitable (const FmPlace *place);

/* a removable medium's name and glyph: "SD card", "USB storage", else the mount's own name */
char       *fm_mount_label       (GMount *mount, FmGlyph *glyph);
/* whether a mount is a removable medium for the drawer and Home (a local filesystem, not a server's share) */
gboolean    fm_mount_is_media    (GMount *mount);

FmGlyph     fm_glyph_for_proto   (FmProto proto);
FmGlyph     fm_glyph_for_scheme  (const char *scheme);
/* a file's icon by its content type (and its name, for the types it does not tell) */
FmGlyph     fm_glyph_for_file    (const char *content_type, const char *name);
/* the XDG folders Home shows get their own badges */
FmGlyph     fm_glyph_for_folder  (GFile *folder);

/* the network schemes GVfs mounts per server (the Remote page's and the Local Network's) */
gboolean    fm_scheme_is_server  (const char *scheme);

G_END_DECLS
