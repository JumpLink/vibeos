/* dictate-voicematch-page: Settings › Voice match. With no sample on the phone, a card that offers to record one
 * (dictate-enroll.h); with one, "Only answer my voice", the sample (Re-record, Delete), how close a match must be, what
 * the owner's voice is asked for, and a test. Re-recording and deleting the sample need the owner's PIN when the phone
 * has one (dictate-owner.h): someone else's voice must not replace the owner's. */
#pragma once

#include "dictate-talk.h"

#include <gtk/gtk.h>
#include <vibeos.h>

G_BEGIN_DECLS

/* the page, for `pages`; `language` is the one Vibe speaks (the recording's phrases are in it) */
GtkWidget *dictate_voicematch_page_new (GtkWidget *window, VibeosPageStack *pages, GSettings *settings,
                                        DictateTalk *talk, const char *language);
/* the Settings row's summary: "On · balanced match", "Off · no voice sample yet" */
char      *dictate_voicematch_summary  (GSettings *settings);

G_END_DECLS
