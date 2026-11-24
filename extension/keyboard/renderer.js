// The keyboard drawn with Cairo in screen pixels, as the AOSP keyboard draws itself: each key a rounded rectangle
// (the action key a pill) grown by the key background's padding, its label centred on the reference letter's
// height, a bold hint letter in the top right corner, icons at their own size in the middle. Above the keys the
// suggestion strip, below them on the tablet the keyboard's own navigation bar.
//
// One canvas holds every key of a page and is drawn again only when the page, the size or the colours change; a key
// under the finger is drawn on its own small canvas over it.
import GObject from 'gi://GObject';
import Pango from 'gi://Pango';
import PangoCairo from 'gi://PangoCairo';

import {Canvas} from '../icons.js';
import {setSource} from '../palette.js';
import {drawnRect, navBarParts, stripDividers, stripSlots} from './geometry.js';
import {paintKeyIcon} from './icons.js';

const FAMILY = 'VibeOS Roboto';
// Roboto's vertical metrics, per px of the size: the ascent and descent its lines are set with, and the top of its
// glyphs' bounding box
const ASCENT = 1900 / 2048;
const DESCENT = 500 / 2048;
const BOX_TOP = 2163 / 2048;
// a label that would fill more of its key's width than this is squeezed (or made smaller) to fit
const MAX_LABEL_RATIO = 0.9;
// an off-centre label starts this many reference letters left of the centre, a hint label this many right of it
const LABEL_OFF_CENTRE = -0.8;
const HINT_LABEL_OFF_CENTRE = 3.0;
// the language on the space bar, at rest
const SPACEBAR_LANGUAGE_ALPHA = 128 / 255;
// the strip's float and split keys, while off
const TOOL_KEY_ALPHA = 128 / 255;
// the hint of a key that shows its shifted letter, white over the key (this theme leaves it as the dark theme has it)
const SHIFTED_HINT = [[255, 255, 255], 0.5];
const SHIFTED_HINT_ACTIVE = [[255, 255, 255], 0.8];
// a suggestion too wide for its place is squeezed to this much of its width at most, then cut off
const MIN_SUGGESTION_SCALE = 0.7;
// Roboto's capital height, per px of the size: a suggestion's letters centre on it
const CAP_HEIGHT = 1456 / 2048;

const trunc = Math.trunc;

let context = null;
const texts = new Map();

// A laid-out line of text: its advance width, its ink's top and height, its baseline from the layout's top
function text(str, size, bold) {
    const key = `${bold ? 'b' : 'r'}${size}\u0000${str}`;
    let t = texts.get(key);
    if (!t) {
        context ??= PangoCairo.FontMap.get_default().create_context();
        const layout = Pango.Layout.new(context);
        layout.set_font_description(Pango.FontDescription.from_string(`${FAMILY}${bold ? ' Bold' : ''} ${size}px`));
        layout.set_text(str, -1);
        const [ink, logical] = layout.get_extents();
        t = {
            layout, width: logical.width / Pango.SCALE,
            inkY: ink.y / Pango.SCALE, inkHeight: ink.height / Pango.SCALE,
            baseline: layout.get_baseline() / Pango.SCALE,
        };
        if (texts.size > 4000)
            texts.clear();
        texts.set(key, t);
    }
    return t;
}

// A key label's width at a text size
export function labelWidth(str, size) {
    return text(str, size, false).width;
}

// Text with its baseline at y, centred on x or starting there; @scaleX squeezes it about that point
function drawText(cr, str, size, bold, x, y, {centre = true, colour, alpha = 1, scaleX = 1}) {
    const t = text(str, size, bold);
    cr.save();
    cr.translate(centre ? x - t.width * scaleX / 2 : x, y - t.baseline);
    if (scaleX !== 1)
        cr.scale(scaleX, 1);
    setSource(cr, colour, alpha);
    cr.moveTo(0, 0);
    PangoCairo.show_layout(cr, t.layout);
    cr.restore();
}

function colourAndAlpha(c) {
    return Array.isArray(c[0]) ? c : [c, 1];
}

function roundRect(cr, x, y, w, h, r) {
    r = Math.max(0, Math.min(r, w / 2, h / 2));
    cr.newSubPath();
    cr.arc(x + w - r, y + r, r, -Math.PI / 2, 0);
    cr.arc(x + w - r, y + h - r, r, 0, Math.PI / 2);
    cr.arc(x + r, y + h - r, r, Math.PI / 2, Math.PI);
    cr.arc(x + r, y + r, r, Math.PI, 3 * Math.PI / 2);
    cr.closePath();
}

