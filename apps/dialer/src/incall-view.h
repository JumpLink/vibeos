/* The in-call screen: a call being placed, up, on hold or just ended.
 *
 * At the top the contact grid: the avatar, a status line ("Calling…"), the name, and under it the kind of number
 * or, once the call is up, how long it has been. In the middle six buttons (Mute, Keypad, Speaker, then Add call
 * and Hold, or Swap and Hold with a second call), and at the foot the red end-call button. The keypad slides up
 * over the buttons and sends touch tones; a call on hold shows as a banner across the top.
 *
 * The view asks the call service itself (mute, speaker, hold, tones, hang up); what the buttons show comes back
 * from the service, so a button only turns on when the modem did what it asked. */
#pragma once

#include <gtk/gtk.h>

#include "caller.h"
#include "calls-client.h"

G_BEGIN_DECLS

#define INCALL_TYPE_VIEW (incall_view_get_type ())
G_DECLARE_FINAL_TYPE (InCallView, incall_view, INCALL, VIEW, GtkWidget)

GtkWidget *incall_view_new (void);
/* the call the screen is about and who is on it, and the other call still going (shown as on hold), or NULL */
void       incall_view_set_calls (InCallView *self, const CallInfo *primary, const Caller *caller,
                                  const CallInfo *secondary, const Caller *secondary_caller);
/* with the phone locked, Add call (which opens the Phone app) is off */
void       incall_view_set_locked (InCallView *self, gboolean locked);
gboolean   incall_view_get_dialpad_shown (InCallView *self);
void       incall_view_show_dialpad (InCallView *self, gboolean shown);

/* Signals: "add-call" (): Add call was tapped; the window opens the Phone app for the second number.
 *          "notice" (s): something to tell the user in a snackbar (the keypad closed: the modem sends no tones) */

G_END_DECLS
