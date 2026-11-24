/* dictate-tools: what Agent can do on the phone, as tools for the model (dictate-talk.c): their definitions for a
 * request, and the running of a call.
 *
 * Each tool belongs to one of the areas of "What Agent can use" (dictate-access.h), or to none (the time and the
 * battery): a tool whose area is off is not offered, and refused if it is called anyway. Tools return only the few
 * fields an answer needs. Their results go to the model on VibeOS's own server and nowhere else.
 *
 * Three kinds:
 *  - reading: contacts, the calendar, alarms, calls, messages, apps, file names, the location, the phone's state;
 *  - doing what can be undone at once on the phone: an alarm or a timer, an app opened, a setting changed;
 *  - doing what reaches other people or cannot be undone (a message, a call, an event, a deletion): the tool only
 *    proposes it, as a DictateAction the user sees on the screen with what exactly will happen, and it runs only
 *    when the user taps its button (dictate_action_run ()). The model's word alone never does it.
 *
 * Calls run on the brain's thread (dictate-talk.c); a proposal is handed to `propose`, which takes it to the main
 * thread. With DICTATE_TEST_DRY_RUN set, an action says in the log that it would run, without its content, and does
 * nothing. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _DictateAction DictateAction;

typedef struct {
  guint32 areas;                /* bit i: dictate_access_list ()[i] is on */
  const char *language;         /* the user's, for names of days and the like */
  void (*propose) (DictateAction *action, gpointer data);       /* takes a reference */
  gpointer data;
  gboolean private_speaker;     /* private mode, the answer on the speaker: no texts of messages or notifications */
  gboolean locked;              /* the phone is locked (Agent asked by holding the power key): reading tools only */
} DictateToolContext;

/* the areas that are on, as DictateToolContext.areas, from Agent's settings */
guint32      dictate_tools_areas       (GSettings *settings);
/* whether `area` (an access-* key) has tools */
gboolean     dictate_tools_area_has_tools (const char *area);
/* the tools of the areas in `areas`, as an OpenAI "tools" JSON array; NULL when there are none. `locked`: only the
 * ones that read (tier 0), for a locked phone */
char        *dictate_tools_definitions (guint32 areas, gboolean locked);
/* the line shown while `name` runs, such as "Checking your calendar…" (translated); NULL for no such tool */
const char  *dictate_tools_status      (const char *name);
/* runs `name` with `arguments` (JSON), at most `timeout_ms`: its result for the model, as JSON (an error is
 * {"error": "…"}), never NULL */
char        *dictate_tools_run         (const char *name, const char *arguments, const DictateToolContext *ctx,
                                        guint timeout_ms);

/* the risk class of a tool, for Voice Match (the voice-match-everyday, -personal and -actions settings) */
typedef enum {
  DICTATE_RISK_EVERYDAY,        /* the time, the weather, alarms, timers, the phone's state */
  DICTATE_RISK_PERSONAL,        /* reading messages, calls, the calendar, contacts, files, where the phone is */
  DICTATE_RISK_ACTIONS,         /* calling, sending, adding, deleting, changing settings */
} DictateRisk;

/* `name`'s class; a tool not known is an action */
DictateRisk  dictate_tools_risk        (const char *name);

/* --- a proposed action */

DictateAction       *dictate_action_ref         (DictateAction *self);
void                 dictate_action_unref       (DictateAction *self);
/* "Send a message"; then what exactly will happen, a line each ("To Anna, mobile", the text…); the button's word */
const char          *dictate_action_get_title   (DictateAction *self);
const char *const   *dictate_action_get_lines   (DictateAction *self);
const char          *dictate_action_get_button  (DictateAction *self);
/* the card's fields, label and value ("Message to", "Kim Novak"; "Text", "“…”"), translated, ended by a NULL label;
 * NULL when it has none (its title and lines then) */
typedef struct {
  const char *label;
  const char *value;
} DictateActionField;
const DictateActionField *dictate_action_get_fields (DictateAction *self);
/* it deletes, or cannot be undone: its button says so */
gboolean             dictate_action_get_destructive (DictateAction *self);
/* it goes to someone, a message or a call; certain: to whom was not guessed (a number given, or the contact's own
 * name), only then may it count down */
gboolean             dictate_action_get_outward  (DictateAction *self);
gboolean             dictate_action_get_certain  (DictateAction *self);
void                 dictate_action_set_outward  (DictateAction *self, gboolean certain);
/* the seconds it counts down once Vibe has read it back, then runs by itself (outward-confirm 'countdown'); 0: it
 * waits for the user's tap or "send". Set by DictateTalk to DICTATE_ACTION_COUNTDOWN when outward-confirm is
 * 'countdown', the turn's voice passed its check and the phone is unlocked; it takes only on an outward action whose
 * recipient is certain and that is not destructive. The window shows the countdown and, at 0, runs it with
 * dictate_action_run_by (DICTATE_ACTION_BY_COUNTDOWN); any new speech or touch, the screen going off or the window
 * leaving the foreground stops it, and the card then waits */
#define DICTATE_ACTION_COUNTDOWN 3
guint                dictate_action_get_countdown (DictateAction *self);
void                 dictate_action_set_countdown (DictateAction *self, guint seconds);
/* nothing was decided about it yet */
gboolean             dictate_action_is_waiting   (DictateAction *self);

/* how an action was run, for the action log and for what the model is told */
typedef enum {
  DICTATE_ACTION_BY_TAP,        /* its button */
  DICTATE_ACTION_BY_COUNTDOWN,  /* its countdown ended */
  DICTATE_ACTION_BY_VOICE,      /* the user said "send" */
} DictateActionBy;
/* the user tapped its button: it runs (once) */
void                 dictate_action_run         (DictateAction *self, GCancellable *cancellable,
                                                 GAsyncReadyCallback callback, gpointer data);
/* the same, run otherwise than by its button */
void                 dictate_action_run_by      (DictateAction *self, DictateActionBy by, GCancellable *cancellable,
                                                 GAsyncReadyCallback callback, gpointer data);
/* what was done, for the card ("Sent"), or FALSE with the error */
char                *dictate_action_run_finish  (DictateAction *self, GAsyncResult *result, GError **error);
/* a line for the model, at the next question, about what became of it ("The user confirmed: the message to Anna
 * was sent"); NULL while the user has not decided */
char                *dictate_action_outcome     (DictateAction *self);
/* the user cancelled it */
void                 dictate_action_cancel      (DictateAction *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (DictateAction, dictate_action_unref)

G_END_DECLS
