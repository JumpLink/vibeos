/* The blocked numbers (blocked-numbers.h). */
#include "blocked-numbers.h"
#include "phone-number.h"

#include <glib/gstdio.h>
#include <sqlite3.h>

struct _BlockedNumbers {
  GObject parent_instance;

  char *path;
  sqlite3 *db;
  GFileMonitor *monitor;        /* the database file: another process blocked or unblocked a number */
  GPtrArray *numbers;           /* char *, the list as it is now, the last blocked first */
};

G_DEFINE_FINAL_TYPE (BlockedNumbers, blocked_numbers, G_TYPE_OBJECT)

enum { CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* BlockedNumberContract.BlockedNumbers: the number as it was blocked, and its E.164 form when it has one. The E.164
 * column is kept for what reads the list as Android does; matching goes through libphonenumber either way. */
static const char schema[] =
  "CREATE TABLE IF NOT EXISTS blocked ("
  "  _id INTEGER PRIMARY KEY AUTOINCREMENT,"
  "  original_number TEXT NOT NULL UNIQUE,"
  "  e164_number TEXT"
  ");";

static char *
default_path (void)
{
  return g_build_filename (g_get_user_data_dir (), "neo-phone", "blocked_numbers.db", NULL);
}

/* --- reading */

static void
load (BlockedNumbers *self)
{
  sqlite3_stmt *row = NULL;

  g_ptr_array_set_size (self->numbers, 0);
  if (!self->db || sqlite3_prepare_v2 (self->db, "SELECT original_number FROM blocked ORDER BY _id DESC", -1, &row,
                                       NULL) != SQLITE_OK)
    return;
  while (sqlite3_step (row) == SQLITE_ROW)
    g_ptr_array_add (self->numbers, g_strdup ((const char *) sqlite3_column_text (row, 0)));
  sqlite3_finalize (row);
}

/* a sender with no digits is a name ("Vodafone"): libphonenumber would read it as nothing and match every other */
static gboolean
is_name (const char *number)
{
  for (const char *c = number; *c; c++)
    if (g_ascii_isdigit (*c))
      return FALSE;
  return TRUE;
}

static gboolean
same_number (const char *a, const char *b, const char *country)
{
  if (is_name (a) || is_name (b))
    return g_strcmp0 (a, b) == 0;
  return phone_number_equal (a, b, country);
}

/* the form the number is on the list in, or NULL */
static const char *
find (BlockedNumbers *self, const char *number, const char *country)
{
  if (!number || !*number)
    return NULL;
  for (guint i = 0; i < self->numbers->len; i++)
    if (same_number (number, self->numbers->pdata[i], country))
      return self->numbers->pdata[i];
  return NULL;
}

gboolean
blocked_numbers_contains (BlockedNumbers *self, const char *number, const char *country)
{
  g_return_val_if_fail (BLOCKED_IS_NUMBERS (self), FALSE);
  return find (self, number, country) != NULL;
}

GStrv
blocked_numbers_list (BlockedNumbers *self)
{
  GStrvBuilder *list = g_strv_builder_new ();

  g_return_val_if_fail (BLOCKED_IS_NUMBERS (self), NULL);
  for (guint i = 0; i < self->numbers->len; i++)
    g_strv_builder_add (list, self->numbers->pdata[i]);
  return g_strv_builder_unref_to_strv (list);
}

/* --- changing */

static gboolean
change (BlockedNumbers *self, const char *sql, const char *first, const char *second, GError **error)
{
  sqlite3_stmt *statement = NULL;
  int result;

  if (!self->db)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED, "blocked numbers: %s cannot be opened",
                   self->path);
      return FALSE;
    }
  if (sqlite3_prepare_v2 (self->db, sql, -1, &statement, NULL) != SQLITE_OK)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "blocked numbers: %s", sqlite3_errmsg (self->db));
      return FALSE;
    }
  sqlite3_bind_text (statement, 1, first, -1, SQLITE_TRANSIENT);
  if (second)
    sqlite3_bind_text (statement, 2, second, -1, SQLITE_TRANSIENT);
  result = sqlite3_step (statement);
  sqlite3_finalize (statement);
  if (result != SQLITE_DONE)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "blocked numbers: %s", sqlite3_errmsg (self->db));
      return FALSE;
    }
  /* this process sees its own change at once; the others through their file monitors */
  load (self);
  g_signal_emit (self, signals[CHANGED], 0);
  return TRUE;
}

