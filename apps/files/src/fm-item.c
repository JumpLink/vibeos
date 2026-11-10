/* FmItem (fm-item.h). */
#include "fm-item.h"
#include "fm-util.h"

#include <string.h>

G_DEFINE_FINAL_TYPE (FmItem, fm_item, G_TYPE_OBJECT)

static void
fm_item_finalize (GObject *object)
{
  FmItem *self = FM_ITEM (object);

  g_clear_object (&self->file);
  g_free (self->name);
  g_free (self->edit_name);
  g_free (self->content_type);
  g_free (self->target_uri);
  g_free (self->orig_path);
  g_free (self->where);
  g_free (self->collate_key);
  G_OBJECT_CLASS (fm_item_parent_class)->finalize (object);
}

static guint changed_signal;

static void
fm_item_class_init (FmItemClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = fm_item_finalize;
  changed_signal = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                 G_TYPE_NONE, 0);
}

void
fm_item_changed (FmItem *item)
{
  g_signal_emit (item, changed_signal, 0);
}

static void
fm_item_init (FmItem *self)
{
  self->count = FM_COUNT_PENDING;
}

static void
set_name (FmItem *self, const char *name)
{
  g_free (self->name);
  self->name = g_strdup (name && *name ? name : "?");
  g_free (self->collate_key);
  /* case left out whatever the locale's collation does with it (the C locale's does not) */
  {
    g_autofree char *folded = g_utf8_casefold (self->name, -1);

    self->collate_key = g_utf8_collate_key_for_filename (folded, -1);
  }
}

FmItem *
fm_item_new_for_file (GFile *file, const char *name, GFileType type)
{
  FmItem *self = g_object_new (FM_TYPE_ITEM, NULL);
  g_autofree char *base = NULL;

  self->file = g_object_ref (file);
  if (!name)
    name = base = g_file_get_basename (file);
  set_name (self, name);
  self->edit_name = g_strdup (self->name);
  self->type = type;
  self->can_write = self->can_delete = self->can_trash = self->can_rename = TRUE;
  return self;
}

FmItem *
fm_item_new_from_info (GFile *parent, GFileInfo *info)
{
  FmItem *self = g_object_new (FM_TYPE_ITEM, NULL);
  const char *display = g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME);
  const char *name = g_file_info_get_name (info);
  const char *edit = g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_EDIT_NAME);
  const char *type = g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE);
  GDateTime *modified, *deleted;

  self->file = g_file_get_child (parent, name);
  set_name (self, display ? display : name);
  self->edit_name = g_strdup (edit ? edit : self->name);
  self->type = g_file_info_get_file_type (info);
  if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_STANDARD_SIZE))
    self->size = g_file_info_get_size (info);
  if (!type)
    type = g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE);
  self->content_type = g_strdup (type);
  self->hidden = fm_info_is_hidden (info);
  self->target_uri = g_strdup (g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI));
  if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_TIME_MODIFIED)
      && (modified = g_file_info_get_modification_date_time (info)))
    {
      self->mtime = g_date_time_to_unix (modified);
      g_date_time_unref (modified);
    }
  self->orig_path = g_strdup (g_file_info_get_attribute_byte_string (info, G_FILE_ATTRIBUTE_TRASH_ORIG_PATH));
  if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_TRASH_DELETION_DATE)
      && (deleted = g_file_info_get_deletion_date (info)))
    {
      self->deleted = g_date_time_to_unix (deleted);
      g_date_time_unref (deleted);
    }
  /* a missing access attribute means the backend does not say: let the operation find out */
#define CAN(attr) (!g_file_info_has_attribute (info, attr) || g_file_info_get_attribute_boolean (info, attr))
  self->can_write = CAN (G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE);
  self->can_delete = CAN (G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE);
  self->can_trash = CAN (G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH);
  self->can_rename = CAN (G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME);
#undef CAN
  self->mode = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_MODE);
  /* a plain file has no count to fill in */
  if (!fm_item_is_folder (self))
    self->count = FM_COUNT_UNKNOWN;
  return self;
}

