/* dictate-settings: Agent's Settings page, from its ⋮ menu, over the conversation.
 *
 * Rows on cards, as the other VibeOS settings pages: the language listened in and spoken, the replies spoken or only
 * written (the bar's speaker button is the same setting), listening again after a reply; what Agent can use, a
 * switch each (dictate-access.h); and the languages, each with what is on the phone of it: one that lacks something
 * is downloaded from here (dictate-download-dialog.h), and what was downloaded can be removed again. */
#pragma once

#include "dictate-talk.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct {
  GSettings *settings;          /* vibeos.Dictate */
  DictateTalk *talk;
  /* the window's: the language listened in now, its dialog, and Back */
  const char *(*language) (gpointer data);
  void (*choose_language) (gpointer data);
  void (*back) (gpointer data);
  /* what was downloaded for `language` has been removed (NULL: nothing to do) */
  void (*removed) (const char *language, gpointer data);
  gpointer data;
} DictateSettingsHooks;

/* the page, for a VibeosPageStack; `hooks` is copied */
GtkWidget *dictate_settings_page_new     (GtkWidget *window, const DictateSettingsHooks *hooks);
/* its rows again, after the language or what is on the phone changed */
void       dictate_settings_page_refresh (GtkWidget *page);

G_END_DECLS
