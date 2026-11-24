/* VibeOS Settings' building blocks, on libvibeos: pages with a top bar over their cards, groups as segmented cards
 * (VibeosCardList), rows of a title and a summary with an icon or a switch (VibeosListRow). On a tablet the main menu
 * and the page of the entry chosen in it are side by side (VibeosListDetail); on a phone the pages open over it. */
#pragma once

#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <vibeos.h>

/* a page: its top bar over its scrolled content */
typedef GtkWidget NsPage;

typedef void (*NsActivate) (GtkWidget *row, gpointer data);

/* the app's navigation: the main menu and the pages opened from it */
void       ns_ui_init (VibeosListDetail *ld);
VibeosListDetail *ns_ui_get_list_detail (void);
/* opens a page one level deeper than the page on top */
void       ns_push (NsPage *page);
/* runs `page` (an entry's page function) so that the page it opens is the entry's: beside the menu on a tablet,
 * without a back arrow there, the row selected */
void       ns_open_entry (NsActivate page, GtkWidget *row, gpointer data);
/* Back: closes the page on top; FALSE when there is nothing to close (Back then leaves the app) */
gboolean   ns_back (void);
/* closes the page `widget` is on, when it is the page on top */
void       ns_pop (GtkWidget *widget);
/* closes the pages over `page` */
void       ns_pop_to (NsPage *page);
/* closes every page, back to the main menu */
void       ns_pop_all (void);
NsPage    *ns_top_page (void);
/* the page `widget` is on, or NULL */
NsPage    *ns_page_of (GtkWidget *widget);
const char *ns_page_get_title (NsPage *page);
/* a page that must not close now (a password being set): Back leaves it open, and the menu opens nothing else */
void       ns_page_set_busy (NsPage *page, gboolean busy);
gboolean   ns_busy (void);
/* `back` runs when the page is on top again, after the page over it closed */
typedef void (*NsPageBack) (NsPage *page);
void       ns_page_on_back (NsPage *page, NsPageBack back);

/* a page: the title in the bar, a back arrow, `content` is the column to add groups to */
NsPage    *ns_page_new (const char *title, GtkWidget **content);
/* a page that is one long list: the list scrolls */
NsPage    *ns_page_new_list (const char *title, GtkWidget *list);
/* a page whose content does not scroll (a pattern is drawn on it) */
NsPage    *ns_page_new_fixed (const char *title, GtkWidget *child);

/* `child` scrolled the way the launcher's app drawer and the shade scroll (libspongy: a fling, and a stretch at
 * the ends), or in a GtkScrolledWindow where libspongy is not installed */
GtkWidget *ns_scroller_new (GtkWidget *child);

/* a card of rows; `title` (or NULL) goes above it, in the accent colour */
GtkWidget *ns_group (GtkWidget *content, const char *title);
/* `icon` NULL: no icon. A page's rows have plain icons in the theme's colours; the main menu's circles are its own */
GtkWidget *ns_row (GtkWidget *group, const char *icon, const char *title, const char *subtitle);
void       ns_row_on_activate (GtkWidget *row, NsActivate cb, gpointer data);
/* something at the row's end: a button takes its own taps, anything else is only shown */
void       ns_row_suffix (GtkWidget *row, GtkWidget *suffix);
/* a small loading indicator (Material 3's indeterminate circular one), for a row's end while its value loads */
GtkWidget *ns_spinner_new (void);
void       ns_row_set_title (GtkWidget *row, const char *title);
void       ns_row_set_subtitle (GtkWidget *row, const char *subtitle);
GtkWidget *ns_row_switch (GtkWidget *group, const char *title, const char *subtitle, GSettings *s, const char *key);
/* a switch at the row's end, returned. `split`: the row opens a page (ns_row_on_activate) and the switch toggles on
 * its own; otherwise a tap anywhere on the row toggles it */
GtkWidget *ns_row_add_switch (GtkWidget *row, gboolean split);
/* a row of its own widget (a slider, a grid) under an optional title */
GtkWidget *ns_row_widget (GtkWidget *group, const char *title, GtkWidget *child);
/* an integer key as a −/+ stepper */
GtkWidget *ns_row_spin (GtkWidget *group, const char *title, GSettings *s, const char *key, int lo, int hi);
/* a string key from a fixed list: a row that shows the choice and opens a picker page. The labels are a static list
 * of N_() strings, translated here (as ns_row_pick's labels and summaries are) */
GtkWidget *ns_row_choice (GtkWidget *group, const char *title, GSettings *s, const char *key,
                          const char *const *values, const char *const *labels);
/* the same with labels shown as they are (ns_row_pick_names) */
GtkWidget *ns_row_choice_names (GtkWidget *group, const char *title, GSettings *s, const char *key,
                                const char *const *values, const char *const *labels);

/* a switch row with an icon, for a switch the caller drives (ns_switch_quiet sets it without its handler acting) */
GtkWidget *ns_row_toggle (GtkWidget *group, const char *icon, const char *title, const char *subtitle);
void       ns_switch_quiet (GtkWidget *sw, gboolean on);
gboolean   ns_switch_syncing (GtkWidget *sw);
gboolean   ns_switch_get_active (GtkWidget *sw);
void       ns_switch_set_active (GtkWidget *sw, gboolean on);
/* the row's switch (of ns_row_switch, ns_row_toggle, ns_row_add_switch) */
GtkWidget *ns_row_get_switch (GtkWidget *row);
/* one flag of a flags key (or one member of a string list) as a switch */
GtkWidget *ns_row_flag (GtkWidget *group, const char *title, const char *subtitle, GSettings *s, const char *key,
                        const char *flag);
