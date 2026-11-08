/* The call log (call-log.h). */
#include "call-log.h"

#include <glib/gstdio.h>
#include <sqlite3.h>

#define SCHEMA_VERSION 1

struct _CallLog {
  sqlite3 *db;
};

/* CallLog.Calls' columns that the Dialer reads (CallLogQuery), less the cached contact fields: the app looks the
 * contact up as it shows a row, so a renamed contact never shows its old name here */
static const char schema[] =
  "CREATE TABLE IF NOT EXISTS calls ("
  "  _id INTEGER PRIMARY KEY AUTOINCREMENT,"
  "  number TEXT NOT NULL DEFAULT '',"
  "  date INTEGER NOT NULL,"
  "  duration INTEGER NOT NULL DEFAULT 0,"
  "  type INTEGER NOT NULL,"
  "  features INTEGER NOT NULL DEFAULT 0,"
  "  country_iso TEXT,"
  "  geocoded_location TEXT,"
  "  presentation INTEGER NOT NULL DEFAULT 1,"
  "  phone_account_id TEXT,"
  "  is_read INTEGER NOT NULL DEFAULT 0,"
  "  new INTEGER NOT NULL DEFAULT 1,"
  "  post_dial_digits TEXT NOT NULL DEFAULT '',"
  "  via_number TEXT NOT NULL DEFAULT ''"
  ");"
  "CREATE INDEX IF NOT EXISTS calls_date ON calls (date DESC);";

void
call_log_entry_free (CallLogEntry *entry)
{
  if (!entry)
    return;
  g_free (entry->number);
  g_free (entry->country_iso);
  g_free (entry);
}

static gboolean
exec (CallLog *self, const char *sql, GError **error)
{
  char *message = NULL;

  if (sqlite3_exec (self->db, sql, NULL, NULL, &message) == SQLITE_OK)
    return TRUE;
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "call log: %s", message);
  sqlite3_free (message);
  return FALSE;
}

/* --- GNOME Calls' history, once */

/* its times are ISO 8601 text ("2026-11-07T14:31:06.215146Z"), or NULL; as Unix ms, 0 for none */
static gint64
iso_ms (sqlite3_stmt *row, int column)
{
  const char *text = (const char *) sqlite3_column_text (row, column);
  g_autoptr(GDateTime) time = text ? g_date_time_new_from_iso8601 (text, NULL) : NULL;

  return time ? g_date_time_to_unix_usec (time) / 1000 : 0;
}

/* GNOME Calls keeps who, which way, and when it began, was answered and ended; it has no "declined", so an
 * unanswered incoming call is taken in as missed. All of it counts as seen: it is history, not news. */
static void
import_gnome_calls (CallLog *self, const char *path)
{
  sqlite3 *old = NULL;
  sqlite3_stmt *row = NULL;
  g_autofree char *uri = g_strconcat ("file:", path, "?mode=ro", NULL);
  guint n = 0;

  if (sqlite3_open_v2 (uri, &old, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL) != SQLITE_OK
      || sqlite3_prepare_v2 (old, "SELECT target, inbound, start, answered, end FROM calls", -1, &row, NULL)
         != SQLITE_OK)
    {
      g_warning ("call log: cannot read GNOME Calls' history: %s", sqlite3_errmsg (old));
      sqlite3_close (old);
      return;
    }
  exec (self, "BEGIN", NULL);
  while (sqlite3_step (row) == SQLITE_ROW)
    {
      const char *target = (const char *) sqlite3_column_text (row, 0);
      gboolean inbound = sqlite3_column_int (row, 1);
      gint64 start = iso_ms (row, 2), answered = iso_ms (row, 3), end = iso_ms (row, 4);
      CallLogEntry entry = {
        /* GNOME Calls stores the number as a tel: URI only for some origins */
        .number = (char *) (target && g_str_has_prefix (target, "tel:") ? target + 4 : target ? target : ""),
        .date = start,
        .duration = answered && end > answered ? (end - answered + 500) / 1000 : 0,
        .type = inbound ? (answered ? CALL_TYPE_INCOMING : CALL_TYPE_MISSED) : CALL_TYPE_OUTGOING,
        .presentation = target && *target ? PRESENTATION_ALLOWED : PRESENTATION_UNKNOWN,
        .is_read = TRUE,
      };

      if (start && call_log_add (self, &entry))
        n++;
    }
  exec (self, "COMMIT", NULL);
  sqlite3_finalize (row);
  sqlite3_close (old);
  g_message ("call log: took in %u calls from GNOME Calls", n);
}

