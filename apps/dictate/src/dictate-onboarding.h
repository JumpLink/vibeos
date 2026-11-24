/* dictate-onboarding: Vibe's introduction, once (the vibeos.Dictate first-run key): on the first start of the app
 * after it is installed or updated, the session starting it (data/vibeos.Agent-intro.desktop) or the user.
 *
 * Six steps on Vibe's stage (dictate-stage.h), each said aloud in the language Vibe speaks and written as said:
 * hello; the language, downloaded if it is not on the phone; the speech rate, tried by ear; what Vibe may use (the
 * access-* switches of the areas it has tools for); the voice sample (dictate-enroll.h), which can be skipped; and
 * the one rule, with things to try. Each step is tapped through, or answered aloud: after Vibe has said it, it
 * listens for "continue", "back", a language, "slower", "turn off files" and the like, matched on the phone; nothing
 * heard in it goes to the model. Its first-run key goes false however it ends. */
#pragma once

#include "dictate-talk.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct {
  /* the language Vibe listens and speaks in, and a new one chosen (on the phone by then) */
  const char *(*language) (gpointer data);
  void (*set_language) (const char *language, gpointer data);
  /* over: what the user said or tapped at the last step, to be asked in the conversation, with its speech (NULL for
   * none); "" to listen now; NULL for nothing */
  void (*done) (const char *said, GBytes *audio, gpointer data);
  gpointer data;
} DictateOnboardingHooks;

/* the introduction, as a page over the conversation; `hooks` is copied */
GtkWidget *dictate_onboarding_new   (GtkWidget *window, GSettings *settings, DictateTalk *talk,
                                     const DictateOnboardingHooks *hooks);
/* its first step said: the page is on show */
void       dictate_onboarding_start (GtkWidget *onboarding);

G_END_DECLS
