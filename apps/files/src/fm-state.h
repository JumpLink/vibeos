/* What the app remembers besides its settings (spec §4.2, §4.3, §4.1, §12): the bookmarks
 * ($XDG_CONFIG_HOME/neo-files/bookmarks.ini), and the folders visited and the last-visited locations
 * ($XDG_STATE_HOME/neo-files/state.ini). Every change is written at once. No GTK: the tests link it alone. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define FM_HISTORY_MAX 50
#define FM_VISITED_MAX 10       /* unpinned last-visited locations kept */

typedef struct {
  char *uri;
  char *name;
} FmBookmark;

typedef struct {
  char *uri;                    /* the folder */
  char *root_name;              /* its location's name ("Main storage", "PLEX") */
  gint64 time;                  /* g_get_real_time: microseconds */
} FmHistoryEntry;

typedef struct {
  char *root_uri;               /* the location's root */
  char *name;                   /* its name */
  char *uri;                    /* the folder last visited in it */
  char *path;                   /* that folder below the root, as shown ("/VIDEO") */
  gboolean pinned;
  gint64 time;                  /* microseconds */
} FmVisited;

typedef struct _FmState FmState;

FmState    *fm_state_new          (const char *config_dir, const char *state_dir);
void        fm_state_free         (FmState *state);

/* FmBookmark, in their order */
GPtrArray  *fm_state_bookmarks    (FmState *state);
gboolean    fm_state_add_bookmark (FmState *state, const char *uri, const char *name);
void        fm_state_rename_bookmark (FmState *state, guint index, const char *name);
void        fm_state_remove_bookmark (FmState *state, guint index);
gboolean    fm_state_is_bookmarked (FmState *state, const char *uri);

/* FmHistoryEntry, newest first */
GPtrArray  *fm_state_history      (FmState *state);
void        fm_state_add_history  (FmState *state, const char *uri, const char *root_name);
void        fm_state_clear_history (FmState *state);

/* FmVisited: the pinned first, then the newest */
GPtrArray  *fm_state_visited      (FmState *state);
void        fm_state_visit        (FmState *state, const char *root_uri, const char *name, const char *uri,
                                   const char *path);
void        fm_state_pin_visited  (FmState *state, const char *root_uri, gboolean pinned);
void        fm_state_remove_visited (FmState *state, const char *root_uri);

G_END_DECLS
