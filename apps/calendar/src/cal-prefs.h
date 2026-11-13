/* The preference screens' rows: Settings, General settings, a calendar's page, and the screens that open from them
 * (cal-settings.c, cal-accounts.c).
 *
 *   cal_pref_row       a row: an icon (its space kept without one), the title, the summary under it, an
 *                      optional widget at the end
 *   cal_pref_category  a section's title in the primary colour
 *   cal_pref_switch    a switch row bound to a GSettings boolean
 *   CalListPref        a list row: the entry as the summary, a dialog of the entries */
#pragma once

#include "cal-compat-switch.h"

#include <gtk/gtk.h>
#include <neo.h>

G_BEGIN_DECLS

typedef struct {
  GtkWidget *row, *title, *summary, *end;
} CalPref;

/* @color: 0xAARRGGBB to tint the icon (a calendar's circle), 0 for the theme's */
GtkWidget *cal_pref_icon        (const char *icon_name, guint32 color);
CalPref    cal_pref_row         (const char *icon_name, guint32 color, const char *title, const char *summary,
                                 GtkWidget *end);
/* the summary, hidden when @text is NULL or empty */
void       cal_pref_set_summary (GtkWidget *summary, const char *text);
void       cal_pref_set_summary_markup (GtkWidget *summary, const char *markup);
/* the first category on a page takes the "first" class (no divider above it) */
GtkWidget *cal_pref_category    (const char *icon_name, const char *title);
/* a page (cal-page.h) with a toolbar titled @title over a scrolling list: rows go into *@list */
GtkWidget *cal_pref_page        (GtkWidget *window, const char *title, GtkWidget **list);

/* a switch row: the row toggles it; its summary label is the row's "cal-pref-summary" data */
GtkWidget *cal_pref_switch      (GSettings *settings, const char *key, const char *title);
/* a "clicked" handler for a row whose end widget is @toggle */
void       cal_pref_toggle_row  (NeoPressable *row, CalCompatSwitch *toggle);

typedef struct _CalListPref CalListPref;
typedef void (*CalListChanged) (CalListPref *list, gpointer data);

/* a list row over a string key (@is_string) or an integer one (the values as decimal strings) */
CalListPref *cal_list_pref_new         (GSettings *settings, const char *key, const char *title, gboolean is_string);
/* takes @label and @value */
void         cal_list_pref_add         (CalListPref *list, char *label, char *value);
/* after the user chose an entry (and the key has it) */
void         cal_list_pref_set_changed (CalListPref *list, CalListChanged changed, gpointer data);
/* the row; it owns @list */
GtkWidget   *cal_list_pref_widget      (CalListPref *list);

/* a reminder time's label: "None" below 0 */
char      *cal_pref_minutes_label (int minutes);
/* a list row of minutes (cal_pref_minutes_label) */
GtkWidget *cal_pref_minutes       (GSettings *settings, const char *key, const char *title, const int *values,
                                   int n_values);

G_END_DECLS
