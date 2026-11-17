/* sw-widgets: the pieces Software's screens are made of.
 *
 *   pages      a pushed page: the top bar (back arrow, title, actions) over a body
 *   status     a body that is loading, failed (a sentence and Retry), empty, or showing its content
 *   buttons    filled, tonal, outlined and text buttons: pills with a ripple
 *   app rows   an app in a list: icon, name, summary, rating, and its install button
 *   app tiles  an app in a horizontal row: icon, name, rating
 *   chips      a filter chip ("Newest"), chosen or not
 *   stars      a rating drawn as five stars, or five stars to tap
 *   progress   a ring: a job's percent, or turning while it is not known
 *   tabs       text tabs with an underline under the chosen one
 *   fields     a text field with its label, an error line under it, and a password's eye
 *   code       six boxes for an emailed code
 *
 * Every page's requests use the page's cancellable (sw_cancellable_for): it is cancelled when the page goes, so the
 * answers' callbacks can rely on their widgets. */
#pragma once

#include "sw-api.h"

#include <gtk/gtk.h>
#include <vibeos.h>

G_BEGIN_DECLS

/* ---- pages ----------------------------------------------------------------------------------------------------- */

/* a page with a back arrow and `title`; the back arrow pops it */
GtkWidget    *sw_page_new          (const char *title);
VibeosTopBar *sw_page_get_bar      (GtkWidget *page);
/* the page's body under its bar (it fills the rest) */
void          sw_page_set_body     (GtkWidget *page, GtkWidget *body);
/* `child` in a vertical scroll view */
GtkWidget    *sw_scroll_new        (GtkWidget *child);
/* the scroll view's offset back to the top */
void          sw_scroll_to_top     (GtkWidget *scroll);
/* cancelled when `widget` is destroyed */
GCancellable *sw_cancellable_for   (GtkWidget *widget);
GtkWidget    *sw_label_new         (const char *text, const char *css_class);
/* a column with the page's side margins */
GtkWidget    *sw_column_new        (int spacing);

/* ---- status ---------------------------------------------------------------------------------------------------- */

#define SW_TYPE_STATUS (sw_status_get_type ())
G_DECLARE_FINAL_TYPE (SwStatus, sw_status, SW, STATUS, GtkWidget)

typedef void (*SwRetryFunc) (gpointer data);

GtkWidget *sw_status_new         (GtkWidget *content);
void       sw_status_set_content (SwStatus *self, GtkWidget *content);
void       sw_status_loading     (SwStatus *self);
/* the error's sentence and a Retry button that calls `retry`; offline errors get the cloud icon */
void       sw_status_error       (SwStatus *self, const SwApiError *error, SwRetryFunc retry, gpointer data);
/* a sentence (and an icon) for an empty list; `action` (NULL: none) is a button calling `retry` */
void       sw_status_empty       (SwStatus *self, const char *icon_name, const char *text, const char *action,
                                  SwRetryFunc retry, gpointer data);
void       sw_status_show        (SwStatus *self);
gboolean   sw_status_is_showing  (SwStatus *self);

/* ---- buttons --------------------------------------------------------------------------------------------------- */

typedef enum {
  SW_BUTTON_FILLED,
  SW_BUTTON_TONAL,
  SW_BUTTON_OUTLINED,
  SW_BUTTON_TEXT,
  SW_BUTTON_DANGER,      /* filled in the error colour: deleting the account */
} SwButtonStyle;

/* a pill with `label`; "clicked" as VibeosPressable */
GtkWidget *sw_button_new        (const char *label, SwButtonStyle style);
void       sw_button_set_label  (GtkWidget *button, const char *label);
void       sw_button_set_style  (GtkWidget *button, SwButtonStyle style);
/* a round icon button (the top bar's look) */
GtkWidget *sw_icon_button_new   (const char *icon_name, const char *tooltip);

/* ---- app rows and tiles ---------------------------------------------------------------------------------------- */

/* the AppSummary of `package` from any list shown since Software started (Mine draws its jobs' rows with them), or
 * NULL; every row and tile keeps its app there */
JsonObject *sw_app_cache_get    (const char *package);
void        sw_app_cache_put    (JsonObject *app);

/* an AppSummary's row; a tap opens its page. `subtitle` replaces the summary when not NULL ("Version 48.0 · 5.6 MB") */
GtkWidget *sw_app_row_new       (JsonObject *app, const char *subtitle);
/* an AppSummary's tile, `width` wide */
GtkWidget *sw_app_tile_new      (JsonObject *app, int width);
/* a horizontal row of tiles (or anything), scrolled sideways: `box`, its items SW_STRIP_SPACING apart */
#define SW_STRIP_SPACING 4
GtkWidget *sw_strip_new         (GtkWidget **box);
/* the strip's items, all `item_width` wide, come to rest with one at the start (24px in), none cut off there */
void       sw_strip_snap        (GtkWidget *strip, int item_width);
/* "Title  3                See all" over a section; `action` NULL for none */
GtkWidget *sw_section_header_new (const char *title, const char *count, const char *action, GCallback callback,
                                  gpointer data);

