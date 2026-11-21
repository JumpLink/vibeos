/* The window: the toolbar (normal, search or selection), the page that shows (Home, Remote, a folder,
 * Settings), the bottom bar (selection or paste), the drawer over them, and the snackbar.
 *
 * FmShell is what every screen of the window shares: the settings, the remotes, what the app remembers, and the
 * way to go somewhere. There is one per window, found from any of its widgets with fm_shell_get. */
#pragma once

#include <gtk/gtk.h>

#include "fm-places.h"
#include "fm-remotes.h"
#include "fm-state.h"

G_BEGIN_DECLS

typedef enum {
  FM_PAGE_HOME,
  FM_PAGE_REMOTE,
  FM_PAGE_BROWSER,
  FM_PAGE_SETTINGS,
} FmPageKind;

typedef struct _FmBrowser FmBrowser;
typedef struct _FmDrawer FmDrawer;
typedef struct _FmHome FmHome;
typedef struct _FmRemotePage FmRemotePage;
typedef struct _FmShell FmShell;

struct _FmShell {
  GtkWidget *window;
  GSettings *settings;
  FmState *state;
  GPtrArray *remotes;           /* FmRemote */
  char *remotes_path;
  GVolumeMonitor *monitor;

  FmPageKind page;
  GtkWidget *pages;             /* GtkStack */
  FmBrowser *browser;
  FmHome *home;
  FmRemotePage *remote_page;
  GtkWidget *settings_page;
  FmDrawer *drawer;

  /* the toolbar */
  GtkWidget *toolbar;           /* GtkStack: "normal", "search", "select" */
  GtkWidget *nav, *title, *actions;
  GtkWidget *search_entry, *search_spinner;
  GtkWidget *select_title, *select_actions;
  /* the bottom bar: "select" or "paste" */
  GtkWidget *bottom, *bottom_items;
  GtkWidget *snackbar;
  GtkWidget *job_line;          /* a hidden job's thin progress line */

  /* paste mode: what Copy or Cut took */
  GPtrArray *clip;              /* GFile */
  gboolean clip_cut;

  GPtrArray *back;              /* the places gone through, for Back */
};

FmShell    *fm_shell_get          (GtkWidget *widget);
GtkWidget  *fm_window_new         (GtkApplication *app, GSettings *settings);

/* goes there: a folder (or a share, a server, the Recycle Bin ...) in the browser */
void        fm_shell_open_location (FmShell *shell, GFile *location);
void        fm_shell_show_page    (FmShell *shell, FmPageKind page);
/* a file: its default app; a folder: open_location */
void        fm_shell_open_item_location (FmShell *shell, GFile *location, gboolean is_folder);
/* ... a file opening out of @from (its icon in a row, or NULL), as an app out of its icon on the home screen */
void        fm_shell_open_item_from (FmShell *shell, GFile *location, gboolean is_folder, GtkWidget *from);
/* Back: the popup, the drawer, selection, search, then the place before; FALSE when there is none */
gboolean    fm_shell_back         (FmShell *shell);

/* the bars again after the page, its selection or its search changed */
void        fm_shell_update_bars  (FmShell *shell);
void        fm_shell_toast        (FmShell *shell, const char *text);
/* the snackbar at the bottom (a job's result); @action (UNDO) runs @action_cb once when tapped. The shell keeps
 * @data until the next snackbar or the window goes, then gives it to @destroy. */
typedef void (*FmSnackAction) (FmShell *shell, gpointer data);
void        fm_shell_snackbar     (FmShell *shell, const char *text, const char *action, FmSnackAction action_cb,
                                   gpointer data, GDestroyNotify destroy);
/* a hidden job's progress line: a fraction, or < 0 to take it away */
void        fm_shell_job_progress (FmShell *shell, double fraction);

/* the remotes were changed (added, edited, removed): saved, and the screens that list them again */
void        fm_shell_remotes_changed (FmShell *shell);
FmRemote   *fm_shell_find_remote  (FmShell *shell, const char *id);
/* the user-visible mounts (GMount, owned) */
GList      *fm_shell_mounts       (FmShell *shell);
FmPlace    *fm_shell_place_for    (FmShell *shell, GFile *file);

/* paste mode: Copy or Cut put @files on the clipboard; NULL leaves paste mode */
void        fm_shell_set_clip     (FmShell *shell, GPtrArray *files, gboolean cut);

/* the listings again (a job ended, a setting changed) */
void        fm_shell_reload       (FmShell *shell);

G_END_DECLS
