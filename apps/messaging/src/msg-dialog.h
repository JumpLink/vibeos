/* Messaging's alert dialogs and snackbar: libvibeos's, in the theme's colours and shapes, with the app's timing. */
#pragma once

#include <vibeos.h>

static inline GtkWidget *
msg_dialog_add_button (VibeosDialog *dialog, const char *label, int response)
{
  return vibeos_dialog_add_button (dialog, label, response);
}

static inline VibeosSnackbar *
msg_snackbar_new (void)
{
  return VIBEOS_SNACKBAR (vibeos_snackbar_new ());
}

static inline void
msg_snackbar_show (VibeosSnackbar *snackbar, const char *text, const char *action, VibeosSnackbarAction action_cb,
                   gpointer data)
{
  /* 5 s, so an Undo can still be reached */
  vibeos_snackbar_show (snackbar, text, action, action_cb, data, 5000);
}