gboolean
blocked_numbers_add (BlockedNumbers *self, const char *number, const char *country, GError **error)
{
  g_autofree char *e164 = NULL;

  g_return_val_if_fail (BLOCKED_IS_NUMBERS (self), FALSE);
  if (!number || !*number)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "blocked numbers: no number");
      return FALSE;
    }
  if (find (self, number, country))
    return TRUE;
  e164 = is_name (number) ? NULL : phone_number_canonical (number, country);
  return change (self, "INSERT OR IGNORE INTO blocked (original_number, e164_number) VALUES (?1, ?2)", number,
                 e164 && g_str_has_prefix (e164, "+") ? e164 : NULL, error);
}

gboolean
blocked_numbers_remove (BlockedNumbers *self, const char *number, const char *country, GError **error)
{
  g_autofree char *listed = NULL;

  g_return_val_if_fail (BLOCKED_IS_NUMBERS (self), FALSE);
  /* a copy: change() reloads the list that find() points into */
  listed = g_strdup (find (self, number, country));
  if (!listed)
    return TRUE;
  return change (self, "DELETE FROM blocked WHERE original_number = ?1", listed, NULL, error);
}

/* --- the database */

static void
on_file_changed (GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event, gpointer data)
{
  BlockedNumbers *self = data;
  GPtrArray *before = self->numbers;
  gboolean same;

  if (event != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT && event != G_FILE_MONITOR_EVENT_CREATED)
    return;
  /* load into a new array, to tell a real change from our own write coming back */
  self->numbers = g_ptr_array_new_with_free_func (g_free);
  load (self);
  same = before->len == self->numbers->len;
  for (guint i = 0; same && i < before->len; i++)
    same = g_str_equal (before->pdata[i], self->numbers->pdata[i]);
  g_ptr_array_unref (before);
  if (!same)
    g_signal_emit (self, signals[CHANGED], 0);
}

static void
blocked_numbers_finalize (GObject *object)
{
  BlockedNumbers *self = BLOCKED_NUMBERS (object);

  if (self->monitor)
    g_signal_handlers_disconnect_by_data (self->monitor, self);
  g_clear_object (&self->monitor);
  g_clear_pointer (&self->db, sqlite3_close);
  g_clear_pointer (&self->numbers, g_ptr_array_unref);
  g_free (self->path);
  G_OBJECT_CLASS (blocked_numbers_parent_class)->finalize (object);
}

static void
blocked_numbers_class_init (BlockedNumbersClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = blocked_numbers_finalize;
  signals[CHANGED] = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                   G_TYPE_NONE, 0);
}

static void
blocked_numbers_init (BlockedNumbers *self)
{
  self->numbers = g_ptr_array_new_with_free_func (g_free);
}

BlockedNumbers *
blocked_numbers_new (const char *path)
{
  BlockedNumbers *self = g_object_new (BLOCKED_TYPE_NUMBERS, NULL);
  g_autofree char *dir = NULL;
  g_autoptr(GFile) file = NULL;
  char *message = NULL;

  self->path = path ? g_strdup (path) : default_path ();
  dir = g_path_get_dirname (self->path);
  g_mkdir_with_parents (dir, 0700);
  /* the default rollback journal, not WAL: every change is then a write to the file itself, which is what the
   * other processes' monitors see (with WAL it would sit in the -wal file until a checkpoint) */
  if (sqlite3_open (self->path, &self->db) != SQLITE_OK
      || sqlite3_exec (self->db, schema, NULL, NULL, &message) != SQLITE_OK)
    {
      g_warning ("blocked numbers: %s: %s", self->path, message ? message : sqlite3_errmsg (self->db));
      sqlite3_free (message);
      g_clear_pointer (&self->db, sqlite3_close);
      return self;
    }
  /* two processes blocking at the same moment wait for each other a little rather than fail */
  sqlite3_busy_timeout (self->db, 2000);
  load (self);
  file = g_file_new_for_path (self->path);
  self->monitor = g_file_monitor_file (file, G_FILE_MONITOR_NONE, NULL, NULL);
  if (self->monitor)
    g_signal_connect (self->monitor, "changed", G_CALLBACK (on_file_changed), self);
  return self;
}

BlockedNumbers *
blocked_numbers_get_default (void)
{
  static BlockedNumbers *list;

  if (!list)
    list = blocked_numbers_new (NULL);
  return list;
}