/* a row with an app's own icon at its start */
GtkWidget *ns_row_app (GtkWidget *group, GAppInfo *app, const char *subtitle);
/* a value: the title with the value under it, not tappable */
GtkWidget *ns_row_info (GtkWidget *group, const char *title, const char *value);
/* an action in a card: the title alone, in the accent colour (the error colour when `destructive`) */
GtkWidget *ns_row_button (GtkWidget *group, const char *title, gboolean destructive, NsActivate cb, gpointer data);
/* a slider from `lo` to `hi` under the title, an icon in front; the GtkScale is returned in `scale` */
GtkWidget *ns_row_scale (GtkWidget *group, const char *title, const char *icon, double lo, double hi, double step,
                         GtkWidget **scale);
/* Android 17's volume row: the title, then the slider with the icon at its end */
GtkWidget *ns_row_volume (GtkWidget *group, const char *title, const char *icon, double lo, double hi, double step,
                          GtkWidget **scale);
/* a choice of `n` steps on a slider under the title (Font size, Display size), with − and + at its ends; the
 * GtkScale (0 to n − 1, in whole steps) is returned in `scale` */
GtkWidget *ns_row_steps (GtkWidget *group, const char *title, guint n, guint selected, GtkWidget **scale);
/* a line under the title of ns_row_steps (the step chosen, why it cannot change), or none */
void       ns_row_steps_set_value_text (GtkWidget *row, const char *text);
/* a choice by position: a row that shows labels[selected] and opens a picker page; `labels` is a static list of N_()
 * strings, translated here (and copied); `picked` gets the position */
typedef void (*NsPicked) (GtkWidget *row, guint index, gpointer data);
GtkWidget *ns_row_pick (GtkWidget *group, const char *title, const char *const *labels, guint selected,
                        NsPicked picked, gpointer data);
/* the same with labels shown as they are (copied): names found at run time (an app's, a display mode's, a sound's, a
 * language's), text that is never translated ("4 × 5", "PEAP") or that the caller translated. Only strings marked
 * for translation are looked up: a name that happened to be one of them would come out translated. */
GtkWidget *ns_row_pick_names (GtkWidget *group, const char *title, const char *const *labels, guint selected,
                              NsPicked picked, gpointer data);
void       ns_row_pick_select (GtkWidget *row, guint selected);
/* what the row says under its title for each choice, in place of the choice's label ("After 1 minute of
 * inactivity" for "1 minute"); copied, as many as the labels */
void       ns_row_pick_set_summaries (GtkWidget *row, const char *const *summaries);
/* a check mark at a picker row's end: shown on the row picked */
GtkWidget *ns_mark_new (gboolean on);
/* a string or integer key, typed in a dialog */
GtkWidget *ns_row_entry (GtkWidget *group, const char *title, GSettings *s, const char *key);

typedef void (*NsEntered) (const char *text, gpointer data);
/* a form field the caller keeps: shows the value ("Not set", dots when `secret`), a tap asks for a new one and
 * `changed` (or NULL) gets it */
GtkWidget  *ns_row_value (GtkWidget *group, const char *title, const char *value, gboolean secret,
                          NsEntered changed, gpointer data);
const char *ns_row_value_get (GtkWidget *row);
void        ns_row_value_set (GtkWidget *row, const char *value);

/* a dim line of text below the cards so far */
GtkWidget *ns_note (GtkWidget *content, const char *text);
/* Android's footnote at a page's end: an info mark, the text under it; the label is returned */
GtkWidget *ns_footnote (GtkWidget *content, const char *text);
/* removes a card's rows (a list that is rebuilt); shows or hides a card with its title */
void       ns_group_clear (GtkWidget *group);
void       ns_group_set_visible (GtkWidget *group, gboolean visible);
void       ns_group_remove (GtkWidget *group, GtkWidget *row);

/* a switch for a row of the caller's own */
GtkWidget *ns_switch_new (void);
/* a button with a label: `filled` the accent's, otherwise tonal */
GtkWidget *ns_button_new (const char *label, gboolean filled);

/* dialogs over the page `parent` is on. ns_ask: a line of text, `done` gets it, or NULL when cancelled */
void       ns_ask (GtkWidget *parent, const char *heading, const char *body, const char *initial, gboolean secret,
                   const char *ok, NsEntered done, gpointer data);
typedef void (*NsAnswered) (gboolean yes, gpointer data);
GtkWidget *ns_confirm (GtkWidget *parent, const char *heading, const char *body, const char *yes,
                       gboolean destructive, NsAnswered done, gpointer data);
/* closes a dialog of ns_confirm as if cancelled */
void       ns_dialog_close (GtkWidget *dialog);

/* a page whose lists follow a service: ns_later (page) fills it again 250 ms after the last call */
typedef void (*NsFill) (NsPage *page);
void       ns_later_init (NsPage *page, NsFill fill);
void       ns_later (NsPage *page);

void       ns_spawn (const char *const *argv);
/* a settings schema if it is installed, else NULL */
GSettings *ns_settings (const char *schema);
gboolean   ns_settings_exists (const char *schema);
/* the same, owned by the page (freed with it) */
GSettings *ns_page_settings (NsPage *page, const char *schema);
/* whether the installed schema has `key` (an older one may not) */
gboolean   ns_has_key (GSettings *s, const char *key);
/* an app id as the notification keys' paths take it (GNOME Shell's way): no .desktop, lower case, anything but
 * letters, digits and '-' made '-', and no two '-' in a row */
char      *ns_munge_app_id (const char *id);
/* a texture of a pixbuf (gdk_texture_new_for_pixbuf is deprecated); any thread */
GdkTexture *ns_texture_for_pixbuf (GdkPixbuf *pb);
/* the first line a program prints, or NULL */
char      *ns_run_line (const char *const *argv);
