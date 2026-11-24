/* dictate-l10n: see dictate-l10n.h. A catalogue is GNU gettext's .mo: a header, then a table of the original strings
 * and one of their translations, each entry a length and an offset into the file. Each language's is read once and
 * kept, its strings pointing into the mapped file. */
#include "dictate-l10n.h"

#include <string.h>

#define MO_MAGIC         0x950412deu
#define MO_MAGIC_SWAPPED 0xde120495u

typedef struct {
  GMappedFile *file;
  GHashTable *strings;          /* msgid → its translation, both in the file */
} Catalogue;

static GHashTable *catalogues;  /* language code → Catalogue, NULL for none */
G_LOCK_DEFINE_STATIC (catalogues);

static guint32
word (const char *data, gsize at, gboolean swapped)
{
  guint32 value;

  memcpy (&value, data + at, 4);
  return swapped ? GUINT32_SWAP_LE_BE (value) : value;
}

/* the string at a table's entry `i`, or NULL when it lies outside the file */
static const char *
entry (const char *data, gsize size, guint32 table, guint32 i, gboolean swapped)
{
  gsize at = (gsize) table + (gsize) i * 8;
  guint32 length, offset;

  if (at + 8 > size)
    return NULL;
  length = word (data, at, swapped);
  offset = word (data, at + 4, swapped);
  if ((gsize) offset + length >= size || data[offset + length] != '\0')
    return NULL;
  return data + offset;
}

static Catalogue *
catalogue_load (const char *language)
{
  g_autofree char *path = g_build_filename (LOCALEDIR, language, "LC_MESSAGES", GETTEXT_PACKAGE ".mo", NULL);
  GMappedFile *file = g_mapped_file_new (path, FALSE, NULL);
  const char *data;
  gsize size;
  guint32 magic, n, originals, translations;
  gboolean swapped;
  Catalogue *c;

  if (!file)
    return NULL;
  data = g_mapped_file_get_contents (file);
  size = g_mapped_file_get_length (file);
  if (size < 28)
    {
      g_mapped_file_unref (file);
      return NULL;
    }
  memcpy (&magic, data, 4);
  if (magic != MO_MAGIC && magic != MO_MAGIC_SWAPPED)
    {
      g_mapped_file_unref (file);
      return NULL;
    }
  swapped = magic == MO_MAGIC_SWAPPED;
  n = word (data, 8, swapped);
  originals = word (data, 12, swapped);
  translations = word (data, 16, swapped);
  c = g_new0 (Catalogue, 1);
  c->file = file;
  c->strings = g_hash_table_new (g_str_hash, g_str_equal);
  for (guint32 i = 0; i < n; i++)
    {
      const char *msgid = entry (data, size, originals, i, swapped);
      const char *msgstr = entry (data, size, translations, i, swapped);

      /* the header (an empty msgid) is no message; a plural's first form is the one */
      if (msgid && msgstr && *msgid && *msgstr)
        g_hash_table_insert (c->strings, (gpointer) msgid, (gpointer) msgstr);
    }
  return c;
}

const char *
dictate_l10n (const char *language, const char *msgid)
{
  Catalogue *c;
  const char *found;

  if (!language || !*language || !msgid)
    return msgid;
  G_LOCK (catalogues);
  if (!catalogues)
    catalogues = g_hash_table_new (g_str_hash, g_str_equal);
  if (!g_hash_table_lookup_extended (catalogues, language, NULL, (gpointer *) &c))
    {
      c = catalogue_load (language);
      g_hash_table_insert (catalogues, g_strdup (language), c);
    }
  found = c ? g_hash_table_lookup (c->strings, msgid) : NULL;
  G_UNLOCK (catalogues);
  return found ? found : msgid;
}
