/* Settings: the Phone's settings on a screen of its own, over the main one.
 *
 * The top list has what this phone can back: Display options (how contacts are sorted and their names written),
 * Sounds and vibration (the ringtone, vibrating for calls, the keypad tones and the in-call vibrations) and Blocked
 * numbers (the numbers whose calls are turned away, and adding one). The Dialer's other rows open other apps' pages
 * or need what the phone does not have yet, and are left out: Quick responses (no text messages yet), Phone number
 * lookup (no lookup providers), Calling accounts, Voicemail and Accessibility (TTY): see the comment in
 * dialer-settings.c.
 *
 * A row opens its page in the same screen, under the same bar, whose title becomes the page's. Up is the screen's
 * back: its accessible name is "Back", the button the shell's back gesture clicks (a11y.js). On a tablet each page's
 * rows are a column in the middle (VibeosClamp). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define DIALER_TYPE_SETTINGS (dialer_settings_get_type ())
G_DECLARE_FINAL_TYPE (DialerSettings, dialer_settings, DIALER, SETTINGS, GtkWidget)

/* @settings: vibeos.Dialer */
GtkWidget *dialer_settings_new (GSettings *settings);

/* Signals: "closed" (): Up or Back on the top list, the window takes the screen away */

G_END_DECLS
