/* expression-db: see expression-db.h */
#include "expression-db.h"

#include <sqlite3.h>

/* ExpressionDB.ExpressionEntry's flags */
#define DEGREE_MODE  2
#define LONG_TIMEOUT 1

struct _ExpressionDB {
  sqlite3 *db;
  long     min_index;   /* the smallest hidden index so far, or MAXIMUM_MIN_INDEX */
  long     max_index;   /* the largest history index so far, or 0 */
};

static void
create_tables (ExpressionDB *self)
{
  sqlite3_exec (self->db,
                "CREATE TABLE IF NOT EXISTS expressions (_id INTEGER PRIMARY KEY, expression BLOB,"
                " flags INTEGER, timeStamp INTEGER);"
                "CREATE INDEX IF NOT EXISTS timestamp_index ON expressions(timeStamp);",
                NULL, NULL, NULL);
}

static long
query_long (ExpressionDB *self, const char *sql, long fallback)
{
  sqlite3_stmt *stmt;
  long result = fallback;

  if (sqlite3_prepare_v2 (self->db, sql, -1, &stmt, NULL) != SQLITE_OK)
    return fallback;
  if (sqlite3_step (stmt) == SQLITE_ROW && sqlite3_column_type (stmt, 0) != SQLITE_NULL)
    result = sqlite3_column_int64 (stmt, 0);
  sqlite3_finalize (stmt);
  return result;
}

ExpressionDB *
expression_db_open (const char *path)
{
  ExpressionDB *self = g_new0 (ExpressionDB, 1);
  g_autofree char *dir = g_path_get_dirname (path);

  g_mkdir_with_parents (dir, 0700);
  if (sqlite3_open (path, &self->db) != SQLITE_OK) {
    g_warning ("cannot open %s (%s): history is not kept", path, sqlite3_errmsg (self->db));
    sqlite3_close (self->db);
    sqlite3_open (":memory:", &self->db);
  }
  create_tables (self);
  self->min_index = MIN (query_long (self, "SELECT MIN(_id) FROM expressions", EXPRESSION_DB_MAXIMUM_MIN_INDEX),
                         EXPRESSION_DB_MAXIMUM_MIN_INDEX);
  self->max_index = MAX (query_long (self, "SELECT MAX(_id) FROM expressions", 0), 0);
  return self;
}

void
expression_db_close (ExpressionDB *self)
{
  if (!self)
    return;
  sqlite3_close (self->db);
  g_free (self);
}

long
expression_db_add_row (ExpressionDB *self, gboolean hidden, GBytes *expression, gboolean degree_mode,
                       gboolean long_timeout, gint64 *time_stamp)
{
  long index = hidden ? self->min_index - 1 : self->max_index + 1;
  gint64 now = g_get_real_time () / 1000;
  sqlite3_stmt *stmt;
  gsize size;
  const void *data = g_bytes_get_data (expression, &size);

  if (hidden)
    self->min_index = index;
  else
    self->max_index = index;
  if (sqlite3_prepare_v2 (self->db, "INSERT INTO expressions (_id, expression, flags, timeStamp) VALUES (?, ?, ?, ?)",
                          -1, &stmt, NULL) == SQLITE_OK) {
    sqlite3_bind_int64 (stmt, 1, index);
    sqlite3_bind_blob (stmt, 2, data, size, SQLITE_TRANSIENT);
    sqlite3_bind_int (stmt, 3, (degree_mode ? DEGREE_MODE : 0) | (long_timeout ? LONG_TIMEOUT : 0));
    sqlite3_bind_int64 (stmt, 4, now);
    if (sqlite3_step (stmt) != SQLITE_DONE)
      g_warning ("cannot save an expression: %s", sqlite3_errmsg (self->db));
    sqlite3_finalize (stmt);
  }
  if (time_stamp)
    *time_stamp = now;
  return index;
}

gboolean
expression_db_get_row (ExpressionDB *self, long index, ExpressionRow *row)
{
  sqlite3_stmt *stmt;
  gboolean found = FALSE;

  if (sqlite3_prepare_v2 (self->db, "SELECT expression, flags, timeStamp FROM expressions WHERE _id = ?",
                          -1, &stmt, NULL) != SQLITE_OK)
    return FALSE;
  sqlite3_bind_int64 (stmt, 1, index);
  if (sqlite3_step (stmt) == SQLITE_ROW) {
    int flags = sqlite3_column_int (stmt, 1);
    row->expression = g_bytes_new (sqlite3_column_blob (stmt, 0), sqlite3_column_bytes (stmt, 0));
    row->degree_mode = (flags & DEGREE_MODE) != 0;
    row->long_timeout = (flags & LONG_TIMEOUT) != 0;
    row->time_stamp = sqlite3_column_int64 (stmt, 2);
    found = TRUE;
  }
  sqlite3_finalize (stmt);
  return found;
}

void
expression_row_clear (ExpressionRow *row)
{
  g_clear_pointer (&row->expression, g_bytes_unref);
}

void
expression_db_erase_all (ExpressionDB *self)
{
  sqlite3_exec (self->db, "DROP INDEX IF EXISTS timestamp_index; DROP TABLE IF EXISTS expressions; VACUUM;",
                NULL, NULL, NULL);
  create_tables (self);
  self->min_index = EXPRESSION_DB_MAXIMUM_MIN_INDEX;
  self->max_index = 0;
}

long
expression_db_get_min_index (ExpressionDB *self)
{
  return self->min_index;
}

long
expression_db_get_max_index (ExpressionDB *self)
{
  return self->max_index;
}
