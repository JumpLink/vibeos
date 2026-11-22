/* SwWindow: Software's window.
 *
 * Four tabs at the bottom (Featured, Apps, Games, Mine; the update count on Mine), and pages pushed over them: an
 * app's page, a list, an account's pages. Back (the top bar's arrow, Escape, Alt+Left, the Back key, VibeOS's edge
 * swipe) walks back through the pages, then to the first tab, then leaves. Back never stops an install: a job outlives
 * every page, and its progress shows on Mine.
 *
 * The functions take any widget in the window (`from`). */
#pragma once

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

enum {
  SW_TAB_FEATURED,
  SW_TAB_APPS,
  SW_TAB_GAMES,
  SW_TAB_MINE,
};

GtkWidget *sw_window_new          (GtkApplication *app);

void       sw_window_push         (GtkWidget *from, GtkWidget *page);
/* where a search opens: beside the rail on a tablet (the rail stays), else over the whole window */
GtkOverlay *sw_window_search_overlay (GtkWidget *from);
/* pops the page `from` is in (if it is the top one) */
void       sw_window_pop          (GtkWidget *from);
/* pops back to the tabs */
void       sw_window_pop_to_tabs  (GtkWidget *from);
/* pops the pages of signing in, signing up and the codes (they set the object data "sw-account-flow") */
void       sw_window_pop_account_flow (GtkWidget *from);
/* replaces the page `from` is in with `page` (a sign-in page with the next step) */
void       sw_window_replace      (GtkWidget *from, GtkWidget *page);
void       sw_window_toast        (GtkWidget *from, const char *text);
void       sw_window_show_tab     (GtkWidget *from, int tab);

/* an app's page; `summary` (an AppSummary, may be NULL) draws its head before the page is loaded */
void       sw_window_open_app     (GtkWidget *from, const char *app_id, JsonObject *summary);
/* a list of apps behind an API path ("/collections/editors-choice/apps") */
void       sw_window_open_list    (GtkWidget *from, const char *title, const char *path);
/* a banner's target: an app, a collection or a category */
void       sw_window_open_target  (GtkWidget *from, JsonObject *target, const char *title);
/* the account's page, or signing in */
void       sw_window_open_account (GtkWidget *from);
void       sw_window_open_search  (GtkWidget *from, const char *type);

/* for VIBEOS_SOFTWARE_PAGE: steps run once the window shows (see sw-main.c) */
void       sw_window_run_steps    (GtkWidget *window, const char *steps);

G_END_DECLS