CallLog *
call_log_open (const char *path, const char *import_path, GError **error)
{
  CallLog *self = g_new0 (CallLog, 1);
  g_autofree char *dir = g_path_get_dirname (path);
  sqlite3_stmt *version = NULL;
  int user_version = 0;

  g_mkdir_with_parents (dir, 0700);
  if (sqlite3_open (path, &self->db) != SQLITE_OK)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "call log: %s", sqlite3_errmsg (self->db));
      call_log_free (self);
      return NULL;
    }
  /* a call that ends as the phone loses power should still be there: WAL, and synced at each commit */
  exec (self, "PRAGMA journal_mode=WAL", NULL);
  if (sqlite3_prepare_v2 (self->db, "PRAGMA user_version", -1, &version, NULL) == SQLITE_OK
      && sqlite3_step (version) == SQLITE_ROW)
    user_version = sqlite3_column_int (version, 0);
  sqlite3_finalize (version);

  if (!exec (self, schema, error))
    {
      call_log_free (self);
      return NULL;
    }
  if (user_version == 0)
    {
      g_autofree char *set = g_strdup_printf ("PRAGMA user_version=%d", SCHEMA_VERSION);

      /* new: the history GNOME Calls kept until now comes along */
      if (import_path && g_file_test (import_path, G_FILE_TEST_IS_REGULAR))
        import_gnome_calls (self, import_path);
      exec (self, set, NULL);
    }
  return self;
}

void
call_log_free (CallLog *self)
{
  if (!self)
    return;
  sqlite3_close (self->db);
  g_free (self);
}

