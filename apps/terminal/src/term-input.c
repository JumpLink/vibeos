/* term-input: see term-input.h */
#include "term-input.h"

#include <string.h>

static gsize
put (char *buf, gsize size, const char *bytes, gsize n)
{
  if (n + 1 > size)
    return 0;
  memcpy (buf, bytes, n);
  buf[n] = '\0';
  return n;
}

gsize
term_input_key (TermKey key, TermMods mods, gboolean app_cursor, char *buf, gsize size)
{
  static const char finals[] = { [TERM_KEY_UP] = 'A', [TERM_KEY_DOWN] = 'B', [TERM_KEY_RIGHT] = 'C',
                                 [TERM_KEY_LEFT] = 'D' };
  char out[16];
  int n;

  switch (key) {
  case TERM_KEY_ESC:
  case TERM_KEY_TAB:
    n = 0;
    if (mods & TERM_MOD_ALT)
      out[n++] = '\033';
    out[n++] = key == TERM_KEY_ESC ? '\033' : '\t';
    return put (buf, size, out, n);
  case TERM_KEY_LEFT:
  case TERM_KEY_UP:
  case TERM_KEY_DOWN:
  case TERM_KEY_RIGHT:
    if (mods)
      n = g_snprintf (out, sizeof out, "\033[1;%d%c", 1 + (mods & TERM_MOD_ALT ? 2 : 0) + (mods & TERM_MOD_CTRL ? 4 : 0),
                      finals[key]);
    else
      n = g_snprintf (out, sizeof out, "\033%c%c", app_cursor ? 'O' : '[', finals[key]);
    return put (buf, size, out, n);
  default:
    return 0;
  }
}

/* the control character Ctrl makes of c, -1 for none */
static int
control (gunichar c)
{
  if (c == '@' || c == ' ')
    return 0;
  if (c >= 'a' && c <= 'z')
    return (int) (c - 'a' + 1);
  if (c >= 'A' && c <= '_')      /* A-Z [ \ ] ^ _ */
    return (int) (c - 'A' + 1);
  if (c == '?')
    return 0x7f;
  return -1;
}

gsize
term_input_char (const char *text, gsize length, TermMods mods, char *buf, gsize size)
{
  const char *end = text + length;
  char out[16];
  gsize n = 0;
  gunichar c;

  if (!text || length == 0 || length > 6 || !g_utf8_validate (text, length, NULL)
      || g_utf8_next_char (text) != end)
    return 0;
  c = g_utf8_get_char (text);
  if (mods & TERM_MOD_ALT)
    out[n++] = '\033';
  if ((mods & TERM_MOD_CTRL) && control (c) != -1) {
    out[n++] = (char) control (c);
  } else {
    memcpy (out + n, text, length);
    n += length;
  }
  return put (buf, size, out, n);
}
