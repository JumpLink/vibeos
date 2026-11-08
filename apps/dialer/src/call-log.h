/* The call log: every call the phone made or took, kept in SQLite (spec §14.1). It stands in for Android's
 * CallLog.Calls provider, with that table's column names, so that what the Dialer reads from it maps one to one.
 *
 * Only the service writes it (a call ends, the user deletes rows); the app asks the service for the rows over
 * D-Bus. One writer keeps it simple: no locking between processes, and one place that says "the log changed". */
#pragma once

#include <gio/gio.h>

#include "call-types.h"

typedef struct {
  gint64 id;                    /* _id */
  char *number;                 /* as dialled or as received; "" when the network sent none */
  gint64 date;                  /* when the call began, Unix ms */
  gint64 duration;              /* how long it was connected, s */
  CallType type;
  Presentation presentation;
  char *country_iso;            /* the network's country at the time ("DE"), for formatting the number; or NULL */
  gboolean is_read;             /* the user has seen it (a missed call is bold until then) */
  gboolean is_new;              /* not yet taken in: counts for the missed-call notification */
} CallLogEntry;

typedef struct _CallLog CallLog;

/* opens (or makes) the log at @path. The first time, it takes in GNOME Calls' history from @import_path if that
 * exists (NULL: none), so that switching dialers keeps the recent calls. */
CallLog      *call_log_open (const char *path, const char *import_path, GError **error);
void          call_log_free (CallLog *log);

/* adds @entry (its id is ignored) and returns the new row's id, or 0 if it could not be written */
gint64        call_log_add (CallLog *log, const CallLogEntry *entry);
/* the newest @limit rows, newest first: a GPtrArray of CallLogEntry */
GPtrArray    *call_log_list (CallLog *log, guint limit);
/* removes the rows with these ids; how many went */
guint         call_log_delete (CallLog *log, const gint64 *ids, guint n_ids);
void          call_log_clear (CallLog *log);
/* missed calls are seen: the one with @id (its notification was swiped away or tapped), or with 0 all of them
 * (the user looked at Recents): is_read and not new; how many changed */
guint         call_log_mark_missed_read (CallLog *log, gint64 id);
/* the missed calls not yet seen: the number the missed-call notification and the Recents badge show */
guint         call_log_count_new_missed (CallLog *log);

void          call_log_entry_free (CallLogEntry *entry);
/* an entry as the a{sv} the service hands out: the column names as keys */
GVariant     *call_log_entry_to_variant (const CallLogEntry *entry);
CallLogEntry *call_log_entry_from_variant (GVariant *dict);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (CallLogEntry, call_log_entry_free)
