/* term-input: the bytes the extra keys send (term-keys.h), and a typed character with the extra keys' Ctrl and Alt on
 * it, as xterm (and VTE, for a keyboard's own Ctrl and Alt) sends them. No GTK: the app and the tests share it. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  TERM_MOD_CTRL = 1 << 0,
  TERM_MOD_ALT  = 1 << 1,
} TermMods;

typedef enum {
  TERM_KEY_ESC,
  TERM_KEY_TAB,
  TERM_KEY_LEFT,
  TERM_KEY_UP,
  TERM_KEY_DOWN,
  TERM_KEY_RIGHT,
} TermKey;

/* A key: Esc is ESC and Tab is HT, either behind an ESC with Alt (Ctrl changes neither). A cursor key is ESC [ A, or
 * ESC O A in application cursor mode (DECCKM, `app_cursor`), and with modifiers ESC [ 1 ; m A whatever the mode, m
 * being 1, +2 for Alt, +4 for Ctrl. The bytes go into `buf` (NUL-terminated); returns their number. */
gsize term_input_key  (TermKey key, TermMods mods, gboolean app_cursor, char *buf, gsize size);

/* One typed character (`length` bytes of UTF-8) under modifiers: Ctrl makes @ and space NUL, a letter of either case
 * its control character (1 to 26), [ ESC, \ FS, ] GS, ^ RS, _ US and ? DEL, and leaves any other character as it is;
 * Alt puts an ESC before it. The bytes go into `buf` (NUL-terminated); returns their number, or 0 when `text` is not
 * one character. */
gsize term_input_char (const char *text, gsize length, TermMods mods, char *buf, gsize size);

G_END_DECLS
