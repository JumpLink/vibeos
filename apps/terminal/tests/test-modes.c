/* TermModes: the modes read from a program's output as VTE reads them, and what a wheel step sends for them */
#include "term-modes.h"

#include <string.h>

static void
feed (TermModes *modes, const char *text)
{
  term_modes_feed (modes, (const guint8 *) text, strlen (text));
}

/* the bytes one step sends, as a string */
static char *
wheel (const TermModes *modes, gboolean down, long column, long row)
{
  char buf[32];
  gsize n = term_modes_wheel (modes, down, column, row, buf, sizeof buf);
  return g_strndup (buf, n);
}

static void
test_defaults (void)
{
  TermModes m;

  term_modes_init (&m);
  g_assert_true (term_modes_has (&m, TERM_MODE_ALT_SCROLL));
  g_assert_false (term_modes_tracks_mouse (&m));
  g_assert_cmpint (term_modes_scroll (&m), ==, TERM_SCROLL_HISTORY);
  g_assert_cmpuint (term_modes_wheel (&m, TRUE, 0, 0, (char[32]) { 0 }, 32), ==, 0);
}

/* less and man: the alternate screen, with cursor keys in normal or application mode */
static void
test_alternate_screen (void)
{
  TermModes m;
  g_autofree char *down = NULL, *up = NULL, *app_down = NULL;

  term_modes_init (&m);
  feed (&m, "\033[?1049h\033[22;0;0t\033[?1h\033=");
  g_assert_true (m.alt_screen);
  g_assert_cmpint (term_modes_scroll (&m), ==, TERM_SCROLL_KEYS);
  app_down = wheel (&m, TRUE, 5, 5);
  g_assert_cmpstr (app_down, ==, "\033OB");
  feed (&m, "\033[?1l");
  down = wheel (&m, TRUE, 5, 5);
  up = wheel (&m, FALSE, 5, 5);
  g_assert_cmpstr (down, ==, "\033[B");
  g_assert_cmpstr (up, ==, "\033[A");
  feed (&m, "\033[?1049l");
  g_assert_false (m.alt_screen);
  g_assert_cmpint (term_modes_scroll (&m), ==, TERM_SCROLL_HISTORY);

  /* 47 and 1047 switch it too, and 1007 off leaves the history to scroll */
  feed (&m, "\033[?47h");
  g_assert_cmpint (term_modes_scroll (&m), ==, TERM_SCROLL_KEYS);
  feed (&m, "\033[?1007l");
  g_assert_cmpint (term_modes_scroll (&m), ==, TERM_SCROLL_HISTORY);
  feed (&m, "\033[?1007h\033[?47l\033[?1047h");
  g_assert_cmpint (term_modes_scroll (&m), ==, TERM_SCROLL_KEYS);
}

/* htop and mc: the mouse tracked, reports in SGR or the original encoding */
static void
test_mouse (void)
{
  TermModes m;
  g_autofree char *sgr_down = NULL, *sgr_up = NULL, *x10 = NULL, *far = NULL;
  char buf[64];
  gsize n;

  term_modes_init (&m);
  feed (&m, "\033[?1049h\033[?1000;1006h");
  g_assert_true (term_modes_tracks_mouse (&m));
  g_assert_cmpint (term_modes_scroll (&m), ==, TERM_SCROLL_MOUSE);
  sgr_down = wheel (&m, TRUE, 9, 4);
  sgr_up = wheel (&m, FALSE, 0, 0);
  g_assert_cmpstr (sgr_down, ==, "\033[<65;10;5M");
  g_assert_cmpstr (sgr_up, ==, "\033[<64;1;1M");
  n = term_modes_click (&m, 2, 3, buf, sizeof buf);
  g_assert_cmpmem (buf, n, "\033[<0;3;4M\033[<0;3;4m", 18);

  /* without 1006: CSI M and three bytes of 32 + the value */
  feed (&m, "\033[?1006l");
  x10 = wheel (&m, TRUE, 9, 4);
  g_assert_cmpstr (x10, ==, "\033[M\x61\x2a\x25");
  n = term_modes_click (&m, 0, 0, buf, sizeof buf);
  g_assert_cmpmem (buf, n, "\033[M\x20\x21\x21\033[M\x23\x21\x21", 12);
  /* a cell past 223 has no such encoding: nothing is sent */
  far = wheel (&m, TRUE, 300, 0);
  g_assert_cmpstr (far, ==, "");

  /* 9 alone reports the press only */
  feed (&m, "\033[?1000l\033[?9h");
  n = term_modes_click (&m, 0, 0, buf, sizeof buf);
  g_assert_cmpuint (n, ==, 6);
  feed (&m, "\033[?9l");
  g_assert_false (term_modes_tracks_mouse (&m));
  /* off again, on the alternate screen: cursor keys */
  g_assert_cmpint (term_modes_scroll (&m), ==, TERM_SCROLL_KEYS);
}

