/* test-input: the extra keys' bytes, and Ctrl and Alt on a typed character (term-input.c) */
#include "term-input.h"

#include <string.h>

static void
check_key (TermKey key, TermMods mods, gboolean app_cursor, const char *want)
{
  char buf[32];
  gsize n = term_input_key (key, mods, app_cursor, buf, sizeof buf);

  g_assert_cmpmem (buf, n, want, strlen (want));
}

static void
check_char (const char *text, TermMods mods, const char *want, gsize want_len)
{
  char buf[32];
  gsize n = term_input_char (text, strlen (text), mods, buf, sizeof buf);

  g_assert_cmpmem (buf, n, want, want_len);
}

static void
test_keys (void)
{
  check_key (TERM_KEY_ESC, 0, FALSE, "\033");
  check_key (TERM_KEY_ESC, TERM_MOD_ALT, FALSE, "\033\033");
  check_key (TERM_KEY_ESC, TERM_MOD_CTRL, FALSE, "\033");
  check_key (TERM_KEY_TAB, 0, TRUE, "\t");
  check_key (TERM_KEY_TAB, TERM_MOD_ALT | TERM_MOD_CTRL, FALSE, "\033\t");
  check_key (TERM_KEY_UP, 0, FALSE, "\033[A");
  check_key (TERM_KEY_DOWN, 0, TRUE, "\033OB");
  check_key (TERM_KEY_RIGHT, TERM_MOD_CTRL, FALSE, "\033[1;5C");
  check_key (TERM_KEY_LEFT, TERM_MOD_ALT, TRUE, "\033[1;3D");
  check_key (TERM_KEY_UP, TERM_MOD_CTRL | TERM_MOD_ALT, FALSE, "\033[1;7A");
}

static void
test_ctrl (void)
{
  check_char ("c", TERM_MOD_CTRL, "\003", 1);
  check_char ("C", TERM_MOD_CTRL, "\003", 1);
  check_char ("a", TERM_MOD_CTRL, "\001", 1);
  check_char ("z", TERM_MOD_CTRL, "\032", 1);
  check_char (" ", TERM_MOD_CTRL, "\0", 1);
  check_char ("@", TERM_MOD_CTRL, "\0", 1);
  check_char ("[", TERM_MOD_CTRL, "\033", 1);
  check_char ("\\", TERM_MOD_CTRL, "\034", 1);
  check_char ("_", TERM_MOD_CTRL, "\037", 1);
  check_char ("?", TERM_MOD_CTRL, "\177", 1);
  /* no control character of its own: as it is */
  check_char ("1", TERM_MOD_CTRL, "1", 1);
  check_char ("\r", TERM_MOD_CTRL, "\r", 1);
  check_char ("é", TERM_MOD_CTRL, "é", strlen ("é"));
  check_char ("x", 0, "x", 1);
}

static void
test_alt (void)
{
  check_char ("x", TERM_MOD_ALT, "\033x", 2);
  check_char ("b", TERM_MOD_ALT | TERM_MOD_CTRL, "\033\002", 2);
  check_char ("\177", TERM_MOD_ALT, "\033\177", 2);
  check_char ("ä", TERM_MOD_ALT, "\033ä", 1 + strlen ("ä"));
}

static void
test_not_one (void)
{
  char buf[32];

  g_assert_cmpuint (term_input_char ("ab", 2, TERM_MOD_CTRL, buf, sizeof buf), ==, 0);
  g_assert_cmpuint (term_input_char ("", 0, TERM_MOD_CTRL, buf, sizeof buf), ==, 0);
  g_assert_cmpuint (term_input_char ("\xff", 1, TERM_MOD_CTRL, buf, sizeof buf), ==, 0);
  /* too small a buffer: nothing */
  g_assert_cmpuint (term_input_char ("x", 1, TERM_MOD_ALT, buf, 2), ==, 0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/input/keys", test_keys);
  g_test_add_func ("/input/ctrl", test_ctrl);
  g_test_add_func ("/input/alt", test_alt);
  g_test_add_func ("/input/not-one", test_not_one);
  return g_test_run ();
}
