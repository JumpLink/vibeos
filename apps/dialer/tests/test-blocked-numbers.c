/* blocked-numbers: a number blocked in one form is blocked in all of them,
 * unblocking takes whatever form it was put on in, and a second process sees the change */
#include "../src/blocked-numbers.h"

#include <glib/gstdio.h>
#include <libebook-contacts/libebook-contacts.h>

typedef struct {
  char *dir;
  char *path;
} Fixture;

static void
fixture_set_up (Fixture *f, gconstpointer data)
{
  f->dir = g_dir_make_tmp ("test-blocked-XXXXXX", NULL);
  f->path = g_build_filename (f->dir, "vibeos-phone", "blocked_numbers.db", NULL);
}

static void
fixture_tear_down (Fixture *f, gconstpointer data)
{
  g_autofree char *sub = g_path_get_dirname (f->path);

  g_unlink (f->path);
  g_rmdir (sub);
  g_rmdir (f->dir);
  g_free (f->path);
  g_free (f->dir);
}

static void
test_forms (Fixture *f, gconstpointer data)
{
  g_autoptr(BlockedNumbers) blocked = blocked_numbers_new (f->path);
  g_autoptr(GError) error = NULL;
  g_auto(GStrv) list = NULL;

  if (!e_phone_number_is_supported ())
    {
      g_test_skip ("EDS built without libphonenumber");
      return;
    }
  g_assert_false (blocked_numbers_contains (blocked, "+493023575124", "DE"));
  g_assert_true (blocked_numbers_add (blocked, "030 23575124", "DE", &error));
  g_assert_no_error (error);
  /* the same line however it is written: with the country code, with spaces, as the network sends it */
  g_assert_true (blocked_numbers_contains (blocked, "+49 30 23575124", "DE"));
  g_assert_true (blocked_numbers_contains (blocked, "+493023575124", "DE"));
  g_assert_false (blocked_numbers_contains (blocked, "+493023575125", "DE"));
  g_assert_false (blocked_numbers_contains (blocked, "", "DE"));
  /* blocked again in another form: still one entry, the first form */
  g_assert_true (blocked_numbers_add (blocked, "+493023575124", "DE", &error));
  list = blocked_numbers_list (blocked);
  g_assert_cmpuint (g_strv_length (list), ==, 1);
  g_assert_cmpstr (list[0], ==, "030 23575124");
  /* unblocked in a third form */
  g_assert_true (blocked_numbers_remove (blocked, "+49 30 23575124", "DE", &error));
  g_assert_no_error (error);
  g_assert_false (blocked_numbers_contains (blocked, "030 23575124", "DE"));
}

static void
test_names (Fixture *f, gconstpointer data)
{
  g_autoptr(BlockedNumbers) blocked = blocked_numbers_new (f->path);
  g_auto(GStrv) list = NULL;

  /* a sender that is a name matches only itself, not every other name and not a number */
  g_assert_true (blocked_numbers_add (blocked, "Vodafone", "DE", NULL));
  g_assert_true (blocked_numbers_contains (blocked, "Vodafone", "DE"));
  g_assert_false (blocked_numbers_contains (blocked, "Telekom", "DE"));
  g_assert_false (blocked_numbers_contains (blocked, "+493023575124", "DE"));
  g_assert_true (blocked_numbers_add (blocked, "+491701234567", "DE", NULL));
  /* the last blocked first */
  list = blocked_numbers_list (blocked);
  g_assert_cmpuint (g_strv_length (list), ==, 2);
  g_assert_cmpstr (list[0], ==, "+491701234567");
  g_assert_cmpstr (list[1], ==, "Vodafone");
}

static void
on_changed (BlockedNumbers *blocked, gpointer data)
{
  *(gboolean *) data = TRUE;
}

static gboolean
on_timeout (gpointer data)
{
  *(gboolean *) data = TRUE;
  return G_SOURCE_REMOVE;
}

static void
test_other_process (Fixture *f, gconstpointer data)
{
  /* two instances on one file stand for the Phone and the Messaging: one blocks, the other is told */
  g_autoptr(BlockedNumbers) phone = blocked_numbers_new (f->path);
  g_autoptr(BlockedNumbers) messaging = blocked_numbers_new (f->path);
  gboolean changed = FALSE, timed_out = FALSE;
  guint timeout;

  g_signal_connect (messaging, "changed", G_CALLBACK (on_changed), &changed);
  timeout = g_timeout_add_seconds (5, on_timeout, &timed_out);
  g_assert_true (blocked_numbers_add (phone, "+491701234567", "DE", NULL));
  while (!changed && !timed_out)
    g_main_context_iteration (NULL, TRUE);
  if (!timed_out)
    g_source_remove (timeout);
  g_assert_true (changed);
  g_assert_true (blocked_numbers_contains (messaging, "+491701234567", "DE"));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add ("/blocked-numbers/forms", Fixture, NULL, fixture_set_up, test_forms, fixture_tear_down);
  g_test_add ("/blocked-numbers/names", Fixture, NULL, fixture_set_up, test_names, fixture_tear_down);
  g_test_add ("/blocked-numbers/other-process", Fixture, NULL, fixture_set_up, test_other_process,
              fixture_tear_down);
  return g_test_run ();
}
