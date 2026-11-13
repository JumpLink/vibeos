/* Settings (DialerSettingsActivity; spec §9): the Phone's settings on a screen of its own, over the main one.
 *
 * The top list has what this phone can back: Display options (how contacts are sorted and their names written) and
 * Sounds and vibration (the ringtone, vibrating for calls, the keypad tones and InCallUI's in-call vibrations).
 * Android's other rows open other apps' pages or need what the phone does not have yet, and are left out: Quick
 * responses (no text messages yet), Phone number lookup (no lookup providers), Calling accounts, Blocked numbers
 * (no blocking), Voicemail and Accessibility (TTY): see the comment in dialer-settings.c.
 *
 * A row opens its page in the same screen, under the same bar, whose title becomes the page's. Up is the screen's
 * back: its accessible name is "Back", the button the shell's back gesture clicks (a11y.js). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define DIALER_TYPE_SETTINGS (dialer_settings_get_type ())
G_DECLARE_FINAL_TYPE (DialerSettings, dialer_settings, DIALER, SETTINGS, GtkWidget)

/* @settings: neo.Dialer */
GtkWidget *dialer_settings_new (GSettings *settings);

/* Signals: "closed" (): Up or Back on the top list, the window takes the screen away */

G_END_DECLS
