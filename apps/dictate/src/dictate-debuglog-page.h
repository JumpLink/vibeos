/* dictate-debuglog-page: the Debug log, from Vibe's settings: the turns recorded on the phone while the Debug log
 * switch is on (dictate-debuglog.h), the newest first. A turn opens on its own page: what was said and answered,
 * how it ended, and each of its steps, a request to the model, an HTTP exchange, an answer, a tool's call; a step
 * opens on a page of its own with all of it, as it was recorded. What is shown and copied has the gateway's key and
 * the phone's id cut short (dictate_debuglog_redact ()). Copy turn puts a whole turn on the clipboard, Clear log
 * deletes all of them. */
#pragma once

#include <gtk/gtk.h>
#include <vibeos.h>

G_BEGIN_DECLS

/* the list page, for a VibeosPageStack; `back` pops it (the window's), `data` its data */
GtkWidget *dictate_debuglog_page_new (GtkWidget *window, VibeosPageStack *pages, void (*back) (gpointer data),
                                      gpointer data);

G_END_DECLS
