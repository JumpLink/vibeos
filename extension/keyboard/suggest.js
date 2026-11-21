// Word suggestions for the suggestion strip, as the AOSP keyboard makes them: the word being typed completed, or
// corrected by one slip (a letter missed, doubled, swapped or wrong), from the language's word list (words/, made by
// tools/latinime-words from LatinIME's own lists) ranked by how often each word is used; and the words this user
// types, learnt as they are typed (their frequency, and which word follows which). Between words, the next word: the
// ones this user types after the last, then the list's (counted in Tatoeba's sentences). And the auto-correction a
// space or a mark makes, as LatinIME's: the word typed, where it is no word, put right.
// No Shell imports, so that it can be checked outside the shell.

// a learnt word or pair counts this much more each time it is typed, up to its cap (on LatinIME's 0-255 scale)
const LEARNT_WORD = {step: 20, max: 80};
const LEARNT_PAIR = {step: 30, max: 120};
// the typed word itself, where it is a word, comes first (as typed before its accented or apostrophed forms: its before
// it's, schon before schön); a longer completion is a little less likely than a shorter one
const VALID_WORD = 1000;
const AS_TYPED = 100;
const SHORTCUT = 900;
// what a slip costs (on the 0-255 scale), by kind: a double letter typed once or thrice, two letters swapped, a
// neighbouring key hit, a letter left out or added, any other key hit
// (a double letter typed once is the commonest slip of all: helo is hello before help, as on Android)
const SLIP = {double: 5, swap: 25, near: 40, missing: 55, extra: 55, other: 75};
// keys this close (in key widths, centre to centre) are neighbours
const NEAR_KEYS = 1.3;
const PER_EXTRA_LETTER = 2;
// how many of the learnt words and pairs are kept (the least typed go first)
const MAX_LEARNT_WORDS = 5000;
const MAX_LEARNT_PAIRS = 10000;
// a word the list has after the previous one counts this much more, the first of them most, less by step for each next
const NEXT_WORD = {first: 60, step: 8};
// the list's next words at a sentence's start are under this form
const SENTENCE_START = '.';
// auto-correction: a correction the list spells differently (accents, an apostrophe) where the word is this frequent;
// one slip put right only in words of this many letters, scoring this much at least (frequency less the slip's
// cost) in a word of that length, perLetter less for each letter more down to least (one slip in a long word is
// more likely a slip), and this much more than the next likeliest (two as likely: no telling which was meant)
const AUTOCORRECT = {form: 40, letters: 3, score: 120, perLetter: 10, least: 40, margin: 12};

