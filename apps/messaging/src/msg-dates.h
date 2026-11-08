/* When a message was, as Messaging words it (spec §1.6; S/util/Dates.java). Times are Unix ms. */
#pragma once

#include <glib.h>

/* getConversationTimeString, a conversation row's: "Just now", "5 mins", "14:05", "Mon", "Mar 7", then the date
 * with its year */
char *msg_dates_conversation (gint64 time, gint64 now);
/* getMessageTimeString, under a bubble: "Now", "5 mins", "14:05", "Mon 14:05", "Mar 7, 14:05", then the date with
 * its year and the time */
char *msg_dates_message (gint64 time, gint64 now);
/* getMessageDetailsTimeString, in Message details: the full date and time */
char *msg_dates_details (gint64 time);