/* ---- the install button ---------------------------------------------------------------------------------------- */

/* Install, Update or Open for `app` (an AppSummary), or a ring while a job runs (a tap stops it). It follows
 * SwInstaller and SwUpdates by itself. */
GtkWidget *sw_install_button_new (JsonObject *app);

/* what an app is to this phone */
typedef enum {
  SW_APP_NOT_INSTALLED,
  SW_APP_INSTALLED,
  SW_APP_UPDATABLE,
  SW_APP_BUSY,           /* a job of it is queued or running */
} SwAppState;
SwAppState sw_app_state         (const char *package);
/* the label for an Install, Update or Open button */
const char *sw_app_state_action (SwAppState state);
/* runs a state's action: install, update, or open the app (`app` an AppSummary or AppDetail) */
void       sw_app_activate      (GtkWidget *from, JsonObject *app);

/* ---- chips ----------------------------------------------------------------------------------------------------- */

/* a filter chip ("Newest") that can be chosen */
GtkWidget *sw_filter_chip_new   (const char *label);
void       sw_filter_chip_set_selected (GtkWidget *chip, gboolean selected);

/* ---- stars ----------------------------------------------------------------------------------------------------- */

#define SW_TYPE_STARS (sw_stars_get_type ())
G_DECLARE_FINAL_TYPE (SwStars, sw_stars, SW, STARS, GtkWidget)

/* five stars `size` across each, `gap` apart, filled up to `rating` (halves too); with `tappable` a tap sets the
 * rating and emits "changed" */
GtkWidget *sw_stars_new          (double rating, int size, int gap, gboolean tappable);
void       sw_stars_set_rating   (SwStars *self, double rating);
double     sw_stars_get_rating   (SwStars *self);
/* "★ 4.6", or NULL for no rating */
GtkWidget *sw_rating_label_new   (JsonObject *app);

/* ---- progress -------------------------------------------------------------------------------------------------- */

#define SW_TYPE_PROGRESS (sw_progress_get_type ())
G_DECLARE_FINAL_TYPE (SwProgress, sw_progress, SW, PROGRESS, GtkWidget)

/* a ring `size` across with a stroke `width` wide; percent < 0 turns */
GtkWidget *sw_progress_new         (int size, double width);
void       sw_progress_set_percent (SwProgress *self, int percent);
/* a line rather than a ring (`size` is its height) */
GtkWidget *sw_progress_bar_new     (int height);

/* ---- tabs ------------------------------------------------------------------------------------------------------ */

#define SW_TYPE_TABS (sw_tabs_get_type ())
G_DECLARE_FINAL_TYPE (SwTabs, sw_tabs, SW, TABS, GtkWidget)

/* text tabs as wide as the window, each half; "changed" (int index) */
GtkWidget *sw_tabs_new           (const char *first, ...) G_GNUC_NULL_TERMINATED;
void       sw_tabs_set_selected  (SwTabs *self, int index);
int        sw_tabs_get_selected  (SwTabs *self);

/* ---- fields ---------------------------------------------------------------------------------------------------- */

typedef enum {
  SW_FIELD_TEXT,
  SW_FIELD_NAME,
  SW_FIELD_EMAIL,
  SW_FIELD_PASSWORD,        /* hidden, with the eye to show it */
  SW_FIELD_NEW_PASSWORD,
} SwFieldKind;

/* a labelled field; "activate" when Enter is pressed in it */
GtkWidget  *sw_field_new         (const char *label, SwFieldKind kind);
const char *sw_field_get_text    (GtkWidget *field);
void        sw_field_set_text    (GtkWidget *field, const char *text);
/* the red line under the field, NULL for none */
void        sw_field_set_error   (GtkWidget *field, const char *error);
/* a grey line under it when there is no error */
void        sw_field_set_hint    (GtkWidget *field, const char *hint);
void        sw_field_grab_focus  (GtkWidget *field);
/* the GtkText inside, for its signals */
GtkWidget  *sw_field_get_text_widget (GtkWidget *field);

/* six boxes for a code; "complete" (const char *code) once all six are in, "changed" on every change */
GtkWidget  *sw_code_field_new    (int length);
char       *sw_code_field_get_code (GtkWidget *field);
void        sw_code_field_clear  (GtkWidget *field);
void        sw_code_field_set_error (GtkWidget *field, gboolean error);

/* the api error's message on `field` when it names it, else NULL; for validation_failed, the field errors */
gboolean    sw_fields_show_errors (const SwApiError *error, ...) G_GNUC_NULL_TERMINATED;

/* ---- misc ------------------------------------------------------------------------------------------------------ */

/* a horizontal line between parts of a page */
GtkWidget *sw_divider_new        (void);
/* a circle with the account's initial, `size` across (signed out: a person) */
GtkWidget *sw_avatar_new         (int size);

G_END_DECLS
