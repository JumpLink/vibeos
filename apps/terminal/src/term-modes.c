/* TermModes: the private modes the program asked for, read from its output (term-modes.h).
 *
 * The parser is ECMA-48's, cut down to what can hide or carry a mode: escape sequences, control sequences (CSI) and
 * the control strings (OSC, DCS, APC, PM, SOS), whose payload is skipped so that text in a window title is never
 * taken for a sequence. It follows VTE's parser where that matters: CAN and SUB cancel a sequence, an ESC anywhere
 * starts a new one (in a string too, which it ends), and a private marker after a parameter spoils the sequence.
 * The 8-bit C1 controls (CSI as one code) are not read: in UTF-8 programs send the 7-bit ones. */
#include "term-modes.h"

#include <stdio.h>
#include <string.h>

enum {
  GROUND,
  ESCAPE,
  ESCAPE_INTERMEDIATE,  /* ESC ( B and the like: up to their final byte */
  CSI_PARAM,
  CSI_IGNORE,           /* a malformed control sequence: up to its final byte */
  STRING,               /* OSC, DCS, APC, PM, SOS: up to BEL or ST */
  STRING_ESCAPE,        /* an ESC in a string: ST if a backslash follows */
};

#define CAN 0x18
#define SUB 0x1a
#define ESC 0x1b
#define BEL 0x07

/* the most a parameter counts to (VTE's limit) */
#define PARAM_MAX 65535

static const guint ALT_SCREEN = TERM_MODE_ALTBUF | TERM_MODE_ALTBUF_OPT | TERM_MODE_ALTBUF_SAVE;

static guint
mode_bit (guint param)
{
  switch (param) {
  case 1:    return TERM_MODE_APP_CURSOR;
  case 9:    return TERM_MODE_MOUSE_X10;
  case 47:   return TERM_MODE_ALTBUF;
  case 1000: return TERM_MODE_MOUSE_VT220;
  case 1001: return TERM_MODE_MOUSE_HILITE;
  case 1002: return TERM_MODE_MOUSE_BUTTON;
  case 1003: return TERM_MODE_MOUSE_ANY;
  case 1006: return TERM_MODE_MOUSE_SGR;
  case 1007: return TERM_MODE_ALT_SCROLL;
  case 1047: return TERM_MODE_ALTBUF_OPT;
  case 1049: return TERM_MODE_ALTBUF_SAVE;
  default:   return 0;
  }
}

void
term_modes_init (TermModes *self)
{
  memset (self, 0, sizeof *self);
  self->modes = TERM_MODES_DEFAULT;
  self->state = GROUND;
}

static void
set_mode (TermModes *self, guint bit, gboolean on)
{
  if (on)
    self->modes |= bit;
  else
    self->modes &= ~bit;
  /* any of the three switches the screen, each way (VTE's set_mode_private) */
  if (bit & ALT_SCREEN)
    self->alt_screen = on;
}

/* RIS (hard, back to the normal screen) and DECSTR (soft: the screen stays): the modes to their defaults, the saved
 * ones forgotten (VTE's reset) */
static void
reset (TermModes *self, gboolean hard)
{
  self->modes = TERM_MODES_DEFAULT;
  self->saved = self->saved_mask = 0;
  if (hard)
    self->alt_screen = FALSE;
}

static void
csi_start (TermModes *self)
{
  self->state = CSI_PARAM;
  self->marker = 0;
  self->intermediate = 0;
  self->n_params = 0;
  self->params[0] = 0;
  self->param_started = FALSE;
}

static void
csi_dispatch (TermModes *self, char final)
{
  guint count = self->param_started || self->n_params > 0 ? self->n_params + 1 : 0;

  if (self->marker == 0 && self->intermediate == '!' && final == 'p') {
    reset (self, FALSE);        /* DECSTR */
    return;
  }
  if (self->marker != '?' || self->intermediate != 0)
    return;
  for (guint i = 0; i < count; i++) {
    guint bit = mode_bit (self->params[i]);
    if (!bit)
      continue;
    switch (final) {
    case 'h':                   /* DECSET */
      set_mode (self, bit, TRUE);
      break;
    case 'l':                   /* DECRST */
      set_mode (self, bit, FALSE);
      break;
    case 's':                   /* XTSAVE */
      self->saved = (self->saved & ~bit) | (self->modes & bit);
      self->saved_mask |= bit;
      break;
    case 'r':                   /* XTRESTORE */
      if (self->saved_mask & bit)
        set_mode (self, bit, (self->saved & bit) != 0);
      break;
    default:
      break;
    }
  }
}

static void
escape (TermModes *self, guint8 c)
{
  switch (c) {
  case '[':
    csi_start (self);
    return;
  case ']': case 'P': case 'X': case '^': case '_':
    self->state = STRING;
    return;
  case 'c':
    reset (self, TRUE);         /* RIS */
    self->state = GROUND;
    return;
  case ESC:
    return;
  case CAN: case SUB:
    self->state = GROUND;
    return;
  default:
    break;
  }
  if (c < 0x20)                 /* a control in an escape sequence is carried out, and the sequence goes on */
    return;
  self->state = c <= 0x2f ? ESCAPE_INTERMEDIATE : GROUND;
}

