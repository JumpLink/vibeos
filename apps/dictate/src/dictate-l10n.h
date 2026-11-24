/* dictate-l10n: Vibe's spoken lines in the language it speaks, which may not be the screen's: the introduction after
 * a language is chosen in it, the passage read for the voice sample. A line is marked N_() and looked up here in that
 * language's catalogue of the app's messages (vibeos-dictate.mo), as gettext would for the screen's language. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* `msgid` in `language` (a code: "de"); `msgid` itself when that language has no catalogue or no translation of it.
 * The string is the catalogue's, kept for the life of the app */
const char *dictate_l10n (const char *language, const char *msgid);

G_END_DECLS