/* a sequence split across two reads, and one parameter list setting several modes */
static void
test_split (void)
{
  TermModes m;
  const char *seq = "\033[?1002;1006h";

  term_modes_init (&m);
  for (const char *p = seq; *p; p++)
    term_modes_feed (&m, (const guint8 *) p, 1);
  g_assert_true (term_modes_has (&m, TERM_MODE_MOUSE_BUTTON));
  g_assert_true (term_modes_has (&m, TERM_MODE_MOUSE_SGR));
}

/* what is no mode change: other private markers, other finals, intermediates, text in strings */
static void
test_not_modes (void)
{
  TermModes m;

  term_modes_init (&m);
  feed (&m, "\033[1049h");                       /* an ECMA mode, not a private one */
  feed (&m, "\033[>1049h");
  feed (&m, "\033[?1049$p");                     /* DECRQM: a query */
  feed (&m, "\033]0;[?1049h\007");               /* a title's text, ended by BEL */
  feed (&m, "\033]0;[?1000h\033\\");             /* ended by ST */
  feed (&m, "\033P1$r[?1000h\033\\");            /* a DCS's payload */
  feed (&m, "\033[?10\03049h");                  /* cancelled by CAN */
  g_assert_false (m.alt_screen);
  g_assert_false (term_modes_tracks_mouse (&m));
  /* and after all that, a real one still counts */
  feed (&m, "plain text \033(B\033[0m\033[?1049h");
  g_assert_true (m.alt_screen);
  /* an ESC in a string ends it and starts a sequence, as in VTE's parser */
  feed (&m, "\033]0;title\033[?1000h");
  g_assert_true (term_modes_tracks_mouse (&m));
}

/* XTSAVE and XTRESTORE, RIS and DECSTR */
static void
test_save_reset (void)
{
  TermModes m;

  term_modes_init (&m);
  feed (&m, "\033[?1000h\033[?1000s\033[?1000l");
  g_assert_false (term_modes_tracks_mouse (&m));
  feed (&m, "\033[?1000r");
  g_assert_true (term_modes_tracks_mouse (&m));

  /* DECSTR: the modes to their defaults, the screen as it is */
  feed (&m, "\033[?1049h\033[?1h\033[!p");
  g_assert_false (term_modes_tracks_mouse (&m));
  g_assert_false (term_modes_has (&m, TERM_MODE_APP_CURSOR));
  g_assert_true (m.alt_screen);
  /* nothing left to restore after a reset */
  feed (&m, "\033[?1000r");
  g_assert_false (term_modes_tracks_mouse (&m));

  /* RIS: back to the normal screen too */
  feed (&m, "\033[?1000h\033c");
  g_assert_false (m.alt_screen);
  g_assert_false (term_modes_tracks_mouse (&m));
  g_assert_true (term_modes_has (&m, TERM_MODE_ALT_SCROLL));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/modes/defaults", test_defaults);
  g_test_add_func ("/modes/alternate-screen", test_alternate_screen);
  g_test_add_func ("/modes/mouse", test_mouse);
  g_test_add_func ("/modes/split", test_split);
  g_test_add_func ("/modes/not-modes", test_not_modes);
  g_test_add_func ("/modes/save-reset", test_save_reset);
  return g_test_run ();
}