gint64
call_log_add (CallLog *self, const CallLogEntry *entry)
{
  sqlite3_stmt *insert = NULL;
  gint64 id = 0;

  if (sqlite3_prepare_v2 (self->db,
                          "INSERT INTO calls (number, date, duration, type, presentation, country_iso, is_read, new)"
                          " VALUES (?, ?, ?, ?, ?, ?, ?, ?)", -1, &insert, NULL) != SQLITE_OK)
    {
      g_warning ("call log: %s", sqlite3_errmsg (self->db));
      return 0;
    }
  sqlite3_bind_text (insert, 1, entry->number ? entry->number : "", -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (insert, 2, entry->date);
  sqlite3_bind_int64 (insert, 3, entry->duration);
  sqlite3_bind_int (insert, 4, entry->type);
  sqlite3_bind_int (insert, 5, entry->presentation ? entry->presentation : PRESENTATION_ALLOWED);
  if (entry->country_iso)
    sqlite3_bind_text (insert, 6, entry->country_iso, -1, SQLITE_TRANSIENT);
  else
    sqlite3_bind_null (insert, 6);
  sqlite3_bind_int (insert, 7, entry->is_read);
  /* only a missed call is news (CallLogNotificationsQueryHelper reads type = MISSED and new = 1) */
  sqlite3_bind_int (insert, 8, !entry->is_read);
  if (sqlite3_step (insert) == SQLITE_DONE)
    id = sqlite3_last_insert_rowid (self->db);
  else
    g_warning ("call log: %s", sqlite3_errmsg (self->db));
  sqlite3_finalize (insert);
  return id;
}

GPtrArray *
call_log_list (CallLog *self, guint limit)
{
  GPtrArray *entries = g_ptr_array_new_with_free_func ((GDestroyNotify) call_log_entry_free);
  sqlite3_stmt *select = NULL;

  if (sqlite3_prepare_v2 (self->db,
                          "SELECT _id, number, date, duration, type, presentation, country_iso, is_read, new"
                          " FROM calls ORDER BY date DESC, _id DESC LIMIT ?", -1, &select, NULL) != SQLITE_OK)
    {
      g_warning ("call log: %s", sqlite3_errmsg (self->db));
      return entries;
    }
  sqlite3_bind_int (select, 1, limit);
  while (sqlite3_step (select) == SQLITE_ROW)
    {
      CallLogEntry *entry = g_new0 (CallLogEntry, 1);
      const char *country = (const char *) sqlite3_column_text (select, 6);

      entry->id = sqlite3_column_int64 (select, 0);
      entry->number = g_strdup ((const char *) sqlite3_column_text (select, 1));
      entry->date = sqlite3_column_int64 (select, 2);
      entry->duration = sqlite3_column_int64 (select, 3);
      entry->type = sqlite3_column_int (select, 4);
      entry->presentation = sqlite3_column_int (select, 5);
      entry->country_iso = g_strdup (country);
      entry->is_read = sqlite3_column_int (select, 7);
      entry->is_new = sqlite3_column_int (select, 8);
      g_ptr_array_add (entries, entry);
    }
  sqlite3_finalize (select);
  return entries;
}

guint
call_log_delete (CallLog *self, const gint64 *ids, guint n_ids)
{
  sqlite3_stmt *delete = NULL;
  guint n = 0;

  if (sqlite3_prepare_v2 (self->db, "DELETE FROM calls WHERE _id = ?", -1, &delete, NULL) != SQLITE_OK)
    return 0;
  exec (self, "BEGIN", NULL);
  for (guint i = 0; i < n_ids; i++)
    {
      sqlite3_bind_int64 (delete, 1, ids[i]);
      if (sqlite3_step (delete) == SQLITE_DONE)
        n += sqlite3_changes (self->db);
      sqlite3_reset (delete);
    }
  exec (self, "COMMIT", NULL);
  sqlite3_finalize (delete);
  return n;
}

void
call_log_clear (CallLog *self)
{
  exec (self, "DELETE FROM calls", NULL);
}

guint
call_log_mark_missed_read (CallLog *self, gint64 id)
{
  g_autofree char *sql = g_strdup_printf ("UPDATE calls SET is_read = 1, new = 0 WHERE type = %d AND"
                                          " (is_read = 0 OR new = 1)%s", CALL_TYPE_MISSED,
                                          id ? " AND _id = ?" : "");
  sqlite3_stmt *update = NULL;
  guint n = 0;

  if (sqlite3_prepare_v2 (self->db, sql, -1, &update, NULL) != SQLITE_OK)
    return 0;
  if (id)
    sqlite3_bind_int64 (update, 1, id);
  if (sqlite3_step (update) == SQLITE_DONE)
    n = sqlite3_changes (self->db);
  sqlite3_finalize (update);
  return n;
}

guint
call_log_count_new_missed (CallLog *self)
{
  sqlite3_stmt *count = NULL;
  guint n = 0;

  if (sqlite3_prepare_v2 (self->db, "SELECT count(*) FROM calls WHERE type = ? AND new = 1 AND is_read = 0", -1,
                          &count, NULL) != SQLITE_OK)
    return 0;
  sqlite3_bind_int (count, 1, CALL_TYPE_MISSED);
  if (sqlite3_step (count) == SQLITE_ROW)
    n = sqlite3_column_int (count, 0);
  sqlite3_finalize (count);
  return n;
}

GVariant *
call_log_entry_to_variant (const CallLogEntry *entry)
{
  GVariantBuilder dict;

  g_variant_builder_init (&dict, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&dict, "{sv}", "_id", g_variant_new_int64 (entry->id));
  g_variant_builder_add (&dict, "{sv}", "number", g_variant_new_string (entry->number ? entry->number : ""));
  g_variant_builder_add (&dict, "{sv}", "date", g_variant_new_int64 (entry->date));
  g_variant_builder_add (&dict, "{sv}", "duration", g_variant_new_int64 (entry->duration));
  g_variant_builder_add (&dict, "{sv}", "type", g_variant_new_uint32 (entry->type));
  g_variant_builder_add (&dict, "{sv}", "presentation", g_variant_new_uint32 (entry->presentation));
  if (entry->country_iso)
    g_variant_builder_add (&dict, "{sv}", "country_iso", g_variant_new_string (entry->country_iso));
  g_variant_builder_add (&dict, "{sv}", "is_read", g_variant_new_boolean (entry->is_read));
  g_variant_builder_add (&dict, "{sv}", "new", g_variant_new_boolean (entry->is_new));
  return g_variant_builder_end (&dict);
}

CallLogEntry *
call_log_entry_from_variant (GVariant *dict)
{
  CallLogEntry *entry = g_new0 (CallLogEntry, 1);
  guint32 type = 0, presentation = PRESENTATION_ALLOWED;

  g_variant_lookup (dict, "_id", "x", &entry->id);
  if (!g_variant_lookup (dict, "number", "s", &entry->number))
    entry->number = g_strdup ("");
  g_variant_lookup (dict, "date", "x", &entry->date);
  g_variant_lookup (dict, "duration", "x", &entry->duration);
  g_variant_lookup (dict, "type", "u", &type);
  g_variant_lookup (dict, "presentation", "u", &presentation);
  g_variant_lookup (dict, "country_iso", "s", &entry->country_iso);
  g_variant_lookup (dict, "is_read", "b", &entry->is_read);
  g_variant_lookup (dict, "new", "b", &entry->is_new);
  entry->type = type;
  entry->presentation = presentation;
  return entry;
}
