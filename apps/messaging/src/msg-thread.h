/* How a conversation's messages are laid out: where a date line goes between them, which messages make a run (the
 * consecutive ones from the same side, drawn as one stack of bubbles), and the date line's words.
 *
 * A date line comes before the first message and wherever an hour or more passed since the one before. A run is
 * broken by a date line, by the other side writing, and in a group by another sender. The newest message has its
 * time under it unless it failed (a failed one has its own line). No widgets here: the conversation view and the
 * tests share it. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define MSG_THREAD_SEPARATOR_GAP_MS (60 * 60 * 1000)

typedef struct {
  gint64      time;         /* ms since the epoch */
  gboolean    outgoing;
  const char *sender;       /* an incoming message's sender; NULL for an outgoing one, or not known */
  gboolean    failed;
} MsgThreadItem;

typedef enum {
  MSG_RUN_SINGLE,           /* alone: all its corners round */
  MSG_RUN_FIRST,            /* the first of several: the corner towards the next one small */
  MSG_RUN_MIDDLE,           /* both corners on its side small */
  MSG_RUN_LAST,             /* the corner towards the one before small */
} MsgRunPosition;

typedef struct {
  gboolean       separator;     /* a date line before it */
  MsgRunPosition position;
  gboolean       time_under;    /* its time under it */
} MsgThreadPlace;

/* `places` has room for `n` */
void   msg_thread_layout          (const MsgThreadItem *items, guint n, MsgThreadPlace *places);

/* the space over a message, in dp: none over the first or under a date line (which has its own), 2 inside a run,
 * 16 between runs */
int    msg_thread_gap_before      (const MsgThreadPlace *places, guint index);

/* the date line's words: "Today • 14:45", "Yesterday • 9:05 AM", "Thursday, Oct 29 • 14:45", with the year when
 * it is not this year's. `now` in ms; the clock as the system shows it (24 h or not) */
char  *msg_thread_separator_text  (gint64 time, gint64 now, gboolean use_24h);

/* a time of day alone: "14:45" or "2:45 PM" */
char  *msg_thread_time_text       (gint64 time, gboolean use_24h);

G_END_DECLS