const has = (key, flag) => key.f?.includes(flag) ?? false;

// the size of a key's label: its flags say which, else a single letter is a letter and anything longer a label
function labelSize(m, key) {
    switch (key.f?.find(f => f.startsWith('size:'))) {
    case 'size:letter':
        return m.letterSize;
    case 'size:largeLetter':
        return m.largeLetterSize;
    case 'size:label':
        return m.labelSize;
    case 'size:hintLabel':
        return m.hintLabelSize;
    default:
        return [...key.l].length === 1 ? m.letterSize : m.labelSize;
    }
}

function background(m, key, c, pressed) {
    switch (key.b ?? 'normal') {
    case 'functional':
    case 'stickyOff':
        return [pressed ? c.functionalPressed : c.functional, m.corner];
    case 'stickyOn':
        return [c.functionalPressed, m.corner];
    case 'spacebar':
        return [pressed ? c.functionalPressed : c.letter, m.corner];
    case 'action':
        return [pressed ? c.actionPressed : c.action, m.actionCorner];
    case 'empty':
        return [pressed ? c.letterPressed : null, m.corner];
    default:
        return [pressed ? c.letterPressed : c.letter, m.corner];
    }
}

// One key (@k, placed by geometry.placeKeys) at its place in @cr's keyboard space (screen pixels).
// @state: {enterIcon: the action key's icon role, language: the space bar's language name or null}
export function paintKey(cr, m, k, c, pressed, state = {}) {
    const key = k.key;
    const [fill, radius] = background(m, key, c, pressed);
    if (fill) {
        const r = drawnRect(m, k);
        roundRect(cr, r.x, r.y, r.w, r.h, radius);
        setSource(cr, fill);
        cr.fill();
    }

    cr.save();
    cr.translate(k.x, k.y);
    const w = k.w, h = k.h;
    const cx = w * 0.5, cy = h * 0.5;
    let labelX = cx, labelBaseline = cy;
    if (key.l !== undefined) {
        let size = labelSize(m, key);
        const ref = text('M', size, false);
        labelBaseline = cy + ref.inkHeight / 2;
        const offCentre = has(key, 'offCenter');
        if (offCentre)
            labelX = cx + LABEL_OFF_CENTRE * ref.width;
        let scaleX = 1;
        if (has(key, 'autoXScale')) {
            const ratio = Math.min(1, w * MAX_LABEL_RATIO / text(key.l, size, false).width);
            if (has(key, 'autoYScale'))
                size *= ratio;
            else
                scaleX = ratio;
        }
        const colour = has(key, 'functionalText') || has(key, 'shiftedActive') ? c.functionalText : c.text;
        drawText(cr, key.l, size, false, labelX, labelBaseline, {centre: !offCentre, colour, scaleX});
    }

    if (key.h) {
        let size, x, y, centre = true, colour;
        if (has(key, 'hintLabel')) {
            // just right of the label, on its baseline (the theme aligns hint labels to the bottom)
            size = m.hintLabelSize;
            x = labelX + HINT_LABEL_OFF_CENTRE * text('M', size, true).width;
            y = labelBaseline;
            centre = false;
            colour = c.hint;
        } else if (has(key, 'shiftedHint')) {
            size = m.shiftedHintSize;
            x = w - m.shiftedHintPadding - text('M', size, true).width / 2;
            y = BOX_TOP * size;
            colour = has(key, 'shiftedActive') ? SHIFTED_HINT_ACTIVE : SHIFTED_HINT;
        } else {
            // the top right corner, centred on the wider of a digit and the hint
            size = m.hintLetterSize;
            x = w - m.hintPadding - Math.max(text('8', size, true).width, text(key.h, size, true).width) / 2;
            y = ASCENT * size + m.hintPaddingVertical;
            colour = c.hint;
        }
        const [rgb, alpha] = colourAndAlpha(colour);
        drawText(cr, key.h, size, true, x, y, {centre, colour: rgb, alpha});
    }

    if (key.c === 'space' && state.language) {
        const size = m.spacebarTextSize;
        const baseline = trunc(h / 2) + (ASCENT + DESCENT) * size / 2;
        drawText(cr, state.language, size, false, trunc(w / 2), baseline - DESCENT * size,
            {colour: c.hint, alpha: SPACEBAR_LANGUAGE_ALPHA});
    } else if (key.l === undefined && key.i) {
        const role = key.i === 'enter_key' && state.enterIcon ? state.enterIcon : key.i;
        const iw = Math.min(m.iconSize, w), ih = m.iconSize;
        const iy = has(key, 'alignIconBottom') ? h - ih : trunc((h - ih) / 2);
        paintKeyIcon(cr, role, trunc((w - iw) / 2), iy, iw, ih, c.icon);
    }
    cr.restore();
}

