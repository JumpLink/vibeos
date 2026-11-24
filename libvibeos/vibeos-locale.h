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

/* TRUE when the user's region tells the weather's temperatures in degrees Fahrenheit (the United States and a few
 * others, by CLDR's unit preferences); Celsius everywhere else, and when the interface is in no language */
gboolean  vibeos_locale_uses_fahrenheit (void);

/* a wind's speed given in km/h, in the unit the user's region tells it in, with that unit's short name: "19 km/h",
 * "12 mph" (the United States, the United Kingdom), "5 м/с" (Russia); newly allocated */
char     *vibeos_format_wind_speed (double kilometres_per_hour);

/* the air pressure given in hPa, in the unit the user's region reads a barometer in: "1.013 hPa", "1,013 mbar" (the
 * United Kingdom), "29.92 inHg" (the United States), "760 мм рт. ст." (Russia); newly allocated */
char     *vibeos_format_air_pressure (double hectopascals);

/* The name of the POSIX locale @posix ("de_DE.UTF-8", "sr_RS.UTF-8@latin"), as a list of languages shows it, its
 * first letter a capital: in the language of the ICU locale @in ("German (Germany)" in "en_US"; vibeos_locale_id ()
 * for the user's), or with @in NULL in its own ("Deutsch (Deutschland)", "Русский (Россия)"), as Android's language
 * list has them. Newly allocated; NULL for C and POSIX. It reads nothing from the environment, so it may be called
 * from any thread. */
char     *vibeos_locale_name    (const char *posix, const char *in);

/* ---- the session's language ----
 *
 * The session's programs take their language from the environment they are started in: LANG, and the Formats
 * region in LC_TIME, LC_NUMERIC, LC_MONETARY, LC_MEASUREMENT and LC_PAPER when it is not the language's (as
 * gnome-session sets them at log in, from org.gnome.system.locale's region). VibeOS Settings changes them at once:
 * for the programs started from then on (the environment systemd's user manager and the session bus start them in)
 * and for the running ones that follow the change (vibeos_locale_watch), which it tells by the session bus signal
 * vibeos.Locale.Changed (as) at /vibeos/Locale: the environment as NAME=VALUE strings, an empty VALUE for a variable
 * that is to be unset. */

/* that environment for @language ("de_DE.UTF-8") and the Formats @region ("" or NULL: the language's): LANG, the
 * variables that would stand in front of it (LANGUAGE, LC_ALL, LC_MESSAGES) unset, and the region's; newly
 * allocated */
char    **vibeos_locale_session_environment (const char *language, const char *region);

/* gives the session @language and @region: to the programs started from now on, to the running ones that follow it,
 * and to this one (its environment, its C locale and ICU's default locale, and its own watches called). FALSE with
 * @error when one of those could not be told (the program's own change is made all the same). */
gboolean  vibeos_locale_set_session (const char *language, const char *region, GError **error);

typedef void (*VibeosLocaleFunc) (gpointer user_data);

/* calls @func when the session's language or region change, after this program's environment, its C locale and
 * ICU's default locale have changed to them: what the program makes from then on is in the new language, and what
 * it made before is for @func to make again. Returns an id for vibeos_locale_unwatch. */
guint     vibeos_locale_watch   (VibeosLocaleFunc func, gpointer user_data, GDestroyNotify destroy);
void      vibeos_locale_unwatch (guint id);

G_END_DECLS
