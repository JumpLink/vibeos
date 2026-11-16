/* Messaging's alert dialogs and snackbar, libvibeos's restyled in messaging.css.
 *
 * The dialogs' buttons are in capitals. The snackbar is the app's own: edge to edge, its action in
 * capitals. */
#pragma once

#include <vibeos.h>

static inline GtkWidget *
msg_dialog_add_button (VibeosDialog *dialog, const char *label, int response)
{
  g_autofree char *caps = g_utf8_strup (label, -1);

  return vibeos_dialog_add_button (dialog, caps, response);
}

static inline VibeosSnackbar *
msg_snackbar_new (void)
{
  VibeosSnackbar *snackbar = VIBEOS_SNACKBAR (vibeos_snackbar_new ());

  vibeos_snackbar_set_margin (snackbar, 0);
  return snackbar;
}

static inline void
msg_snackbar_show (VibeosSnackbar *snackbar, const char *text, const char *action, VibeosSnackbarAction action_cb,
                   gpointer data)
{
  g_autofree char *caps = action ? g_utf8_strup (action, -1) : NULL;

  /* 5 s, Bugle's default */
  vibeos_snackbar_show (snackbar, text, caps, action_cb, data, 5000);
}
