/* FmItem: one row of a listing (a file, a folder, a share on a server, a host on the network), as GIO describes it,
 * and the orders File Manager+ sorts a folder in. No GTK: the tests link it alone. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* the attributes every listing asks for */
#define FM_ITEM_ATTRIBUTES                                                                                         \
  "standard::name,standard::display-name,standard::edit-name,standard::type,standard::size,standard::is-hidden,"  \
  "standard::is-backup,standard::content-type,standard::fast-content-type,standard::target-uri,"                 \
  "standard::is-symlink,standard::symlink-target,time::modified,trash::orig-path,trash::deletion-date,"          \
  "access::can-read,access::can-write,access::can-delete,access::can-trash,access::can-rename,unix::mode,"       \
  "thumbnail::path,mountable::can-mount"

/* the child count of a folder: not asked yet, or asked and unknown ("- items") */
#define FM_COUNT_PENDING (-2)
#define FM_COUNT_UNKNOWN (-1)

typedef enum {
  FM_SORT_NONE,
  FM_SORT_NAME_UP,
  FM_SORT_NAME_DOWN,
  FM_SORT_SIZE_UP,
  FM_SORT_SIZE_DOWN,
  FM_SORT_DATE_UP,
  FM_SORT_DATE_DOWN,
  FM_SORT_TYPE_UP,
  FM_SORT_TYPE_DOWN,
} FmSort;

#define FM_TYPE_ITEM (fm_item_get_type ())
G_DECLARE_FINAL_TYPE (FmItem, fm_item, FM, ITEM, GObject)

struct _FmItem {
  GObject parent;
  GFile *file;
  char *name;                   /* what the row shows */
  char *edit_name;              /* what Rename starts from */
  GFileType type;
  goffset size;
  gint64 mtime;                 /* unix seconds, 0 unknown */
  char *content_type;
  gboolean hidden;
  char *target_uri;             /* a share, a host, a recent file: where it really is */
  char *orig_path;              /* in the Recycle Bin: where it was */
  gint64 deleted;               /* in the Recycle Bin: when (unix seconds) */
  gboolean can_write, can_delete, can_trash, can_rename;
  guint32 mode;                 /* unix::mode, 0 unknown */
  int count;                    /* a folder's items: FM_COUNT_PENDING, FM_COUNT_UNKNOWN or the number */
  char *where;                  /* a search result: the folder it is in, as shown */
  gboolean selected;
  char *collate_key;            /* g_utf8_collate_key_for_filename of the name */
};

FmItem     *fm_item_new_from_info (GFile *parent, GFileInfo *info);
/* "changed": the count, the selection or the thumbnail changed; a row that shows it draws it again */
void        fm_item_changed       (FmItem *item);
FmItem     *fm_item_new_for_file  (GFile *file, const char *name, GFileType type);

/* a folder to the user: a directory, a share or host (a mountable or a shortcut with a target) */
gboolean    fm_item_is_folder     (FmItem *item);
/* where opening it goes: the target of a share or a recent file, else the file itself (new reference) */
GFile      *fm_item_get_location  (FmItem *item);
/* the extension, lower case, without the dot ("" for none or a folder) */
const char *fm_item_extension     (FmItem *item);

/* folders first, then @sort; NONE keeps the order the backend gave (stable) */
int         fm_item_compare       (FmItem *a, FmItem *b, FmSort sort);
void        fm_items_sort         (GPtrArray *items, FmSort sort);

/* GSettings' sort-order string ("NameUp", ...: File Manager+'s own values) and back */
FmSort      fm_sort_from_string   (const char *value);
const char *fm_sort_to_string     (FmSort sort);

G_END_DECLS
