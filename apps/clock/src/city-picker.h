/* CityPicker: the cities picker (DeskClock's CitySelectionActivity, L/cities_activity.xml, L/city_list_item.xml;
 * spec/clock.md §6.2), opened by the Clock tab's FAB.
 *
 * Every city of the list, each row with its section index, a check box, the name and the time there now. The cities
 * chosen when the page opened come first, under "Selected Cities", by name; the rest follow by name or by time (the
 * ⋮ menu switches, and remembers). Tapping a row checks or unchecks it; the choice is saved as the page goes
 * (onPause). The search icon opens a field that keeps only the unchosen cities whose names start with what is typed.
 *
 * Signals: "closed" (): Up was pressed; "show-settings" (): the menu's Settings. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CLOCK_TYPE_CITY_PICKER (city_picker_get_type ())
G_DECLARE_FINAL_TYPE (CityPicker, city_picker, CLOCK, CITY_PICKER, GtkWidget)

GtkWidget *city_picker_new (GSettings *settings);

G_END_DECLS
