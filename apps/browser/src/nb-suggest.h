/* nb-suggest: the search suggestion providers, as Jelly has them: one GET per query, up to 5 results.
 * The History provider searches the store; None gives nothing. */
#pragma once

#include <gio/gio.h>
#include "store.h"

G_BEGIN_DECLS

#define NB_SUGGEST_LIMIT 5

typedef struct {
  char *title;
  char *url;                  /* history results only; a row's text loads `url ?: title` */
} NbSuggestion;

void nb_suggestion_free (NbSuggestion *s);

/* the providers, in the settings list's order */
extern const char *const nb_suggest_ids[];     /* BAIDU … NONE, the stored values */
extern const char *const nb_suggest_names[];   /* Baidu … None, as the list shows them */

/* `done` gets the NbSuggestion found, in an array valid for the call only (take a ref to keep it), or is not
 * called at all when `cancel` fired first */
typedef void (*NbSuggestDone) (GPtrArray *suggestions, gpointer data);
void nb_suggest_fetch (NbStore *store, const char *provider, const char *query, GCancellable *cancel,
                       NbSuggestDone done, gpointer data);

G_END_DECLS
