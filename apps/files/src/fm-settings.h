/* The Settings page: check rows bound to GSettings, under the toolbar's ← (the shell's). */
#pragma once

#include <gtk/gtk.h>

#include "fm-window.h"

G_BEGIN_DECLS

GtkWidget *fm_settings_page_new (FmShell *shell);

G_END_DECLS
