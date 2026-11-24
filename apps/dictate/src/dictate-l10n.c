/* dictate-l10n: see dictate-l10n.h. A catalogue is GNU gettext's .mo: a header, then a table of the original strings
 * and one of their translations, each entry a length and an offset into the file. Each language's is read once and
 * kept, its strings pointing into the mapped file. */
#include "dictate-l10n.h"

#include <string.h>

#define MO_MAGIC         0x950412deu
#define MO_MAGIC_SWAPPED 0xde120495u

typedef struct {
  const char *text;             /* the translation: a plural's forms, one after another, each ended by a NUL */
  gsize length;                 /* up to and not counting the last form's NUL */
} Translation;

typedef struct {
  GMappedFile *file;
  GHashTable *strings;          /* msgid → its Translation, both in the file; a context's entry is "context\004msgid" */
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

/* the string at a table's entry `i` (its length in `*out_length`), or NULL when it lies outside the file */
static const char *
entry (const char *data, gsize size, guint32 table, guint32 i, gboolean swapped, gsize *out_length)
{
  gsize at = (gsize) table + (gsize) i * 8;
  guint32 length, offset;

  if (at + 8 > size)
    return NULL;
  length = word (data, at, swapped);
  offset = word (data, at + 4, swapped);
  if ((gsize) offset + length >= size || data[offset + length] != '\0')
    return NULL;
  if (out_length)
    *out_length = length;
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
      const char *msgid = entry (data, size, originals, i, swapped, NULL);
      gsize length = 0;
      const char *msgstr = entry (data, size, translations, i, swapped, &length);

      /* the header (an empty msgid) is no message; a plural's key is "msgid\0msgid_plural", so it is found by its
       * singular, and its forms follow one another in the translation */
      if (msgid && msgstr && *msgid && *msgstr)
        {
          Translation *t = g_new (Translation, 1);

          t->text = msgstr;
          t->length = length;
          g_hash_table_insert (c->strings, (gpointer) msgid, t);
        }
    }
  return c;
}

/* the translation of `key` in `language`, or NULL; the catalogue's, so valid for the life of the app */
static const Translation *
lookup (const char *language, const char *key)
{
  Catalogue *c;
  const Translation *found;

  if (!language || !*language || !key)
    return NULL;
  G_LOCK (catalogues);
  if (!catalogues)
    catalogues = g_hash_table_new (g_str_hash, g_str_equal);
  if (!g_hash_table_lookup_extended (catalogues, language, NULL, (gpointer *) &c))
    {
      c = catalogue_load (language);
      g_hash_table_insert (catalogues, g_strdup (language), c);
    }
  found = c ? g_hash_table_lookup (c->strings, key) : NULL;
  G_UNLOCK (catalogues);
  return found;
}

const char *
dictate_l10n (const char *language, const char *msgid)
{
  const Translation *t = msgid ? lookup (language, msgid) : NULL;

  return t ? t->text : msgid;
}

const char *
dictate_l10n_c (const char *language, const char *context, const char *msgid)
{
  g_autofree char *key = NULL;
  const Translation *t;

  if (!context || !msgid)
    return dictate_l10n (language, msgid);
  key = g_strconcat (context, "\004", msgid, NULL);
  t = lookup (language, key);
  return t ? t->text : msgid;
}

/* which of a language's forms `n` takes: the Plural-Forms rule of the catalogues Vibe ships */
static guint
plural_form (const char *language, gulong n)
{
  if (!g_strcmp0 (language, "fr"))
    return n > 1;
  if (!g_strcmp0 (language, "ru"))
    {
      if (n % 10 == 1 && n % 100 != 11)
        return 0;
      return n % 10 >= 2 && n % 10 <= 4 && (n % 100 < 10 || n % 100 >= 20) ? 1 : 2;
    }
  return n != 1;
}

const char *
dictate_l10n_n (const char *language, const char *msgid, const char *msgid_plural, gulong n)
{
  const Translation *t = msgid ? lookup (language, msgid) : NULL;
  guint want = plural_form (language, n);
  const char *form;

  if (!t)
    return n == 1 ? msgid : msgid_plural;
  /* the wanted form, or the last there is when the catalogue has fewer */
  form = t->text;
  for (guint i = 0; i < want; i++)
    {
      const char *next = form + strlen (form) + 1;

      if ((gsize) (next - t->text) > t->length)
        break;
      form = next;
    }
  return form;
}
