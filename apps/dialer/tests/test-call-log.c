/* call-log against CallLog.Calls' rules: newest first, the missed calls that are news, and GNOME Calls' history
 * taken in once */
#include "../src/call-log.h"

#include <glib/gstdio.h>
#include <sqlite3.h>

static char *
temp_path (const char *name)
{
  g_autofree char *dir = g_dir_make_tmp ("test-call-log-XXXXXX", NULL);

  return g_build_filename (dir, name, NULL);
}

static void
test_add_and_list (void)
{
  g_autofree char *path = temp_path ("calls.db");
  g_autoptr(GError) error = NULL;
  CallLog *log = call_log_open (path, NULL, &error);
  CallLogEntry outgoing = { .number = "+493023575124", .date = 1000, .duration = 61, .type = CALL_TYPE_OUTGOING,
                            .presentation = PRESENTATION_ALLOWED, .country_iso = "DE", .is_read = TRUE };
  CallLogEntry missed = { .number = "", .date = 3000, .type = CALL_TYPE_MISSED,
                          .presentation = PRESENTATION_RESTRICTED };
  CallLogEntry incoming = { .number = "030123", .date = 2000, .duration = 5, .type = CALL_TYPE_INCOMING,
                            .is_read = TRUE };
  g_autoptr(GPtrArray) entries = NULL;
  CallLogEntry *first;

  g_assert_no_error (error);
  g_assert_cmpint (call_log_add (log, &outgoing), >, 0);
  g_assert_cmpint (call_log_add (log, &missed), >, 0);
  g_assert_cmpint (call_log_add (log, &incoming), >, 0);

  /* by date, newest first */
  entries = call_log_list (log, 10);
  g_assert_cmpuint (entries->len, ==, 3);
  first = entries->pdata[0];
  g_assert_cmpint (first->date, ==, 3000);
  g_assert_cmpint (first->type, ==, CALL_TYPE_MISSED);
  g_assert_cmpint (first->presentation, ==, PRESENTATION_RESTRICTED);
  g_assert_false (first->is_read);
  g_assert_true (first->is_new);
  g_assert_cmpint (((CallLogEntry *) entries->pdata[2])->duration, ==, 61);
  g_assert_cmpstr (((CallLogEntry *) entries->pdata[2])->country_iso, ==, "DE");
  /* an entry without a presentation counts as allowed */
  g_assert_cmpint (((CallLogEntry *) entries->pdata[1])->presentation, ==, PRESENTATION_ALLOWED);

  /* the limit */
  g_clear_pointer (&entries, g_ptr_array_unref);
  entries = call_log_list (log, 2);
  g_assert_cmpuint (entries->len, ==, 2);
  call_log_free (log);
}

static void
test_missed (void)
{
  g_autofree char *path = temp_path ("calls.db");
  CallLog *log = call_log_open (path, NULL, NULL);
  CallLogEntry missed = { .number = "1", .date = 1, .type = CALL_TYPE_MISSED };
  CallLogEntry rejected = { .number = "2", .date = 2, .type = CALL_TYPE_REJECTED, .is_read = TRUE };
  gint64 first, second;

  first = call_log_add (log, &missed);
  second = call_log_add (log, &missed);
  call_log_add (log, &rejected);
  /* only missed calls are news; a declined one is not */
  g_assert_cmpuint (call_log_count_new_missed (log), ==, 2);

  /* one seen (its notification swiped away), then all of them (Recents looked at) */
  g_assert_cmpuint (call_log_mark_missed_read (log, first), ==, 1);
  g_assert_cmpuint (call_log_count_new_missed (log), ==, 1);
  g_assert_cmpuint (call_log_mark_missed_read (log, first), ==, 0);
  g_assert_cmpuint (call_log_mark_missed_read (log, 0), ==, 1);
  g_assert_cmpuint (call_log_count_new_missed (log), ==, 0);
  (void) second;
  call_log_free (log);
}

static void
test_delete_and_clear (void)
{
  g_autofree char *path = temp_path ("calls.db");
  CallLog *log = call_log_open (path, NULL, NULL);
  CallLogEntry entry = { .number = "1", .date = 1, .type = CALL_TYPE_OUTGOING, .is_read = TRUE };
  gint64 ids[3];
  g_autoptr(GPtrArray) entries = NULL;

  for (guint i = 0; i < 3; i++)
    ids[i] = call_log_add (log, &entry);
  g_assert_cmpuint (call_log_delete (log, ids, 2), ==, 2);
  entries = call_log_list (log, 10);
  g_assert_cmpuint (entries->len, ==, 1);
  g_assert_cmpint (((CallLogEntry *) entries->pdata[0])->id, ==, ids[2]);

  call_log_clear (log);
  g_clear_pointer (&entries, g_ptr_array_unref);
  entries = call_log_list (log, 10);
  g_assert_cmpuint (entries->len, ==, 0);
  call_log_free (log);
}

