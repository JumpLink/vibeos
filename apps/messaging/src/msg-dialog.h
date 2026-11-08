/* Messaging's alert dialogs and snackbar, libneo's restyled in messaging.css.
 *
 * The dialogs are AppCompat's AlertDialog (spec §7.4), their buttons in capitals (Widget.MaterialComponents.Button
 * .TextButton is textAllCaps). The snackbar is Bugle's own (§1.13): edge to edge, #323232, its action in capitals. */
#pragma once

#include <neo.h>

static inline GtkWidget *
msg_dialog_add_button (NeoDialog *dialog, const char *label, int response)
{
  g_autofree char *caps = g_utf8_strup (label, -1);

  return neo_dialog_add_button (dialog, caps, response);
}

static inline NeoSnackbar *
msg_snackbar_new (void)
{
  NeoSnackbar *snackbar = NEO_SNACKBAR (neo_snackbar_new ());

  neo_snackbar_set_margin (snackbar, 0);
  return snackbar;
}

static inline void
msg_snackbar_show (NeoSnackbar *snackbar, const char *text, const char *action, NeoSnackbarAction action_cb,
                   gpointer data)
{
  g_autofree char *caps = action ? g_utf8_strup (action, -1) : NULL;

  /* SnackBar.LENGTH_LONG, Bugle's default */
  neo_snackbar_show (snackbar, text, caps, action_cb, data, 5000);
}
