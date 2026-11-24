/* vibeos-locale: dates and times the way Android writes them in the user's language. A skeleton names the fields
 * wanted, as ICU's (and Android's DateFormat.getBestDateTimePattern's) do: "EEEMMMd" is the weekday, the month and
 * the day, abbreviated; "EEEEMMMMd" the same written out; "yMMMd" a date with its year; "jm" the hours and minutes.
 * ICU's CLDR data turns it into the language's own pattern, so "EEEMMMd" is "Tue, Nov 24" in English (US),
 * "Di., 24. Nov." in German, "mar. 24 nov." in French and "вт, 24 нояб." in Russian.
 *
 * The locale is the user's: the language the interface is in (LANGUAGE, LC_ALL, LC_MESSAGES, LANG), so the names are
 * always in it, with the country of the Formats region when one is set apart from the language (LC_TIME, as
 * gnome-session sets it): English with the region Germany writes "Tue, 24 Nov". An hour field 'j' follows the
 * 12/24-hour setting (vibeos_clock_uses_24h).
 *
 * Only GLib, GIO and ICU: the launcher builds this file in too. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* ICU's id of the user's locale: "de_DE", "en_DE", "nl"; "en_US" when the interface is in no language (C) */
char     *vibeos_locale_id      (void);

/* @time in the fields @skeleton names, in @time's own time zone; newly allocated */
char     *vibeos_format_date    (GDateTime *time, const char *skeleton);

/* the time from @from to @to in the fields @skeleton names, the parts they share written once, as the language
 * does: "Nov 24 – 26", "24.–26. Nov.", "10:00 – 11:30", "Fri, Apr 6, 17:00 – Sun, Apr 8, 18:00"; in @from's time
 * zone; newly allocated */
char     *vibeos_format_interval (GDateTime *from, GDateTime *to, const char *skeleton);

/* the hours and minutes of @time, by the 12/24-hour setting: "14:05", "2:05 PM" */
char     *vibeos_format_time    (GDateTime *time);

/* the hours and minutes as a large clock shows them: "14:05", or "2:05" with no AM or PM in 12-hour mode (as the
 * clocks of Android's home and lock screens) */
char     *vibeos_format_clock   (GDateTime *time);

/* the language's names for the morning and the afternoon of a 12-hour clock ("AM", "PM"); newly allocated */
void      vibeos_locale_am_pm   (char **am, char **pm);

/* TRUE when the clock is set to 24 hours (GNOME's clock-format, which VibeOS Settings sets) */
gboolean  vibeos_clock_uses_24h (void);

G_END_DECLS
