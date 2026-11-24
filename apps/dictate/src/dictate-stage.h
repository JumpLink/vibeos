/* DictateStage: Vibe's conversation, one surface for every task.
 *
 * The exchange going on fills the screen: what the user said in a bubble at the top right, the orb (dictate-orb.h)
 * or a card in the middle, then who speaks ("Vibe says"), the reply's first sentence large and the rest under it,
 * the replies' buttons, and at the bottom a line of what can be done now ("Tap anywhere and speak"). Earlier
 * exchanges are above it, in the same style: scrolled up, they come back, the newest at the bottom. Above all of it
 * is the app's own bar: "Vibe" and Settings.
 *
 * The window says what is shown; the stage only lays it out. A tap anywhere on it but its buttons is "tapped", a
 * long press "long-pressed", Settings "settings". */
#pragma once

#include "dictate-engine.h"
#include "dictate-orb.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef enum {
  DICTATE_STAGE_BUTTON,          /* on the surface's raised tone */
  DICTATE_STAGE_BUTTON_PRIMARY,  /* the one that does it: Send, Answer, That's good */
  DICTATE_STAGE_BUTTON_TONAL,    /* the secondary container's: Turn on */
  DICTATE_STAGE_BUTTON_DANGER,   /* the error container's: Decline */
} DictateStageButtonKind;

#define DICTATE_TYPE_STAGE (dictate_stage_get_type ())
G_DECLARE_FINAL_TYPE (DictateStage, dictate_stage, DICTATE, STAGE, GtkWidget)

GtkWidget  *dictate_stage_new            (DictateEngine *engine);

/* the bar: "Vibe" and Settings, or a widget in either place instead (NULL: the usual one again); a call's label
 * ("Incoming call") in place of both */
void        dictate_stage_set_title      (DictateStage *self, const char *title);
void        dictate_stage_set_header_start (DictateStage *self, GtkWidget *widget);
void        dictate_stage_set_header_end (DictateStage *self, GtkWidget *widget);
void        dictate_stage_set_call_label (DictateStage *self, const char *label);

/* the exchange going on */
void        dictate_stage_set_said       (DictateStage *self, const char *said);
DictateOrb *dictate_stage_get_orb        (DictateStage *self);
/* a card in the middle in place of the orb; NULL for the orb again */
void        dictate_stage_set_middle     (DictateStage *self, GtkWidget *widget);
/* the text over the middle instead of under it (a question with its choices under it) */
void        dictate_stage_set_text_first (DictateStage *self, gboolean first);
void        dictate_stage_set_who        (DictateStage *self, const char *who, gboolean error);
void        dictate_stage_set_text       (DictateStage *self, const char *headline, const char *sub);
/* what is being heard: the finished words, and the ones still being made out, fainter */
void        dictate_stage_set_heard      (DictateStage *self, const char *finished, const char *partial);
/* a reply: its first sentence large, the rest under it */
void        dictate_stage_set_reply      (DictateStage *self, const char *reply);
/* lines written and not spoken, under the text (private mode's names and messages); NULL for none */
void        dictate_stage_set_written    (DictateStage *self, const char *const *lines);
void        dictate_stage_clear_buttons  (DictateStage *self);
/* a button in the row under the text: emits "clicked" (VibeosPressable's) */
GtkWidget  *dictate_stage_add_button     (DictateStage *self, const char *label, DictateStageButtonKind kind);
/* the call's two: taller, a size larger */
void        dictate_stage_set_large_buttons (DictateStage *self, gboolean large);
/* a bar under the text, filled to `fraction` (a countdown, a download); below 0 for none */
void        dictate_stage_set_progress   (DictateStage *self, double fraction);
void        dictate_stage_set_hint       (DictateStage *self, const char *hint);
/* all of the above as on a new screen: no bubble, the orb at rest, no text, buttons or hint */
void        dictate_stage_reset          (DictateStage *self);

/* split as dictate_stage_set_reply does: the first sentence, and the rest ("" for none) */
void        dictate_stage_split_reply    (const char *reply, char **headline, char **rest);

/* the earlier exchanges, above: `note` is what became of it ("Not carried out", "Sent"), or NULL */
void        dictate_stage_add_past       (DictateStage *self, const char *said, const char *reply, const char *note);
void        dictate_stage_clear_past     (DictateStage *self);
gboolean    dictate_stage_has_past       (DictateStage *self);

/* the dots of a series of steps, for the bar (the introduction, a recording): the step now wide, the ones done in the
 * primary colour */
GtkWidget  *dictate_dots_new             (guint n);
void        dictate_dots_set             (GtkWidget *dots, guint current);

/* Signals: "tapped" (), "long-pressed" (), "settings" () */

G_END_DECLS