static void
csi_param (TermModes *self, guint8 c)
{
  if (c >= '0' && c <= '9') {
    if (self->intermediate) {
      self->state = CSI_IGNORE;
      return;
    }
    guint *p = &self->params[self->n_params];
    *p = MIN (*p * 10 + (c - '0'), PARAM_MAX);
    self->param_started = TRUE;
  } else if (c == ';' || c == ':') {
    if (self->intermediate) {
      self->state = CSI_IGNORE;
      return;
    }
    if (self->n_params + 1 < TERM_MODES_MAX_PARAMS)
      self->params[++self->n_params] = 0;
    else
      self->params[self->n_params] = 0;
    self->param_started = FALSE;
  } else if (c >= 0x3c && c <= 0x3f) {
    /* a private marker comes first or not at all */
    if (self->marker || self->param_started || self->n_params || self->intermediate)
      self->state = CSI_IGNORE;
    else
      self->marker = c;
  } else if (c >= 0x20 && c <= 0x2f) {
    self->intermediate = c;
  } else if (c >= 0x40 && c <= 0x7e) {
    csi_dispatch (self, c);
    self->state = GROUND;
  } else if (c >= 0x80) {
    self->state = CSI_IGNORE;
  }
  /* other controls are carried out without ending the sequence; DEL is nothing */
}

void
term_modes_feed (TermModes *self, const guint8 *data, gsize length)
{
  for (gsize i = 0; i < length; i++) {
    guint8 c = data[i];

    /* CAN and SUB end any sequence; ESC starts a new one (in a string it may be the start of ST) */
    if (c == CAN || c == SUB) {
      self->state = GROUND;
      continue;
    }

    switch (self->state) {
    case GROUND:
      if (c == ESC)
        self->state = ESCAPE;
      break;
    case ESCAPE:
      escape (self, c);
      break;
    case ESCAPE_INTERMEDIATE:
      if (c == ESC)
        self->state = ESCAPE;
      else if (c >= 0x30 && c <= 0x7e)
        self->state = GROUND;
      break;
    case CSI_PARAM:
      if (c == ESC)
        self->state = ESCAPE;
      else
        csi_param (self, c);
      break;
    case CSI_IGNORE:
      if (c == ESC)
        self->state = ESCAPE;
      else if (c >= 0x40 && c <= 0x7e)
        self->state = GROUND;
      break;
    case STRING:
      if (c == BEL)
        self->state = GROUND;
      else if (c == ESC)
        self->state = STRING_ESCAPE;
      break;
    case STRING_ESCAPE:
      if (c == '\\') {
        self->state = GROUND;
      } else {
        /* the ESC began another sequence, and ended the string */
        self->state = ESCAPE;
        escape (self, c);
      }
      break;
    default:
      self->state = GROUND;
      break;
    }
  }
}

gboolean
term_modes_has (const TermModes *self, guint mode)
{
  return (self->modes & mode) != 0;
}

gboolean
term_modes_tracks_mouse (const TermModes *self)
{
  return (self->modes & TERM_MODES_MOUSE) != 0;
}

TermScroll
term_modes_scroll (const TermModes *self)
{
  if (term_modes_tracks_mouse (self))
    return TERM_SCROLL_MOUSE;
  if (self->alt_screen && term_modes_has (self, TERM_MODE_ALT_SCROLL))
    return TERM_SCROLL_KEYS;
  return TERM_SCROLL_HISTORY;
}

/* a mouse report of button code `cb` at the cell, in the encoding the program chose (VTE's feed_mouse_event): SGR
 * (1006), or the original three bytes, which carry no cell past 223 (nothing is sent then) */
static gsize
mouse_report (const TermModes *self, int cb, gboolean release, long column, long row, char *buf, gsize size)
{
  long x = column + 1, y = row + 1;
  int n;

  if (x < 1 || y < 1)
    return 0;
  if (term_modes_has (self, TERM_MODE_MOUSE_SGR)) {
    n = g_snprintf (buf, size, "\033[<%d;%ld;%ld%c", cb, x, y, release ? 'm' : 'M');
    return n > 0 && (gsize) n < size ? (gsize) n : 0;
  }
  if (release)
    cb = 3;
  if (cb > 223 || x > 223 || y > 223 || size < 6)
    return 0;
  buf[0] = '\033';
  buf[1] = '[';
  buf[2] = 'M';
  buf[3] = (char) (32 + cb);
  buf[4] = (char) (32 + x);
  buf[5] = (char) (32 + y);
  return 6;
}

gsize
term_modes_wheel (const TermModes *self, gboolean down, long column, long row, char *buf, gsize size)
{
  switch (term_modes_scroll (self)) {
  case TERM_SCROLL_MOUSE:
    /* buttons 4 (up) and 5 (down): 64 and 65 */
    return mouse_report (self, down ? 65 : 64, FALSE, column, row, buf, size);
  case TERM_SCROLL_KEYS: {
    const char *key = term_modes_has (self, TERM_MODE_APP_CURSOR) ? (down ? "\033OB" : "\033OA")
                                                                 : (down ? "\033[B" : "\033[A");
    gsize n = strlen (key);
    if (n > size)
      return 0;
    memcpy (buf, key, n);
    return n;
  }
  case TERM_SCROLL_HISTORY:
  default:
    return 0;
  }
}

gsize
term_modes_click (const TermModes *self, long column, long row, char *buf, gsize size)
{
  if (!term_modes_tracks_mouse (self))
    return 0;
  gsize n = mouse_report (self, 0, FALSE, column, row, buf, size);
  if (!n)
    return 0;
  /* X10 alone reports the press only */
  if (self->modes & (TERM_MODES_MOUSE & ~TERM_MODE_MOUSE_X10))
    n += mouse_report (self, 0, TRUE, column, row, buf + n, size - n);
  return n;
}
