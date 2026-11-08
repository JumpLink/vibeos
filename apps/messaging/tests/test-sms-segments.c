/* sms-segments against SmsMessage.calculateLength's numbers */
#include "../src/sms-segments.h"

static void
check (const char *text, guint segments, guint remaining, gboolean unicode)
{
  SmsLength length = sms_segments_count (text);

  g_assert_cmpuint (length.segments, ==, segments);
  g_assert_cmpuint (length.remaining, ==, remaining);
  g_assert_cmpint (length.unicode, ==, unicode);
}

static void
test_gsm (void)
{
  g_autofree char *full = g_strnfill (160, 'a');
  g_autofree char *over = g_strnfill (161, 'a');
  g_autofree char *two = g_strnfill (306, 'a');

  check ("", 1, 160, FALSE);
  check ("Hello", 1, 155, FALSE);
  /* the extension table counts two */
  check ("{€}", 1, 154, FALSE);
  check (full, 1, 0, FALSE);
  /* past 160 each part holds 153 */
  check (over, 2, 145, FALSE);
  check (two, 2, 0, FALSE);
  check ("Grüße aus Köln", 1, 146, FALSE);
}

static void
test_unicode (void)
{
  g_autofree char *full = g_strnfill (69, 'a');
  g_autofree char *with_emoji = g_strconcat (full, "😀", NULL);

  /* a character outside the alphabet makes it UCS-2: 70 units */
  check ("Привет", 1, 64, TRUE);
  check ("ok 😀", 1, 65, TRUE);
  /* 69 + 2 units: two parts of 67 */
  check (with_emoji, 2, 63, TRUE);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/sms-segments/gsm", test_gsm);
  g_test_add_func ("/sms-segments/unicode", test_unicode);
  return g_test_run ();
}
