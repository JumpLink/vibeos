/* dictate-actions: Agent's log of what it did, kept on the phone only: every question it answered and every tool it
 * ran or proposed, with the tool's risk tier, how it went, and where anything went off the phone. It is a file of
 * JSON lines, vibeos-dictate/agent-log.jsonl under the user's data directory (0600), the newest LOG_KEEP entries.
 * Nothing in it is what was said or read: an entry has the time, the tool, its tier, its outcome, where something
 * went ("sent"), and for an action that can be undone what undoing it needs (an alarm's id, a radio's state before).
 * The tools get_action_log ("what did you just do?", "did that go to the server?") and undo_last_action read it.
 *
 * The tiers, from VibeOS's agent plan:
 *   0  a read, answered at once (the battery, the messages)
 *   1  done at once, and it can be undone: an alarm, a timer, Wi-Fi or Bluetooth (an app opened is tier 1 too)
 *   2  done only on the user's tap on a card: a call, a message, a new contact or calendar event
 *   3  costly (paying, deleting for good): none yet
 *
 * Every entry may be written from any thread. */
#pragma once

#include <json-glib/json-glib.h>

G_BEGIN_DECLS

typedef enum {
  DICTATE_TIER_READ,
  DICTATE_TIER_UNDOABLE,
  DICTATE_TIER_CONFIRMED,
  DICTATE_TIER_COSTLY,
} DictateTier;

/* where every question and every tool's result goes: the model that answers */
#define DICTATE_SENT_SERVER "VibeOS's model server"

/* appends an entry: `sent` is where something went off the phone (NULL-terminated, may be NULL), `undo` what undoing
 * it needs (a JSON object, taken; NULL: it cannot be undone), `undid` the id of the entry this one undid (0: none).
 * Its id (> 0), or 0 when it could not be written */
gint64     dictate_actions_record   (const char *tool, DictateTier tier, const char *outcome,
                                     const char *const *sent, JsonNode *undo, gint64 undid);
/* the newest `max` entries, newest first: JsonObject (owned by the array) */
GPtrArray *dictate_actions_recent   (guint max);
/* whether entry `id` has been undone: an entry among `entries` (dictate_actions_recent ()) undid it */
gboolean   dictate_actions_undone   (GPtrArray *entries, gint64 id);

/* the time "what did I miss?" was last answered (Unix seconds), 0 for never; and setting it */
gint64     dictate_actions_missed_since     (void);
void       dictate_actions_set_missed_since (gint64 when);

/* the log and that time removed: for the test set's private phone only (agent-eval --fixtures) */
void       dictate_actions_forget   (void);

G_END_DECLS
