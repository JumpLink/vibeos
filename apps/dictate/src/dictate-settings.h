/* dictate-settings: Vibe's Settings page, from "Settings" in the conversation's bar, over the conversation.
 *
 * Rows on cards, as the other VibeOS settings pages: the language listened in and spoken, the replies spoken or only
 * written, how fast and how much Vibe says, listening again after a reply; Voice match (dictate-voicematch-page.h);
 * calls, private mode and quiet hours; what Vibe can use, a switch each for the areas it has tools for
 * (dictate-access.h); the conversation kept, and clearing it; the languages, each with what is on the phone of it:
 * one that lacks something is downloaded from here (dictate-download-dialog.h), and what was downloaded can be
 * removed again; the debug log (dictate-debuglog-page.h), and About. */
#pragma once

#include "dictate-talk.h"

#include <gtk/gtk.h>
#include <vibeos.h>

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
  /* the window's pages, for the pages this one opens */
  VibeosPageStack *pages;
  /* the conversation: whether there is one, and clearing it (asked first) */
  gboolean (*has_conversation) (gpointer data);
  void (*clear_conversation) (gpointer data);
  /* About Vibe */
  void (*about) (gpointer data);
  gpointer data;
} DictateSettingsHooks;

/* the page, for a VibeosPageStack; `hooks` is copied */
GtkWidget *dictate_settings_page_new     (GtkWidget *window, const DictateSettingsHooks *hooks);
/* its rows again, after the language or what is on the phone changed */
void       dictate_settings_page_refresh (GtkWidget *page);

G_END_DECLS
