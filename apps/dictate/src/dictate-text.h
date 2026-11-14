/* dictate-text: what the recogniser heard, as it goes into the document.
 *
 * Moonshine hears speech in lines that end at pauses, and capitalises each line's first letter, also where the line
 * goes on a sentence after a pause ("... the age of wisdom, It was the age of ..."); inside a line it sometimes starts
 * a sentence in lower case ("... best of times. it was ..."). dictate_text_tidy cases the text by its sentences
 * instead. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* `line` with a capital at the start of each sentence: its start if `sentence_start` (the document's start, or after
 * . ! ? … or a line break), and after . ! ? … and a space in it. Its first word goes in lower case where it goes on a
 * sentence, unless it is "I" or "I'…" or has capitals further on (an acronym, McDonald). A doubled comma is one. */
char *dictate_text_tidy (const char *line, gboolean sentence_start);

/* TRUE when text after `before` starts a sentence: `before` is empty or only white space, or its last character
 * other than white space is . ! ? …, or a line break follows that */
gboolean dictate_text_ends_sentence (const char *before);

G_END_DECLS