// A canvas drawn in the keyboard's screen pixels: subclasses paint(cr, view) with the context scaled so that one
// unit is one screen pixel and x = 0 at the monitor's left edge
const ScreenCanvas = GObject.registerClass(
class ScreenCanvas extends Canvas {
    setView(view) {
        this._view = view;
        this.queue_repaint();
    }

    draw(cr, _w, _h) {
        cr.setOperator(0);
        cr.paint();
        cr.setOperator(2);
        if (!this._view)
            return;
        cr.scale(1 / this._view.scale, 1 / this._view.scale);
        this.paint(cr, this._view);
    }

    paint(_cr, _view) {
    }
});

function fillBand(cr, v, height) {
    setSource(cr, v.colors.background);
    cr.rectangle(v.m.insetLeft, 0, v.m.width, height);
    cr.fill();
}

// view: {m (geometry.measure), rows (geometry.placeKeys), colors, scale, enterIcon, language, rtl, switcher}

export const KeysCanvas = GObject.registerClass(
class KeysCanvas extends ScreenCanvas {
    paint(cr, v) {
        fillBand(cr, v, v.m.height);
        cr.translate(v.m.insetLeft, 0);
        for (const row of v.rows) {
            for (const k of row.keys)
                paintKey(cr, v.m, k, v.colors, false, v);
        }
    }
});

// The key under the finger, pressed, on a canvas of its own laid over the keys: @place(v, k) moves it there
export const PressedKey = GObject.registerClass(
class PressedKey extends ScreenCanvas {
    place(v, k) {
        const r = drawnRect(v.m, k);
        const s = v.scale;
        const x0 = Math.floor((v.m.insetLeft + r.x) / s), y0 = Math.floor(r.y / s);
        const x1 = Math.ceil((v.m.insetLeft + r.x + r.w) / s), y1 = Math.ceil((r.y + r.h) / s);
        this._key = k;
        this._origin = [x0 * s, y0 * s];
        this.set_position(x0, y0);
        this.set_size(x1 - x0, y1 - y0);
        this.setView(v);
        this.show();
    }

    paint(cr, v) {
        if (!this._key)
            return;
        cr.translate(-this._origin[0], -this._origin[1]);
        const r = drawnRect(v.m, this._key);
        cr.translate(v.m.insetLeft, 0);
        setSource(cr, v.colors.background);
        cr.rectangle(r.x, r.y, r.w, r.h);
        cr.fill();
        paintKey(cr, v.m, this._key, v.colors, true, v);
    }
});

// A long press's panel (geometry.placeMoreKeys): the keys on a rounded card in the letter keys' colour, the one the
// finger chooses in their pressed colour. It stands over the keyboard and above it, in the stage: @place(v, panel,
// keysX, keysY) puts it there, (keysX, keysY) being where the keys' actor is on the stage; @select(i) marks a key.
export const MoreKeysPanel = GObject.registerClass(
class MoreKeysPanel extends ScreenCanvas {
    place(v, panel, keysX, keysY) {
        const s = v.scale;
        const left = v.m.insetLeft + panel.x;
        const x0 = Math.floor(left / s), y0 = Math.floor(panel.y / s);
        const x1 = Math.ceil((left + panel.w) / s), y1 = Math.ceil((panel.y + panel.h) / s);
        this._panel = panel;
        this._selected = -1;
        this._origin = [x0 * s, y0 * s];
        this.set_position(keysX + x0, keysY + y0);
        this.set_size(x1 - x0, y1 - y0);
        this.setView(v);
        this.show();
    }

    select(i) {
        if (i === this._selected)
            return;
        this._selected = i;
        this.queue_repaint();
    }

    paint(cr, v) {
        const panel = this._panel;
        if (!panel)
            return;
        cr.translate(v.m.insetLeft - this._origin[0], -this._origin[1]);
        roundRect(cr, panel.x, panel.y, panel.w, panel.h, panel.corner);
        setSource(cr, v.colors.letter);
        cr.fill();
        // the keys at the panel's text sizes, on the card (only the chosen one has a background of its own)
        const m = {...v.m, letterSize: panel.letterSize, labelSize: panel.labelSize};
        panel.keys.forEach((k, i) =>
            paintKey(cr, m, {...k, key: {...k.key, b: 'empty'}}, v.colors, i === this._selected, v));
    }
});

