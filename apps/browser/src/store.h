/* The browser's JSON store: history (newest first, one entry per URL), favourites (bookmarks.json, newest first),
 * preferences and the session, in ~/.local/share/de.yesman.NeoBrowser: the same files the GJS and the Chrome-style
 * browsers kept, in the same formats, so nothing is lost in the switch (spec/jelly.md §15). Favourites may carry a
 * "color" (Jelly's card colour); preferences keep every member they had, known or not. */
#pragma once

#include <glib.h>
#include <json-glib/json-glib.h>

typedef struct {
  char  *url;
  char  *title;
  double time;      /* seconds since the epoch */
  int    visits;    /* history only */
  char  *color;     /* favourites: "#rrggbb", or NULL (drawn in Jelly's default) */
} NbEntry;

typedef struct {
  char       *dir;
  GPtrArray  *history;     /* NbEntry, newest first */
  GPtrArray  *bookmarks;   /* NbEntry, newest first */
  JsonObject *prefs;       /* adblock, filterTime, and whatever else was there */
  GHashTable *pending;     /* file name -> source id of a batched write */
} NbStore;

NbStore    *nb_store_new          (const char *dir, const char *old_dir);
void        nb_store_flush        (NbStore *store);
void        nb_store_free         (NbStore *store);

JsonNode   *nb_store_load         (NbStore *store, const char *name);   /* NULL when missing or unreadable */
void        nb_store_write_node   (NbStore *store, const char *name, JsonNode *node);

void        nb_store_visit        (NbStore *store, const char *url, const char *title);
void        nb_store_retitle      (NbStore *store, const char *url, const char *title);
void        nb_store_clear_history(NbStore *store);
GPtrArray  *nb_store_suggest      (NbStore *store, const char *text, guint limit);   /* borrowed NbEntry */
GPtrArray  *nb_store_top_sites    (NbStore *store, guint limit);                     /* borrowed NbEntry */
gboolean    nb_store_is_bookmarked(NbStore *store, const char *url);
void        nb_store_toggle_bookmark (NbStore *store, const char *url, const char *title);
/* FavoriteProvider.addOrUpdateItem: a URL there already gets the new title (and colour), else it goes first */
void        nb_store_add_favorite (NbStore *store, const char *url, const char *title, const char *color);
/* the favourite `old_url` becomes `url` / `title` (Edit favorite) */
void        nb_store_edit_favorite (NbStore *store, const char *old_url, const char *url, const char *title);
void        nb_store_remove_favorite (NbStore *store, const char *url);
/* history rows: one removed (the swipe), and put back where it was (Undo) */
NbEntry    *nb_store_take_history (NbStore *store, guint index);
void        nb_store_put_history  (NbStore *store, guint index, NbEntry *entry);
void        nb_entry_free         (NbEntry *entry);

gboolean    nb_store_adblock      (NbStore *store);
void        nb_store_set_adblock  (NbStore *store, gboolean on);
double      nb_store_filter_time  (NbStore *store);
void        nb_store_set_filter_time (NbStore *store, double t);

/* Jelly's settings, beside the old ones in prefs.json */
gboolean    nb_store_get_bool     (NbStore *store, const char *key, gboolean fallback);
void        nb_store_set_bool     (NbStore *store, const char *key, gboolean value);
const char *nb_store_get_string   (NbStore *store, const char *key, const char *fallback);
void        nb_store_set_string   (NbStore *store, const char *key, const char *value);
GStrv       nb_store_get_strv     (NbStore *store, const char *key);
void        nb_store_set_strv     (NbStore *store, const char *key, const char *const *value);
/* a choice remembered per origin under the object `key` ({"https://a.org/": true}): -1 when there is none */
int         nb_store_get_origin   (NbStore *store, const char *key, const char *origin);
void        nb_store_set_origin   (NbStore *store, const char *key, const char *origin, gboolean value);

/* "example.org" from "https://www.example.org/a" (the text itself when it has no scheme) */
char       *nb_host               (const char *url);
/* the host's first letter, upper case: a site's stand-in icon */
char       *nb_letter             (const char *url);
