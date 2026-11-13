/* The answer screen: a ringing call, full screen.
 *
 * At the top "Call from", the caller's name and the kind of their number; at the foot a white disc with a phone in
 * it that bounces. Swiping it up answers, swiping it down declines; a tap on it only shows the hints. While
 * another call is up, a second way in sits in the bottom-right corner: swiping from that icon answers and ends the
 * other call.
 *
 * The view asks the call service itself; whether the call was answered shows when the service says so (the window
 * then shows the in-call screen, or closes). */
#pragma once

#include <gtk/gtk.h>

#include "caller.h"
#include "calls-client.h"

G_BEGIN_DECLS

#define ANSWER_TYPE_VIEW (answer_view_get_type ())
G_DECLARE_FINAL_TYPE (AnswerView, answer_view, ANSWER, VIEW, GtkWidget)

GtkWidget *answer_view_new (void);
/* the call it is for, and who it is from; a new call starts the screen over (its entry animation), the same call
 * only updates what it says */
void       answer_view_set_call (AnswerView *self, const CallInfo *call, const Caller *caller);
guint      answer_view_get_call (AnswerView *self);
/* whether another call is up (answering can end it) and what the hint then says: whether the modem
 * can hold, and whether a call is on hold already */
void       answer_view_set_other_call (AnswerView *self, gboolean other_call, gboolean can_hold, gboolean held_call);
/* an ask the service refused (calls-client "failed"): the puck comes back so the user can try again */
void       answer_view_reset (AnswerView *self);

G_END_DECLS
