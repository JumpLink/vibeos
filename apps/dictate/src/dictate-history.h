/* dictate-history: Vibe's conversation kept on the phone, so that it is still there after the app has closed: each
 * exchange's words, Vibe's reply and what became of it, in vibeos-dictate/conversation.json in the user's data
 * directory (readable by the user alone). Clearing the conversation empties it. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char *said, *reply;
  char *note;                   /* what became of it ("Sent", "Not carried out"), or NULL */
  gboolean refused;             /* not carried out for the voice: not the model's to remember */
} DictateHistoryEntry;

void       dictate_history_entry_free (DictateHistoryEntry *entry);
/* the exchanges kept, oldest first (an empty array for none) */
GPtrArray *dictate_history_load  (void);
/* `entries` written, at most the newest DICTATE_HISTORY_KEEP of them */
void       dictate_history_save  (GPtrArray *entries);
void       dictate_history_clear (void);

#define DICTATE_HISTORY_KEEP 200

G_END_DECLS
