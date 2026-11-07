/* Neo Browser's JSON store: history (newest first, one entry per URL), bookmarks, preferences and the session, in
 * ~/.local/share/de.yesman.NeoBrowser, the same files the GJS browser kept, so nothing is lost in the switch. */
#pragma once

#include <glib.h>
#include <json-glib/json-glib.h>

typedef struct {
  char  *url;
  char  *title;
  double time;      /* seconds since the epoch */
  int    visits;    /* history only */
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

gboolean    nb_store_adblock      (NbStore *store);
void        nb_store_set_adblock  (NbStore *store, gboolean on);
double      nb_store_filter_time  (NbStore *store);
void        nb_store_set_filter_time (NbStore *store, double t);

/* "example.org" from "https://www.example.org/a" (the text itself when it has no scheme) */
char       *nb_host               (const char *url);
/* the host's first letter, upper case: a site's stand-in icon */
char       *nb_letter             (const char *url);
