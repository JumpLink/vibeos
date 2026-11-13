/* The rows of a contact list, worked out from the contacts (no drawing here, so that the tests can check them):
 * which contacts show, in what order, and which row starts a section and so shows its letter.
 *
 * The main list: the favourites first, under a star, in their own section with no label (so that the
 * fast scroller shows no letter for them); then everyone, the favourites again among them,
 * by the sort key, each section's first row showing its letter. A search: only the contacts that match, by
 * the sort key, with no sections and no favourites block. */
#pragma once

#include "ct-store.h"

G_BEGIN_DECLS

typedef struct {
  CtEntry *entry;               /* a reference */
  const char *name;             /* the name it shows (the entry's, in the display order) */
  int section;                  /* its section (an index into CtRows.sections), −1 in a search */
  gboolean first;               /* it starts its section: its header column shows the letter or the star */
  CtMatch match;                /* in a search: what matched */
} CtRow;

typedef struct {
  const char *label;            /* "" for the favourites, else ct_section_of's */
  int rank;                     /* ct_section_rank, CT_BUCKET_FAVOURITES for the favourites */
  guint first_row;
} CtSection;

typedef struct {
  GArray *rows;                 /* CtRow */
  GArray *sections;             /* CtSection, in list order */
} CtRows;

/* @entries: the contacts the list is for (already narrowed to a label or an account); @query: NULL or "" for the
 * whole list; @favourites: whether the starred contacts come first (not in a label's list or a picker) */
CtRows *ct_rows_new  (GPtrArray *entries, CtNameOrder display, CtNameOrder sort, const char *query,
                      gboolean favourites);
void    ct_rows_free (CtRows *rows);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (CtRows, ct_rows_free)

/* the section of row @index (for the pinned header and the fast scroller's letter); NULL in a search */
const CtSection *ct_rows_section_at (CtRows *rows, guint index);

G_END_DECLS