gboolean
fm_item_is_folder (FmItem *item)
{
  switch (item->type)
    {
    case G_FILE_TYPE_DIRECTORY:
    case G_FILE_TYPE_MOUNTABLE:
      return TRUE;
    case G_FILE_TYPE_SHORTCUT:
      /* recent:// has shortcuts to files; network:// and smb://host/ to folders. A shortcut whose target has a
       * content type that is not a folder is a file. */
      return !item->content_type || g_content_type_is_a (item->content_type, "inode/directory") ||
             !g_strcmp0 (item->content_type, "inode/directory");
    default:
      return item->content_type && g_content_type_equals (item->content_type, "inode/directory");
    }
}

GFile *
fm_item_get_location (FmItem *item)
{
  if (item->target_uri && *item->target_uri)
    return g_file_new_for_uri (item->target_uri);
  return g_object_ref (item->file);
}

const char *
fm_item_extension (FmItem *item)
{
  const char *dot;

  if (fm_item_is_folder (item))
    return "";
  dot = strrchr (item->name, '.');
  return dot && dot != item->name && dot[1] ? dot + 1 : "";
}

static int
compare_names (FmItem *a, FmItem *b)
{
  return strcmp (a->collate_key, b->collate_key);
}

static int
compare_numbers (gint64 a, gint64 b)
{
  return a < b ? -1 : a > b ? 1 : 0;
}

int
fm_item_compare (FmItem *a, FmItem *b, FmSort sort)
{
  gboolean fa = fm_item_is_folder (a), fb = fm_item_is_folder (b);
  int c = 0;

  if (fa != fb)
    return fa ? -1 : 1;
  switch (sort)
    {
    case FM_SORT_NONE:
      return 0;
    case FM_SORT_NAME_UP:
    case FM_SORT_NAME_DOWN:
      c = compare_names (a, b);
      return sort == FM_SORT_NAME_UP ? c : -c;
    case FM_SORT_SIZE_UP:
    case FM_SORT_SIZE_DOWN:
      /* folders by their counts, files by their sizes; equal ones by name */
      c = fa ? compare_numbers (a->count, b->count) : compare_numbers (a->size, b->size);
      if (sort == FM_SORT_SIZE_DOWN)
        c = -c;
      break;
    case FM_SORT_DATE_UP:
    case FM_SORT_DATE_DOWN:
      c = compare_numbers (a->deleted ? a->deleted : a->mtime, b->deleted ? b->deleted : b->mtime);
      if (sort == FM_SORT_DATE_DOWN)
        c = -c;
      break;
    case FM_SORT_TYPE_UP:
    case FM_SORT_TYPE_DOWN:
      c = g_ascii_strcasecmp (fm_item_extension (a), fm_item_extension (b));
      if (sort == FM_SORT_TYPE_DOWN)
        c = -c;
      break;
    }
  return c ? c : compare_names (a, b);
}

typedef struct {
  FmItem *item;
  guint index;
} Indexed;

static int
compare_indexed (gconstpointer pa, gconstpointer pb, gpointer data)
{
  const Indexed *a = pa, *b = pb;
  int c = fm_item_compare (a->item, b->item, GPOINTER_TO_INT (data));

  return c ? c : (a->index < b->index ? -1 : 1);
}

void
fm_items_sort (GPtrArray *items, FmSort sort)
{
  g_autofree Indexed *all = g_new (Indexed, items->len);

  /* g_qsort_with_data is not stable everywhere: the index keeps equal rows in the backend's order */
  for (guint i = 0; i < items->len; i++)
    all[i] = (Indexed) { items->pdata[i], i };
  g_sort_array (all, items->len, sizeof (Indexed), compare_indexed, GINT_TO_POINTER (sort));
  for (guint i = 0; i < items->len; i++)
    items->pdata[i] = all[i].item;
}

static const char *const sort_names[] = {
  "None", "NameUp", "NameDown", "SizeUp", "SizeDown", "DateUp", "DateDown", "TypeUp", "TypeDown",
};

FmSort
fm_sort_from_string (const char *value)
{
  for (guint i = 0; i < G_N_ELEMENTS (sort_names); i++)
    if (!g_strcmp0 (value, sort_names[i]))
      return i;
  return FM_SORT_NAME_UP;
}

const char *
fm_sort_to_string (FmSort sort)
{
  return sort < G_N_ELEMENTS (sort_names) ? sort_names[sort] : "NameUp";
}
