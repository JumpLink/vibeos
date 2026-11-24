"""The reference for tests/ru-golden.c: tiny-voca-ru's own front end (Python, version 0.2.1) on each line of
corpus.txt, one line of golden.tsv each: the text, it normalised, it stressed (full: homographs by their sentence;
lite: without), then each utterance it is spoken in, as PIECE\\x1fIDS\\x1fSTRESS (the ids and stress comma-separated).

    python3 -I make-golden.py SITE OMOGRAPH_ONNX corpus.txt golden.tsv

SITE is where the tiny_voca_ru package is (with onnxruntime and numpy); OMOGRAPH_ONNX its homograph model. The
package is someone else's code: run it isolated (-I), offline, as a user that can write only the output."""
import sys

site, omograph, corpus, out_path = sys.argv[1:5]
sys.path.insert(0, site)

from tiny_voca_ru.frontend import (FullStress, NothingToPronounce, ensure_terminal_punctuation, normalize,
                                   phonemize, split_clause, split_sentences)
from tiny_voca_ru.frontend.stress import LiteStress

full = FullStress(model_path=omograph)
lite = LiteStress()
LIMIT = 512 - 12        # the synthesizer's: max_tokens less 12


def utterances(text):
    found = []

    def read(piece):
        try:
            phones = phonemize(piece, stress=full)
        except NothingToPronounce:
            return
        if len(phones) <= LIMIT:
            found.append("\x1f".join([piece, ",".join(map(str, phones.ids)), ",".join(map(str, phones.stress))]))
            return
        for part in split_clause(piece) or []:
            read(part)

    for sentence in split_sentences(ensure_terminal_punctuation(text)):
        read(sentence)
    return found


with open(corpus, encoding="utf-8") as f, open(out_path, "w", encoding="utf-8") as out:
    for line in f:
        text = line.rstrip("\n")
        if text:
            normalized = normalize(text)
            row = [text, normalized, full.accentuate(normalized), lite.accentuate(normalized)] + utterances(text)
            out.write("\t".join(row) + "\n")
