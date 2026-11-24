/* dictate-enroll: the owner's voice sample recorded, as Vibe leads it on a screen of its own (Vibe's stage, the
 * whole of it the button). A short passage is read in four parts, the last of them a few short commands; Vibe says
 * each part first, so it can be repeated by ear, then listens. Each recording is checked as it comes
 * (dictate-voicematch.h): one that will not do is read again, without starting over. With all four in, the sample
 * is saved, and only then does it replace the old one; Voice match is then on.
 *
 * Nothing heard here goes to the model: the microphone is an engine of the enrollment's own, or one the caller lends
 * it that nothing else listens to meanwhile. */
#pragma once

#include "dictate-engine.h"
#include "dictate-talk.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* over: `saved`, a new sample is on the phone */
typedef void (*DictateEnrollDone) (gboolean saved, gpointer data);

/* the screen; `talk` says each phrase (NULL: they are only written), in `language`; `engine` listens (NULL: one of
 * its own). `corner` is the bar's way out: "Skip" in the introduction, "Cancel" in Settings */
GtkWidget *dictate_enroll_new  (GSettings *settings, DictateTalk *talk, DictateEngine *engine, const char *language,
                                const char *corner, DictateEnrollDone done, gpointer data);
/* `dots` in the bar instead of the four of the phrases (the introduction's, at its step), left as they are */
void       dictate_enroll_set_dots (GtkWidget *enroll, GtkWidget *dots);
/* the test of the sample on the phone, on the same screen: a command said, how close its voice is told, and
 * nothing carried out. `done` is told FALSE */
GtkWidget *dictate_enroll_test_new (GSettings *settings, const char *language, DictateEnrollDone done, gpointer data);
/* the first phrase said and listened for: the screen is on show */
void       dictate_enroll_start (GtkWidget *enroll);

G_END_DECLS
