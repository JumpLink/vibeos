/* dictate-tools-private.h: between dictate-tools.c (the table, the running of a call, the proposed actions) and
 * dictate-tools-phone.c (what each tool reads or does on the phone). */
#pragma once

#include "dictate-actions.h"
#include "dictate-tools.h"

#include <json-glib/json-glib.h>

G_BEGIN_DECLS

typedef struct _ToolRun ToolRun;

/* a tool: its result for the model, a JSON object (NULL with `error`, whose message the model is given) */
typedef JsonNode *(*ToolFunc) (JsonObject *args, ToolRun *run, GError **error);

typedef struct {
  const char *name;
  const char *area;             /* its "What Agent can use" switch (an access-* key); NULL: always there */
  const char *status;           /* the line while it runs, untranslated (N_) */
  const char *description;      /* for the model */
  const char *parameters;       /* a JSON schema, for the model */
  ToolFunc func;
  DictateTier tier;             /* its risk tier (dictate-actions.h) */
  const char *did;              /* what it did, for the action log's reader: "set an alarm" */
  gboolean others;              /* its result can hold what others wrote (a message, a note, an event's title) */
} DictateTool;

const DictateTool *dictate_tools_table (guint *n);

/* a run's: */
gboolean      tool_run_area_on   (ToolRun *run, const char *area);
/* DICTATE_TEST_DRY_RUN: what reaches outside (an app opened, a message, a call) is only logged */
gboolean      tool_run_dry       (ToolRun *run);
/* private mode with the answer on the speaker: no message or notification texts (DictateToolContext) */
gboolean      tool_run_private   (ToolRun *run);
GCancellable *tool_run_cancellable (ToolRun *run);
/* hands a proposal to the window (once the model's turn may still use it) */
void          tool_run_propose   (ToolRun *run, DictateAction *action);
/* for the action log: what undoing this needs (a JSON object, taken), somewhere else something went besides the
 * model's server ("the town's rough position, to Open-Meteo"), and the entry it undid */
void          tool_run_set_undo  (ToolRun *run, JsonNode *undo);
void          tool_run_add_sent  (ToolRun *run, const char *where);
void          tool_run_set_undid (ToolRun *run, gint64 id);
/* it ran, and nothing changed (a switch already as asked): nothing for undo */
void          tool_run_set_unchanged (ToolRun *run);

/* what a proposal does when the user taps its button, on a thread of its own */
typedef gboolean (*DictateActionFunc) (const char *destination, const char *text, GError **error);

/* kind: for the log, with nothing personal ("send a message"); title, lines, button: the card's; done: the card's
 * word once it ran ("Sent"); about: for the model ("the message to Anna"); destination and text: what func gets */
DictateAction *dictate_action_new (const char *kind, const char *title, const char *const *lines,
                                   const char *button, const char *done, const char *about,
                                   DictateActionFunc func, const char *destination, const char *text);
/* for the action log, once the user has decided: the tool that proposed it, and where its doing sends something
 * ("the message, by SMS to its recipient"; NULL: it stays on the phone) */
void           dictate_action_set_log (DictateAction *action, const char *tool, const char *sent);

/* `name` has its own risk class (dictate_tools_risk ()), for the tests */
gboolean       dictate_tools_risk_known (const char *name);

/* the tools' table entry called `name`, or NULL */
const DictateTool *dictate_tools_find (const char *name);

/* arguments */
const char *tool_arg_string  (JsonObject *args, const char *name);
gint64      tool_arg_int     (JsonObject *args, const char *name, gint64 fallback);
double      tool_arg_double  (JsonObject *args, const char *name, double fallback);
gboolean    tool_arg_bool    (JsonObject *args, const char *name, gboolean fallback);

/* the model's error for a tool: a GError in G_IO_ERROR with `message` */
void        tool_error       (GError **error, const char *format, ...) G_GNUC_PRINTF (2, 3);

/* "today at 14:32", "yesterday at 9:05", "on Monday at 8:10", "on 12 November at 8:10", and within 12 hours of now
 * how far off: "today at 09:50 (25 minutes ago)", "today at 15:30 (in 5 hours)" (English: the model says it in the
 * user's language); `ms` Unix ms */
char       *tool_when        (gint64 ms, gboolean with_time);

G_END_DECLS
