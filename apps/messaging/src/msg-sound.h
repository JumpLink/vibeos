/* The sound as a message goes: message_sent.wav, when Settings' "Outgoing message
 * sounds" is on and the phone is not silenced. It plays on the notification stream so that it stays quiet
 * whenever notifications are: here that is feedbackd's profile, which must be "full" (not "quiet" or "silent"). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

void msg_sound_play_sent (void);

G_END_DECLS
