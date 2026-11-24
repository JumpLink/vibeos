/* dictate-ru: Russian text to what tiny-voca-ru's voice model reads (dictate-voice.c): its sentences, and each one's
 * phone tokens with their stress. The text is normalised (numbers, dates, units, abbreviations in words, in the case
 * the words around them ask for), stressed (a dictionary, a model for the words it lacks and one for the homographs)
 * and turned into phones by rule.
 *
 * A port to C of tiny-voca-ru's text front end (src/tiny_voca_ru/frontend/ of github.com/yilbegan/tiny-voca-ru,
 * version 0.2.1), Copyright 2026 Evgeniy Chetvertkov, under the Apache License 2.0; tiny-voca-ru is based on
 * tiny-voca, Copyright 2026 Muhammed Nazeem, Apache License 2.0. Its NOTICE: "The stress dictionary (stress.txt.gz),
 * unknown-word accent model (accent.onnx), homograph tokenizer vocabulary (omograph_vocab.txt.gz), and INT8-quantized
 * homograph resolver (omograph.onnx, derived from nn/nn_omograph/tiny2.1) are derived from ruaccent
 * (github.com/Den4ikAI/ruaccent, huggingface.co/ruaccent/accentuator), MIT License." Those data files are
 * downloaded with the voice, not part of this source. Changed from the original: rewritten in C; a number of 10^15
 * or more, which the original cannot say, is read digit by digit.
 *
 * Not thread-safe across calls on one DictateRu for the models' sake: each is used under its lock. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _DictateRu DictateRu;

/* the front end on the data in `dir` (stress.txt.gz, accent.onnx, omograph.onnx, omograph_vocab.txt.gz), each loaded
 * when first needed; `homographs`: resolve them by their sentence (the original's "full"), else take a homograph's
 * most frequent reading ("lite") */
DictateRu *dictate_ru_new          (const char *dir, gboolean homographs);
void       dictate_ru_free         (DictateRu *self);

/* `text`'s sentences, as they are read one at a time (a full stop added when it ends without one) */
char     **dictate_ru_sentences    (const char *text);
/* a piece of text's tokens (the model's symbol ids, with <bos>, <sp> between words, <eos>) and their stress (0, 1)
 * appended to `ids` and `stress` (both gint64); FALSE, with nothing appended, when it has nothing to pronounce */
gboolean   dictate_ru_phonemize    (DictateRu *self, const char *text, GArray *ids, GArray *stress);
/* a sentence too long to be read at once, split at its clauses' punctuation, else in two halves by words; NULL when
 * it is one word */
char     **dictate_ru_split_clause (const char *text);
/* the model's symbols, in their ids' order (`n` of them): what its model.json must list */
const char *const *dictate_ru_symbols (guint *n);

/* the steps, for the tests: the normalised text, and it stressed ('+' before the stressed vowel) */
char      *dictate_ru_normalize    (const char *text);
char      *dictate_ru_accentuate   (DictateRu *self, const char *normalized);

G_END_DECLS