static void
test_variant (void)
{
  CallLogEntry entry = { .id = 7, .number = "+4930", .date = 123, .duration = 9, .type = CALL_TYPE_INCOMING,
                         .presentation = PRESENTATION_ALLOWED, .country_iso = "DE", .is_read = TRUE };
  g_autoptr(GVariant) dict = g_variant_ref_sink (call_log_entry_to_variant (&entry));
  g_autoptr(CallLogEntry) back = call_log_entry_from_variant (dict);

  g_assert_cmpint (back->id, ==, 7);
  g_assert_cmpstr (back->number, ==, "+4930");
  g_assert_cmpint (back->date, ==, 123);
  g_assert_cmpint (back->duration, ==, 9);
  g_assert_cmpint (back->type, ==, CALL_TYPE_INCOMING);
  g_assert_cmpstr (back->country_iso, ==, "DE");
  g_assert_true (back->is_read);
}

/* GNOME Calls' records.db, as gom writes it */
static void
write_gnome_calls (const char *path)
{
  sqlite3 *db = NULL;

  g_assert_cmpint (sqlite3_open (path, &db), ==, SQLITE_OK);
  g_assert_cmpint (sqlite3_exec (db,
                                 "CREATE TABLE 'calls' ('id' INTEGER PRIMARY KEY AUTOINCREMENT,'target' TEXT,"
                                 "'inbound' INTEGER,'start' BLOB,'answered' BLOB,'end' BLOB, 'protocol' TEXT);"
                                 "INSERT INTO calls (target, inbound, start, answered, end, protocol) VALUES"
                                 " ('+491701234567', 1, '2026-11-07T14:31:06.215146Z', NULL,"
                                 "  '2026-11-07T14:31:20.000000Z', 'tel'),"
                                 " ('030123', 1, '2026-11-07T15:00:00.000000Z', '2026-11-07T15:00:05.000000Z',"
                                 "  '2026-11-07T15:01:05.400000Z', 'tel'),"
                                 " ('tel:112', 0, '2026-11-07T16:00:00.000000Z', NULL,"
                                 "  '2026-11-07T16:00:10.000000Z', 'tel');",
                                 NULL, NULL, NULL), ==, SQLITE_OK);
  sqlite3_close (db);
}

static void
test_import (void)
{
  g_autofree char *path = temp_path ("calls.db");
  g_autofree char *old = temp_path ("records.db");
  CallLog *log;
  g_autoptr(GPtrArray) entries = NULL;
  CallLogEntry *e;

  write_gnome_calls (old);
  log = call_log_open (path, old, NULL);
  entries = call_log_list (log, 10);
  g_assert_cmpuint (entries->len, ==, 3);

  /* outgoing, its tel: dropped */
  e = entries->pdata[0];
  g_assert_cmpint (e->type, ==, CALL_TYPE_OUTGOING);
  g_assert_cmpstr (e->number, ==, "112");
  g_assert_cmpint (e->duration, ==, 0);
  /* answered: incoming, connected 60.4 s */
  e = entries->pdata[1];
  g_assert_cmpint (e->type, ==, CALL_TYPE_INCOMING);
  g_assert_cmpint (e->duration, ==, 60);
  /* never answered: missed, but history, so not news */
  e = entries->pdata[2];
  g_assert_cmpint (e->type, ==, CALL_TYPE_MISSED);
  g_assert_cmpint (e->date, ==, 1794061866215);
  g_assert_true (e->is_read);
  g_assert_cmpuint (call_log_count_new_missed (log), ==, 0);
  call_log_free (log);

  /* once: a log that exists takes nothing in again */
  log = call_log_open (path, old, NULL);
  g_clear_pointer (&entries, g_ptr_array_unref);
  entries = call_log_list (log, 10);
  g_assert_cmpuint (entries->len, ==, 3);
  call_log_free (log);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/call-log/add-and-list", test_add_and_list);
  g_test_add_func ("/call-log/missed", test_missed);
  g_test_add_func ("/call-log/delete-and-clear", test_delete_and_clear);
  g_test_add_func ("/call-log/variant", test_variant);
  g_test_add_func ("/call-log/import", test_import);
  return g_test_run ();
}
