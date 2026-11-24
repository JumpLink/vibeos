/* dictate-l10n: Vibe's spoken lines in the language it speaks, which may not be the screen's: the introduction after
 * a language is chosen in it, the passage read for the voice sample. A line is marked N_() and looked up here in that
 * language's catalogue of the app's messages (vibeos-dictate.mo), as gettext would for the screen's language. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* `msgid` in `language` (a code: "de"); `msgid` itself when that language has no catalogue or no translation of it.
 * The string is the catalogue's, kept for the life of the app */
const char *dictate_l10n (const char *language, const char *msgid);

/* the same for a message with a context (C_ / NC_ in the source): the context is part of what is looked up */
const char *dictate_l10n_c (const char *language, const char *context, const char *msgid);

/* the plural form of a message for the count `n`, by `language`'s rule (a catalogue's Plural-Forms, as the languages
 * Vibe speaks use them: n != 1; French n > 1; Russian's three forms). `msgid` and `msgid_plural` are the source's
 * ngettext() pair, and what is returned when that language has no translation: the English form for `n` */
const char *dictate_l10n_n (const char *language, const char *msgid, const char *msgid_plural, gulong n);

G_END_DECLS
