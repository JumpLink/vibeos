/* sw-pages: Software's screens. Each returns a page for SwWindow (or a tab's content). */
#pragma once

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* ---- the tabs (sw-featured.c, sw-mine.c) ----------------------------------------------------------------------- */

typedef enum {
  SW_HOME_FEATURED,      /* GET /featured */
  SW_HOME_APPS,          /* GET /browse/apps, and Popular: GET /apps?type=app */
  SW_HOME_GAMES,         /* GET /browse/games, and Popular: GET /apps?type=game */
} SwHomeKind;

GtkWidget *sw_home_new             (SwHomeKind kind);
/* back to the top, and the first sub-tab; FALSE if it was there already */
gboolean   sw_home_reset           (GtkWidget *home);
GtkWidget *sw_mine_new             (void);

/* ---- lists (sw-list.c) ----------------------------------------------------------------------------------------- */

/* a page listing the apps behind `path`, page after page as it scrolls */
GtkWidget *sw_list_page_new        (const char *title, const char *path);
/* the list alone (a body without a top bar), for the Popular sub-tab and search */
GtkWidget *sw_app_list_new         (const char *path);
/* called with the first page's "meta" (search: the corrected query) */
typedef void (*SwListMeta) (GtkWidget *list, JsonObject *meta, gpointer data);
void       sw_app_list_set_meta_func (GtkWidget *list, SwListMeta func, gpointer data);
/* the sentence when there are no apps at all (default "No apps here yet.") */
void       sw_app_list_set_empty   (GtkWidget *list, const char *text);
/* a line over the rows ("Showing results for chess"), or NULL */
void       sw_app_list_set_header  (GtkWidget *list, GtkWidget *header);
/* every category of `type` ("apps", "games") */
GtkWidget *sw_categories_page_new  (const char *type);

/* ---- an app (sw-app-page.c, sw-reviews.c) ---------------------------------------------------------------------- */

GtkWidget *sw_app_page_new         (const char *app_id, JsonObject *summary);
GtkWidget *sw_reviews_page_new     (const char *app_id, const char *app_name);
/* writing a review; `rating` the stars already tapped (0 none), `review` the account's own (NULL: none) */
GtkWidget *sw_review_editor_new    (const char *app_id, const char *app_name, int rating, JsonObject *review);
/* the sheet for reporting a review */
void       sw_report_present       (GtkWidget *from, gint64 review_id);
/* a review of `app_id` was written, changed or deleted: its page reloads when it shows again */
void       sw_reviews_changed      (const char *app_id);
gboolean   sw_reviews_take_changed (const char *app_id);
/* a review's card; `app_id` lets it be edited when it is the account's own */
GtkWidget *sw_review_card_new      (JsonObject *review, const char *app_id, const char *app_name);

/* ---- search (sw-search.c) -------------------------------------------------------------------------------------- */

/* opens the search view over the window; `type` "app", "game" or NULL for both */
void       sw_search_open          (GtkWidget *from, const char *type, const char *text);
/* the results of `query` as a page */
GtkWidget *sw_search_results_new   (const char *query, const char *type);

/* ---- Mine (sw-mine.c) ------------------------------------------------------------------------------------------ */

/* On this device | Not on this device */
GtkWidget *sw_all_apps_page_new    (int tab);

/* ---- the account (sw-account-pages.c) -------------------------------------------------------------------------- */

GtkWidget *sw_sign_in_page_new     (void);
GtkWidget *sw_register_page_new    (void);
/* entering the emailed code that confirms the account's address */
GtkWidget *sw_verify_page_new      (gboolean just_sent);
GtkWidget *sw_forgot_page_new      (const char *email);
GtkWidget *sw_profile_page_new     (void);
GtkWidget *sw_delete_account_page_new (void);
GtkWidget *sw_my_reviews_page_new  (void);
GtkWidget *sw_settings_page_new    (void);
void       sw_about_present        (GtkWidget *from);

G_END_DECLS