// The form words are matched in: lower case, no accents, no apostrophes (dont finds don't, cafe café)
export function fold(s) {
    return s.toLowerCase().normalize('NFD').replace(/[̀-ͯ'’]/g, '');
}

// lower bound: the first index in the sorted @keys not below @key
function lowerBound(keys, key) {
    let lo = 0, hi = keys.length;
    while (lo < hi) {
        const mid = (lo + hi) >> 1;
        if (keys[mid] < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

// A language's words, from a words/<locale>.txt file (tools/latinime-words): its lines in the order of their forms, so
// that nothing needs sorting. Read a slice at a time (add), so that the shell is never held up for long, then done().
export class Lexicon {
    constructor() {
        this.keys = [];
        this.words = [];
        this.freqs = [];
        this.freqOf = new Map();
        this.shortcuts = new Map();
        // the words that follow a word, by its form (SENTENCE_START: a sentence's first), most often first
        this.next = new Map();
        // the file's part being read: the words, the corrections, the next words (each after a line "=")
        this._part = 0;
        // the letters the language's words are made of, for the corrections; which keys are next to which
        // (neighboursOf), once the keyboard's layout is known
        this.letters = [];
        this.near = null;
    }

    // @lines[from, to): the file's lines
    add(lines, from, to) {
        for (let i = from; i < to; i++) {
            const line = lines[i];
            if (line === '=') {
                this._part++;
                continue;
            }
            const parts = line.split('\t');
            if (!parts[0] || parts.length < 2)
                continue;
            if (this._part === 2) {
                this.next.set(parts[0], parts.slice(1));
                continue;
            }
            if (this._part === 1) {
                this.shortcuts.set(fold(parts[0]), parts[1]);
                continue;
            }
            const freq = Number(parts[1]);
            this.keys.push(parts[2] ?? parts[0]);
            this.words.push(parts[0]);
            this.freqs.push(freq);
            if (!this.freqOf.has(parts[0]))
                this.freqOf.set(parts[0], freq);
        }
    }

    done() {
        // (a file out of order, as an older generator wrote it, is put in order)
        let sorted = true;
        for (let i = 1; i < this.keys.length && sorted; i++)
            sorted = this.keys[i - 1] <= this.keys[i];
        if (!sorted) {
            const order = this.keys.map((_k, i) => i).sort((a, b) =>
                this.keys[a] < this.keys[b] ? -1 : this.keys[a] > this.keys[b] ? 1 : this.freqs[b] - this.freqs[a]);
            [this.keys, this.words, this.freqs] = [this.keys, this.words, this.freqs].map(a => order.map(i => a[i]));
        }
        const letters = new Set();
        for (const key of this.keys) {
            for (const ch of key)
                letters.add(ch);
        }
        this.letters = [...letters].filter(ch => /\p{L}/u.test(ch));
        return this;
    }

    static fromText(text) {
        const lines = text.split('\n');
        const lexicon = new Lexicon();
        lexicon.add(lines, 0, lines.length);
        return lexicon.done();
    }

    // every word whose matching form is @key exactly: fn(word, freq)
    exact(key, fn) {
        for (let i = lowerBound(this.keys, key); i < this.keys.length && this.keys[i] === key; i++)
            fn(this.words[i], this.freqs[i]);
    }

    // the words that most often follow @previous (null: at a sentence's start), most often first
    followers(previous) {
        return this.next.get(previous === null ? SENTENCE_START : fold(previous)) ?? [];
    }

    // the words that start with @key (it not included), most frequent first, at most @n
    completions(key, n) {
        const best = [];
        for (let i = lowerBound(this.keys, key); i < this.keys.length && this.keys[i].startsWith(key); i++) {
            if (this.keys[i] === key)
                continue;
            const f = this.freqs[i] - PER_EXTRA_LETTER * (this.keys[i].length - key.length);
            if (best.length < n || f > best[best.length - 1].f) {
                best.push({word: this.words[i], f});
                best.sort((a, b) => b.f - a.f);
                if (best.length > n)
                    best.pop();
            }
        }
        return best;
    }
}

// One slip away from @key: each form the typed one would be with a letter it has too many taken out, two of its
// letters swapped, one changed or one it lacks put in, and what that slip costs (SLIP). @near: a key's neighbours.
function slips(key, letters, near) {
    const out = new Map();
    const add = (form, cost) => {
        if (form !== key && !(out.get(form) <= cost))
            out.set(form, cost);
    };
    const chars = [...key];
    const n = chars.length;
    const join = (...parts) => parts.flat().join('');
    for (let i = 0; i < n; i++) {
        const doubled = chars[i] === chars[i - 1] || chars[i] === chars[i + 1];
        add(join(chars.slice(0, i), chars.slice(i + 1)), doubled ? SLIP.double : SLIP.extra);
    }
    for (let i = 0; i < n - 1; i++)
        add(join(chars.slice(0, i), chars[i + 1], chars[i], chars.slice(i + 2)), SLIP.swap);
    for (let i = 0; i < n; i++) {
        const around = near?.get(chars[i]);
        for (const ch of letters) {
            if (ch !== chars[i])
                add(join(chars.slice(0, i), ch, chars.slice(i + 1)), around?.has(ch) ? SLIP.near : SLIP.other);
        }
    }
    for (let i = 0; i <= n; i++) {
        for (const ch of letters)
            add(join(chars.slice(0, i), ch, chars.slice(i)), ch === chars[i - 1] || ch === chars[i] ? SLIP.double : SLIP.missing);
    }
    return out;
}

// Which letters' keys are next to which, from a page of the layout: rows of keys with x, w (in % of the keyboard's
// width) and l (the letter)
export function neighboursOf(rows) {
    const keys = [];
    rows.forEach((row, r) => {
        for (const k of row) {
            if (k.l && [...k.l].length === 1 && /\p{L}/u.test(k.l))
                keys.push({ch: fold(k.l), cx: k.x + k.w / 2, w: k.w, r});
        }
    });
    const near = new Map();
    for (const a of keys) {
        const set = new Set();
        for (const b of keys) {
            if (b !== a && Math.abs(a.r - b.r) <= 1 && Math.abs(a.cx - b.cx) <= NEAR_KEYS * a.w)
                set.add(b.ch);
        }
        near.set(a.ch, set);
    }
    return near;
}

// @word in the capitals of @typed: Capitalised, or ALL CAPITALS (a word the list has capitalised stays so)
function inCase(word, typed, capitalise) {
    const first = [...typed][0];
    if ([...typed].length > 1 && typed === typed.toUpperCase() && typed !== typed.toLowerCase())
        return word.toUpperCase();
    if ((first && first !== first.toLowerCase()) || (!first && capitalise))
        return word.charAt(0).toUpperCase() + word.slice(1);
    return word;
}

// What this user types, per language: {words: {word: count}, pairs: {"previous\u0001word": count}}
export class Learnt {
    constructor(data) {
        this.words = new Map(Object.entries(data?.words ?? {}));
        this.pairs = new Map(Object.entries(data?.pairs ?? {}));
    }

    add(previous, word) {
        if (!word || !/\p{L}/u.test(word))
            return;
        this.words.set(word, (this.words.get(word) ?? 0) + 1);
        if (previous)
            this.pairs.set(`${fold(previous)}\u0001${word}`, (this.pairs.get(`${fold(previous)}\u0001${word}`) ?? 0) + 1);
        prune(this.words, MAX_LEARNT_WORDS);
        prune(this.pairs, MAX_LEARNT_PAIRS);
    }

    wordBoost(word) {
        return Math.min(LEARNT_WORD.max, (this.words.get(word) ?? 0) * LEARNT_WORD.step);
    }

    pairBoost(previous, word) {
        if (!previous)
            return 0;
        return Math.min(LEARNT_PAIR.max, (this.pairs.get(`${fold(previous)}\u0001${word}`) ?? 0) * LEARNT_PAIR.step);
    }

    // the words typed after @previous, most often first
    followers(previous, n) {
        const prefix = `${fold(previous)}\u0001`;
        const out = [];
        for (const [pair, count] of this.pairs) {
            if (pair.startsWith(prefix))
                out.push({word: pair.slice(prefix.length), f: count});
        }
        return out.sort((a, b) => b.f - a.f).slice(0, n);
    }

    toJSON() {
        return {words: Object.fromEntries(this.words), pairs: Object.fromEntries(this.pairs)};
    }
}

function prune(map, max) {
    if (map.size <= max)
        return;
    const sorted = [...map].sort((a, b) => a[1] - b[1]);
    for (const [k] of sorted.slice(0, map.size - Math.floor(max * 0.9)))
        map.delete(k);
}

// How much more likely @word is for following @previous in the list (null previous: at a sentence's start, @start)
function nextBoost(lexicon, previous, start, word) {
    if (!lexicon || (previous === null && !start))
        return 0;
    const i = lexicon.followers(previous).indexOf(word);
    return i < 0 ? 0 : Math.max(0, NEXT_WORD.first - NEXT_WORD.step * i);
}

// The suggestions for @typed (the word before the cursor, '' between words) after @previous (the word before it, or
// null), best first, at most @n. @capitalise: the field wants a capital here (the start of a sentence). @next: the
// next word's suggestions are on (between words, and the list's next words count while typing); @start: the cursor is
// at a sentence's start (no previous word there, but the list knows how sentences start).
export function suggest(lexicon, learnt, typed, previous, n = 3, capitalise = false, {next = true, start = false} = {}) {
    const scores = new Map();
    const offer = (word, score) => {
        if (scores.has(word) && scores.get(word) >= score)
            return;
        scores.set(word, score);
    };
    const boost = word => (learnt?.wordBoost(word) ?? 0) + (learnt?.pairBoost(previous, word) ?? 0) +
        (next ? nextBoost(lexicon, previous, start, word) : 0);

    if (!typed) {
        // between words: the words this user has typed after the previous one, then the ones the list has after it
        if (!next || (previous === null && !start))
            return [];
        const words = [];
        if (previous && learnt)
            words.push(...learnt.followers(previous, n).map(s => s.word));
        words.push(...lexicon?.followers(previous) ?? []);
        return [...new Set(words.map(w => inCase(w, '', capitalise)))].slice(0, n);
    }

    const key = fold(typed);
    if (lexicon) {
        const asTyped = typed.toLowerCase();
        lexicon.exact(key, (word, f) =>
            offer(word, VALID_WORD + f + boost(word) + (word.toLowerCase() === asTyped ? AS_TYPED : 0)));
        const to = lexicon.shortcuts.get(key);
        if (to)
            offer(to, SHORTCUT + boost(to));
        for (const c of lexicon.completions(key, n + 2))
            offer(c.word, c.f + boost(c.word));
        if ([...key].length >= 2) {
            for (const [slip, cost] of slips(key, lexicon.letters, lexicon.near))
                lexicon.exact(slip, (word, f) => offer(word, f - cost + boost(word)));
        }
    }
    // the words this user types that the list has not
    if (learnt) {
        for (const [word] of learnt.words) {
            const k = fold(word);
            if (k === key)
                offer(word, VALID_WORD + boost(word));
            else if (k.startsWith(key) && !lexicon?.freqOf.has(word) && learnt.words.get(word) >= 2)
                offer(word, boost(word) * 2);
        }
    }

    const ranked = [...scores].sort((a, b) => b[1] - a[1]).map(([word]) => inCase(word, typed, capitalise));
    // the same word twice (once as typed, once from the list) is shown once
    return [...new Set(ranked)].slice(0, n);
}

// What a space or a mark typed after @typed makes of it (null: it stays as typed), as LatinIME's auto-correction: a
// correction the list has for it (dont: don't), the list's word with its accents or apostrophe (fur: für, in German),
// or one slip put right (teh: the) where @typed is no word, neither in the list nor typed by this user, and the
// correction is a common one, clearly likelier than any other. Not a word with capitals inside or at its start where
// none was asked for (a name): only the list's own corrections there. The first letter is taken as typed, but for two
// swapped there (hte: the).
export function autocorrection(lexicon, learnt, typed, previous, capitalise = false, {next = true, start = false} = {}) {
    if (!lexicon || !typed)
        return null;
    const chars = [...typed];
    if (chars.slice(1).some(ch => ch !== ch.toLowerCase()))
        return null;
    const key = fold(typed);
    const lower = typed.toLowerCase();
    let valid = false;
    let form = null;
    // typed as the list has it, or in capitals where the list has it in lower case (The, THE); the list's capitals
    // otherwise (i: I, monday: Monday)
    lexicon.exact(key, (word, f) => {
        if (word === typed || (word === lower && word === word.toLowerCase()))
            valid = true;
        else if (!form || f > form.f)
            form = {word, f};
    });
    if (valid || learnt?.words.has(typed) || learnt?.words.has(lower))
        return null;
    const to = lexicon.shortcuts.get(key);
    if (to)
        return inCase(to, typed, capitalise);
    if (form && form.f >= AUTOCORRECT.form)
        return inCase(form.word, typed, capitalise);
    if ((chars[0] !== chars[0].toLowerCase() && !capitalise) || [...key].length < AUTOCORRECT.letters)
        return null;
    const k = [...key];
    const scored = new Map();
    for (const [slip, cost] of slips(key, lexicon.letters, lexicon.near)) {
        const s = [...slip];
        if (s[0] !== k[0] && !(s[0] === k[1] && s[1] === k[0]))
            continue;
        lexicon.exact(slip, (word, f) => {
            const score = f - cost + (learnt?.wordBoost(word) ?? 0) + (learnt?.pairBoost(previous, word) ?? 0) +
                (next ? nextBoost(lexicon, previous, start, word) : 0);
            if (!(scored.get(word) >= score))
                scored.set(word, score);
        });
    }
    const [best, second] = [...scored].sort((a, b) => b[1] - a[1]);
    const least = Math.max(AUTOCORRECT.least, AUTOCORRECT.score - AUTOCORRECT.perLetter * (k.length - AUTOCORRECT.letters));
    if (!best || best[1] < least || (second && best[1] - second[1] < AUTOCORRECT.margin))
        return null;
    return inCase(best[0], typed, capitalise);
}

// Where the suggestions go in the strip's three places, [left, middle, right]: the best in the middle, the next on its
// left, the third on its right (as LatinIME places them)
export function placeInStrip(suggestions) {
    return [suggestions[1] ?? null, suggestions[0] ?? null, suggestions[2] ?? null];
}

// Whether @before (the field's text before the word being typed) ends where a sentence starts: nothing, or only marks
// and spaces, or a sentence's end and a space
export function atSentenceStart(before) {
    return /^[^\p{L}\p{N}]*$/u.test(before) || /[.!?\u2026]["'\u201d\u00bb)]*\s+$/u.test(before);
}

// The word being typed and the one before it, from the field's text before the cursor: [typed, previous]. Only
// letters (and apostrophes inside a word) count; a sentence's end leaves no previous word.
export function wordsBefore(before) {
    const m = before.match(/(?:^|[^\p{L}\p{M}'’])((?:\p{L}|\p{M}|['’](?=\p{L}))*)$/u);
    const typed = m ? m[1] : '';
    const rest = before.slice(0, before.length - typed.length);
    const p = rest.match(/(\p{L}(?:\p{L}|\p{M}|['’](?=\p{L}))*)[ \t]+$/u);
    return [typed, p ? p[1] : null];
}
