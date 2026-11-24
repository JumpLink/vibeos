/* TermModes: what the program in the terminal has asked of the terminal, read from its output on its way to VTE.
 *
 * VTE keeps these modes itself, and answers a mouse wheel by them: a program that tracks the mouse (htop, mc, vim
 * with mouse=a) gets wheel button reports, one on the alternate screen (less, man, vim) gets cursor keys, and anything
 * else scrolls the terminal's history. VTE has no wheel for a finger, though, and does not say which mode it is in.
 * So the app, which owns the pty and hands VTE everything the program writes, reads the same private modes from the
 * same bytes (DECSET and DECRST, XTSAVE and XTRESTORE, RIS and DECSTR as VTE takes them), and a scroll with two
 * fingers sends what VTE would send for a wheel.
 *
 * Plain C over GLib, no GTK: the tests feed it bytes. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* the private modes kept, as bits */
enum {
  TERM_MODE_APP_CURSOR   = 1 << 0,     /* DECCKM (1): cursor keys send ESC O, not CSI */
  TERM_MODE_MOUSE_X10    = 1 << 1,     /* 9: presses reported */
  TERM_MODE_MOUSE_VT220  = 1 << 2,     /* 1000: presses and releases */
  TERM_MODE_MOUSE_HILITE = 1 << 3,     /* 1001 */
  TERM_MODE_MOUSE_BUTTON = 1 << 4,     /* 1002: and drags */
  TERM_MODE_MOUSE_ANY    = 1 << 5,     /* 1003: and every motion */
  TERM_MODE_MOUSE_SGR    = 1 << 6,     /* 1006: reports as CSI < b ; x ; y M/m */
  TERM_MODE_ALT_SCROLL   = 1 << 7,     /* 1007: on the alternate screen a wheel sends cursor keys (on by default) */
  TERM_MODE_ALTBUF       = 1 << 8,     /* 47 */
  TERM_MODE_ALTBUF_OPT   = 1 << 9,     /* 1047 */
  TERM_MODE_ALTBUF_SAVE  = 1 << 10,    /* 1049 */
};

#define TERM_MODES_DEFAULT TERM_MODE_ALT_SCROLL
#define TERM_MODES_MOUSE (TERM_MODE_MOUSE_X10 | TERM_MODE_MOUSE_VT220 | TERM_MODE_MOUSE_HILITE | \
                          TERM_MODE_MOUSE_BUTTON | TERM_MODE_MOUSE_ANY)

/* what a wheel does now */
typedef enum {
  TERM_SCROLL_HISTORY,       /* the terminal's own scrollback moves */
  TERM_SCROLL_MOUSE,         /* the program gets wheel button reports */
  TERM_SCROLL_KEYS,          /* the program gets cursor keys */
} TermScroll;

#define TERM_MODES_MAX_PARAMS 16

typedef struct {
  guint    modes;
  guint    saved;              /* XTSAVE's values ... */
  guint    saved_mask;         /* ... of these modes */
  gboolean alt_screen;         /* on the alternate screen (47, 1047 or 1049 set last) */

  /* the parser's state between two reads */
  guint8   state;
  char     marker;             /* a CSI's private marker: ? > < = */
  char     intermediate;       /* its intermediate byte, the last one */
  guint    n_params;
  guint    params[TERM_MODES_MAX_PARAMS];
  gboolean param_started;
} TermModes;

void       term_modes_init      (TermModes *self);
/* the program's output, in pieces as it comes: a sequence split across two pieces is read whole */
void       term_modes_feed      (TermModes *self, const guint8 *data, gsize length);

gboolean   term_modes_has       (const TermModes *self, guint mode);
gboolean   term_modes_tracks_mouse (const TermModes *self);
TermScroll term_modes_scroll    (const TermModes *self);

/* What one step of the wheel sends to the program, as VTE sends it: `down` towards the end of the text (the content
 * moving up), at the cell `column`, `row` (0-based, on the screen). Writes the bytes to `buf` (32 bytes are enough)
 * and returns their number: a button report while the mouse is tracked, a cursor key on the alternate screen, and 0
 * otherwise (the history scrolls) or when the cell is too far out for the report's encoding. */
gsize      term_modes_wheel     (const TermModes *self, gboolean down, long column, long row, char *buf, gsize size);
/* A click of the first button at the cell, as VTE reports it to a program that tracks the mouse: the press and,
 * except in X10 mode (9), the release. 0 when the mouse is not tracked. */
gsize      term_modes_click     (const TermModes *self, long column, long row, char *buf, gsize size);

G_END_DECLS
