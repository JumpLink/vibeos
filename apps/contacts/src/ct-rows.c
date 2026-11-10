/* The rows of a contact list (ct-rows.h). */
#include "ct-rows.h"

#include <string.h>

typedef struct {
  CtNameOrder sort;
} SortBy;

static int
by_sort_key (gconstpointer a, gconstpointer b, gpointer data)
{
  const CtEntry *x = *(CtEntry *const *) a, *y = *(CtEntry *const *) b;
  const SortBy *by = data;
  int order = strcmp (x->sort_key[by->sort], y->sort_key[by->sort]);

  /* two people with the same name keep one order from one load to the next */
  return order ? order : g_strcmp0 (x->uid, y->uid);
}

static void
row_clear (gpointer data)
{
  CtRow *row = data;

  g_clear_pointer (&row->entry, ct_entry_unref);
  ct_match_clear (&row->match);
}

static void
add_row (CtRows *rows, CtEntry *entry, CtNameOrder display, const char *label, int rank)
{
  CtRow row = { .entry = ct_entry_ref (entry), .name = entry->name[display], .section = -1 };
  CtSection *last = rows->sections->len ? &g_array_index (rows->sections, CtSection, rows->sections->len - 1) : NULL;

  row.match.name_start = row.match.name_length = -1;
  if (label)
    {
      /* a new section where the rank changes: the two "…" sections have the same label but not the same place */
      if (!last || last->rank != rank)
        {
          CtSection section = { .label = label, .rank = rank, .first_row = rows->rows->len };

          g_array_append_val (rows->sections, section);
          row.first = TRUE;
        }
      row.section = (int) rows->sections->len - 1;
    }
  g_array_append_val (rows->rows, row);
}

CtRows *
ct_rows_new (GPtrArray *entries, CtNameOrder display, CtNameOrder sort, const char *query, gboolean favourites)
{
  CtRows *rows = g_new0 (CtRows, 1);
  g_autoptr(GPtrArray) sorted = g_ptr_array_copy (entries, NULL, NULL);
  SortBy by = { sort };
  g_autofree char *trimmed = g_strstrip (g_strdup (query ? query : ""));

  /* a shallow copy, but g_ptr_array_copy keeps the original's free function: the entries are not this one's */
  g_ptr_array_set_free_func (sorted, NULL);
  rows->rows = g_array_new (FALSE, TRUE, sizeof (CtRow));
  g_array_set_clear_func (rows->rows, row_clear);
  rows->sections = g_array_new (FALSE, TRUE, sizeof (CtSection));
  g_ptr_array_sort_with_data (sorted, by_sort_key, &by);

  if (*trimmed)
    {
      for (guint i = 0; i < sorted->len; i++)
        {
          CtEntry *entry = sorted->pdata[i];
          CtMatch match;

          if (!ct_contact_match (entry->contact, entry->name[display], trimmed, &match))
            continue;
          add_row (rows, entry, display, NULL, 0);
          g_array_index (rows->rows, CtRow, rows->rows->len - 1).match = match;
        }
      return rows;
    }

  if (favourites)
    for (guint i = 0; i < sorted->len; i++)
      {
        CtEntry *entry = sorted->pdata[i];

        if (entry->starred)
          add_row (rows, entry, display, "", CT_BUCKET_FAVOURITES);
      }
  for (guint i = 0; i < sorted->len; i++)
    {
      CtEntry *entry = sorted->pdata[i];

      add_row (rows, entry, display, entry->section[sort], entry->section_rank[sort]);
    }
  return rows;
}

void
ct_rows_free (CtRows *rows)
{
  if (!rows)
    return;
  g_array_unref (rows->rows);
  g_array_unref (rows->sections);
  g_free (rows);
}

const CtSection *
ct_rows_section_at (CtRows *rows, guint index)
{
  const CtRow *row;

  if (!rows || index >= rows->rows->len)
    return NULL;
  row = &g_array_index (rows->rows, CtRow, index);
  return row->section < 0 ? NULL : &g_array_index (rows->sections, CtSection, row->section);
}
