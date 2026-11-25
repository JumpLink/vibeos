/* TermSession: the shell the window runs, on a pseudo-terminal the app owns rather than VTE.
 *
 * VTE can run the child itself, but then the bytes between the two are VTE's alone. Here the app opens the pty
 * (VtePty), starts the user's shell on it and does the reading and writing: what the shell writes is read by the app,
 * passed through TermModes (term-modes.h) and fed to the terminal (vte_terminal_feed); what the terminal sends (keys,
 * pastes, its answers to the program's queries: VteTerminal::commit, which VTE emits whether or not it has a pty of
 * its own) is written to the pty. So the app knows the private modes the program set, and can write to the program
 * itself (term_session_write): the wheel a scroll stands for, the extra keys.
 *
 * Reading keeps pace with drawing, as VTE's own does: up to a budget of bytes, then nothing until the next frame of
 * the terminal (or 100 ms when it has no frames, unmapped), so a program that writes without end (yes) cannot pile up
 * more than one budget in VTE's queue. The budget doubles while frames keep coming in time and halves when VTE takes
 * longer over what it was given.
 *
 * Signals: "exited" (int status): the shell is gone (the status as waitpid () gave it). */
#pragma once

#include <vte/vte.h>

#include "term-modes.h"

G_BEGIN_DECLS

#define TERM_TYPE_SESSION (term_session_get_type ())
G_DECLARE_FINAL_TYPE (TermSession, term_session, TERM, SESSION, GObject)

/* a session showing in `terminal`, not yet started */
TermSession     *term_session_new        (VteTerminal *terminal);
/* Starts the user's shell (their login shell from the password database, else $SHELL, else /bin/sh) in `directory`
 * (NULL: the home directory). FALSE with `error` set when the pty could not be opened; a shell that could not be run
 * says why in the terminal. */
gboolean         term_session_start      (TermSession *self, const char *directory, GError **error);
/* the grid's size, for the program (TIOCSWINSZ, and with it SIGWINCH) */
void             term_session_set_size   (TermSession *self, long rows, long columns);
/* bytes to the program, after whatever is still waiting to be written */
void             term_session_write      (TermSession *self, const char *data, gsize length);
const TermModes *term_session_get_modes  (TermSession *self);
/* what the terminal sends goes through `filter` first: it writes something else itself and returns TRUE, or FALSE
 * to have it written as it is (the extra keys' Ctrl and Alt on a typed character: term_keys_filter ()) */
typedef gboolean (*TermSessionFilter) (const char *text, gsize length, gpointer data);
void             term_session_set_filter (TermSession *self, TermSessionFilter filter, gpointer data);

G_END_DECLS