// The suggestion strip: the float and split keys (the tablet's, turned only the float), the dividers between the three
// words' places and the words (setWords). The microphone stays off until dictation is there.
export const StripCanvas = GObject.registerClass(
class StripCanvas extends ScreenCanvas {
    // @words: [left, middle, right], null where there is none; @bold: the place whose word is in bold (the one a
    // space will put in: an auto-correction), -1 for none
    setWords(words, bold = -1) {
        if (JSON.stringify(words) === JSON.stringify(this._words ?? [null, null, null]) && bold === (this._bold ?? -1))
            return;
        this._words = words;
        this._bold = bold;
        this.queue_repaint();
    }

    paint(cr, v) {
        const m = v.m;
        fillBand(cr, v, m.strip);
        cr.translate(m.insetLeft, 0);
        // mirrored for a right-to-left language
        const at = (x, w) => v.rtl ? m.width - x - w : x;
        if (m.tablet) {
            const tools = m.land ? ['float_key', 'split_key'] : ['float_key'];
            tools.forEach((role, i) => {
                const x = at(i * m.stripEdgeKey, m.stripEdgeKey) + trunc((m.stripEdgeKey - m.iconSize) / 2);
                paintKeyIcon(cr, role, x, trunc((m.strip - m.iconSize) / 2), m.iconSize, m.iconSize, v.colors.tool,
                    TOOL_KEY_ALPHA);
            });
        }
        const [rgb, alpha] = colourAndAlpha(v.colors.divider);
        setSource(cr, rgb, alpha);
        for (const x of stripDividers(m))
            cr.rectangle(at(x, m.divider.width), trunc((m.strip - m.divider.height) / 2), m.divider.width, m.divider.height);
        cr.fill();
        // each word in the middle of its place, on the letters' cap height, squeezed (as LatinIME squeezes it, to 70 %
        // of its width at most) where it is wider than the place less its padding, then cut off
        const size = m.suggestionText;
        const [text_, alpha_] = colourAndAlpha(v.colors.text);
        stripSlots(m).forEach(([x, w], i) => {
            const word = this._words?.[i];
            if (!word)
                return;
            const t = text(word, size, i === this._bold);
            const room = w - 2 * m.suggestionPadding;
            const squeeze = Math.max(MIN_SUGGESTION_SCALE, Math.min(1, room / t.width));
            cr.save();
            cr.rectangle(at(x, w), 0, w, m.strip);
            cr.clip();
            const left = at(x, w) + (w - Math.min(room, t.width * squeeze)) / 2;
            cr.translate(left, Math.round(m.strip / 2 + CAP_HEIGHT * size / 2 - t.baseline));
            cr.scale(squeeze, 1);
            setSource(cr, text_, alpha_);
            cr.moveTo(0, 0);
            PangoCairo.show_layout(cr, t.layout);
            cr.restore();
        });
    }
});

// The tablet keyboard's navigation bar: the hide chevron and the input switcher (with more than one input source).
// No gesture handle: the tablet shows none, the keyboard up or not (owner, 11-21).
export const NavCanvas = GObject.registerClass(
class NavCanvas extends ScreenCanvas {
    paint(cr, v) {
        const m = v.m;
        setSource(cr, v.colors.background);
        cr.rectangle(0, 0, m.monitorWidth, m.navBar);
        cr.fill();
        const parts = navBarParts(m);
        const [rgb, alpha] = colourAndAlpha(v.colors.navIcon);
        const {hide, switcher} = parts;
        cr.save();
        cr.translate(hide.cx, hide.cy);
        // the back chevron, turned to point down
        cr.rotate(-Math.PI / 2);
        paintKeyIcon(cr, 'nav_hide', -hide.size / 2, -hide.size / 2, hide.size, hide.size, rgb, alpha);
        cr.restore();
        if (v.switcher) {
            paintKeyIcon(cr, 'nav_switcher', switcher.cx - switcher.size / 2, switcher.cy - switcher.size / 2,
                switcher.size, switcher.size, rgb, alpha);
        }
    }

    // what a tap at @x (screen pixels from the monitor's left edge) does: 'hide', 'switch' or null
    hitTest(v, x) {
        const {hide, switcher} = navBarParts(v.m);
        const reach = 64 * v.m.density;
        if (Math.abs(x - hide.cx) < reach)
            return 'hide';
        if (v.switcher && Math.abs(x - switcher.cx) < reach)
            return 'switch';
        return null;
    }
});
