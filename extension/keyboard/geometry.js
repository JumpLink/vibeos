// Where the keyboard's parts and keys go, in screen pixels, sized the way the AOSP keyboard sizes itself: its
// numbers per form factor (layouts/geometry.json), the monitor, and the keyboard's own dp. No Shell imports, so that
// the arithmetic can be checked outside the shell.
//
// The dp here is the keyboard's own: one dp is one logical pixel, so as many screen pixels as the monitor's scale
// (3 on the phone, 2 on the tablet). The shell's own units.dp() counts the phone's 3 screen pixels on every device and
// would make the tablet's keyboard half as large again.

// a monitor whose smaller side is at least this many dp gets the tablet's layouts and sizes
const TABLET_MIN_WIDTH_DP = 600;
// turned on a tablet, the keyboard is this much lower than its default height (the height a split keyboard has,
// split or not)
const TABLET_LAND_HEIGHT_RATIO = 0.78;
// a tablet keeps a taskbar's height out of the app area its keyboard's maximum height is a share of
const TABLET_TASKBAR_DP = 32;
// under the keys, the tablet's keyboard draws its own navigation bar: the hide chevron and the input switcher
const TABLET_NAV_BAR_DP = 56;
// the suggestion strip's dividers between its three words
const STRIP_DIVIDER_DP = {width: 1, height: 24};
const KEYBOARD_ROWS = 4;
// a long press's panel: its keys a tenth of the screen's shorter side wide (8 % on the tablet), upright or turned;
// a key whose label is longer than a letter as wide as the label and this much (and a fifth of a key more where
// the layout says its panel has labels)
const MORE_KEYS_WIDTH = {phone: 0.1, tablet: 0.08};
const MORE_KEYS_LABEL_PADDING_DP = 12;
const MORE_KEYS_LABEL_PADDING_RATIO = 0.2;
// a panel of one key, where the keyboard shows its key preview: the preview's size, so that one becomes the other
const SINGLE_MORE_KEY_DP = {width: 40};
const MORE_KEYS_CORNER_DP = 8;

export const FORMS = ['phone', 'phone-land', 'tablet', 'tablet-land'];

// The form factor for a monitor of @widthDp x @heightDp (logical pixels)
export function formFactor(widthDp, heightDp) {
    const tablet = Math.min(widthDp, heightDp) >= TABLET_MIN_WIDTH_DP;
    return `${tablet ? 'tablet' : 'phone'}${widthDp > heightDp ? '-land' : ''}`;
}

const trunc = Math.trunc;
// a dimension in whole pixels, rounded and at least 1, as a view's size is
const pixels = v => v === 0 ? 0 : Math.max(1, Math.round(v));

// The keyboard's frame for one monitor:
//   g: geometry.json's entry for the form factor; form: its name
//   monitorWidth, monitorHeight: the monitor in screen pixels; density: screen pixels per dp
//   insetLeft, insetRight: screen pixels at the sides the keys stay out of (a camera hole's band, turned)
export function measure(g, form, {monitorWidth, monitorHeight, density, insetLeft = 0, insetRight = 0}) {
    const tablet = form.startsWith('tablet');
    const land = form.endsWith('-land');
    const width = monitorWidth - insetLeft - insetRight;
    const appHeight = monitorHeight - (tablet ? pixels(TABLET_TASKBAR_DP * density) : 0);

    // the default height, no higher than its share of the app area and no lower than its minimum (a share of the
    // width where the minimum is given negative)
    const maxH = g.maxKeyboardHeight * appHeight;
    const minH = g.minKeyboardHeight < 0 ? -g.minKeyboardHeight * width : g.minKeyboardHeight * appHeight;
    let height = trunc(Math.max(Math.min(g.keyboardHeight * density, maxH), minH));
    if (tablet && land)
        height = Math.round(height * TABLET_LAND_HEIGHT_RATIO);

    const top = trunc(g.topPadding * height);
    const bottom = trunc(g.bottomPadding * height);
    const left = trunc(g.leftPadding * width);
    const right = trunc(g.rightPadding * width);
    const baseWidth = width - left - right;
    const hgap = trunc(g.horizontalGap * baseWidth);
    const vgap = trunc(g.verticalGap * height);
    const baseHeight = height - top - bottom + vgap;
    const rowHeight = trunc(baseHeight / KEYBOARD_ROWS);
    // the most common key's height: every text size is a share of it
    const keyHeight = rowHeight - vgap;
    const size = ratio => trunc(ratio * keyHeight);

    const navBar = tablet ? pixels(TABLET_NAV_BAR_DP * density) : 0;
    // the strip a pixel or two higher where that makes the whole keyboard a whole number of logical pixels: the shell
    // fits the app above it in logical pixels, and half of one left a line of what is behind between the two
    let strip = pixels(g.stripHeight * density);
    if (Number.isInteger(density) && density > 1)
        strip += (density - (strip + height + navBar) % density) % density;
    return {
        form, tablet, land, density,
        monitorWidth, monitorHeight, insetLeft, insetRight, width,
        strip, height, navBar, total: strip + height + navBar,
        top, bottom, left, right, baseWidth, hgap, vgap, baseHeight, rowHeight, keyHeight,
        // every key's background is drawn this much larger on each side than the key
        bleed: g.keyBackgroundPadding * density,
        corner: g.keyCornerRadius * density,
        actionCorner: g.actionKeyCornerRadius * density,
        letterSize: size(g.letterRatio),
        labelSize: size(g.labelRatio),
        largeLetterSize: size(g.largeLetterRatio),
        hintLetterSize: size(g.hintLetterRatio),
        hintLabelSize: size(g.hintLabelRatio),
        shiftedHintSize: size(g.shiftedLetterHintRatio),
        spacebarTextSize: keyHeight * g.spacebarLanguageRatio,
        hintPadding: g.hintLetterPadding * density,
        hintPaddingVertical: g.hintLetterPaddingVertical * density,
        shiftedHintPadding: g.shiftedLetterHintPadding * density,
        iconSize: pixels(24 * density),
        stripMargin: pixels(g.stripMargin * density),
        stripEdgeKey: pixels(g.stripEdgeKeyWidth * density),
        suggestionPadding: pixels(g.suggestionPadding * density),
        suggestionText: pixels(g.suggestionTextSize * density),
        suggestionsCount: g.suggestionsCount,
        centerSuggestion: g.centerSuggestion,
        divider: {width: pixels(STRIP_DIVIDER_DP.width * density), height: pixels(STRIP_DIVIDER_DP.height * density)},
        moreKeys: {
            keyWidth: trunc(MORE_KEYS_WIDTH[tablet ? 'tablet' : 'phone'] * Math.min(monitorWidth, monitorHeight)),
            // the panel's rows are the keyboard's, with half its gap between them
            vgap: trunc(vgap / 2),
            maxColumns: g.moreKeysMaxColumns,
            longPress: g.longPressTimeout,
            // its bottom this far below the key's top: where the key preview's bottom is, where there is one
            offset: g.previewOn ? g.previewOffset * density : 0,
            single: g.previewOn
                ? {width: pixels(SINGLE_MORE_KEY_DP.width * density), height: pixels(g.previewHeight * density)} : null,
            labelPadding: MORE_KEYS_LABEL_PADDING_DP * density,
            letterRatio: g.letterRatio,
            labelRatio: g.labelRatio,
            corner: MORE_KEYS_CORNER_DP * density,
        },
    };
}

// The panel a long press on @k (placed by placeKeys) opens with its more keys, laid out as the AOSP keyboard lays
// them: the first over the key, the others in turn right and left of it, rows of them upwards; a top row with fewer
// keys than the others half a key to the left where it would not be centred otherwise; the panel inside the
// keyboard. A layout can ask for a fixed number of columns and for its own order.
//   @at: the x the first key centres on (the key's centre on the phone, the finger on the tablet)
//   @labelWidth(label, size): a label's width at a text size
// Returns {x, y, w, h, keys: [{key (the more key), x, y, w, h}], letterSize, labelSize} in the keyboard's screen
// pixels (as placeKeys's; y above the keys is negative).
export function placeMoreKeys(m, k, at, labelWidth) {
    const mk = m.moreKeys;
    const more = k.key.m;
    const n = more.length;
    const flags = k.key.mf ?? [];
    const single = n === 1 && mk.single && !k.key.a?.includes('noPreview');

    let keyWidth, rowHeight;
    if (single) {
        keyWidth = mk.single.width;
        rowHeight = mk.single.height + mk.vgap;
    } else {
        keyWidth = mk.keyWidth;
        rowHeight = m.rowHeight;
    }
    const keyHeight = rowHeight - mk.vgap;
    const letterSize = trunc(mk.letterRatio * keyHeight);
    const labelSize = trunc(mk.labelRatio * keyHeight);
    if (!single) {
        const padding = mk.labelPadding + (flags.includes('labels') ? keyWidth * MORE_KEYS_LABEL_PADDING_RATIO : 0);
        for (const more1 of more) {
            if (more1.l && [...more1.l].length > 1)
                keyWidth = Math.max(keyWidth, trunc(labelWidth(more1.l, labelSize) + padding));
        }
    }

    const fixedColumns = flags.includes('fixed') || flags.includes('auto');
    const fixedOrder = flags.includes('fixed');
    const maxColumns = Math.max(1, Math.min(k.key.mc ?? mk.maxColumns, trunc(m.width / keyWidth)));
    const rows = Math.ceil(n / maxColumns);
    let columns = Math.min(n, maxColumns);
    if (!fixedColumns) {
        // as few columns as leave the top row less than a row short
        const empty = c => n % c === 0 ? 0 : c - n % c;
        while (columns > 1 && empty(columns) >= rows)
            columns--;
    }
    const topKeys = n % columns === 0 ? columns : n % columns;

    // how many columns go left of the first key and how many right of it (the first included), as far as the
    // keyboard's edges allow
    const centre = k.x + k.w / 2;
    const maxLeft = trunc(centre / keyWidth);
    const maxRight = trunc((m.width - centre) / keyWidth);
    let left = trunc((columns - 1) / 2);
    let right = columns - left;
    if (left > maxLeft) {
        left = maxLeft;
        right = columns - left;
    } else if (right > maxRight + 1) {
        right = maxRight + 1;
        left = columns - right;
    }
    if (maxLeft === left && left > 0) {
        left--;
        right++;
    }
    if (maxRight === right - 1 && right > 1) {
        left++;
        right--;
    }
    const topRow = row => rows > 1 && row === rows - 1;
    const topShift = rows === 1 || topKeys % 2 === 1 || topKeys === columns || left === 0 || right === 1 ? 0 : -1;

    const column = i => {
        const col = i % columns;
        if (fixedOrder) {
            if (!topRow(trunc(i / columns)))
                return col - left;
            const rightSide = trunc(topKeys / 2);
            const leftSide = topKeys - (rightSide + 1);
            const pos = col - leftSide;
            const roomLeft = left + topShift, roomRight = right - 1;
            if (roomRight >= rightSide && roomLeft >= leftSide)
                return pos;
            if (roomRight < rightSide)
                return pos - (rightSide - roomRight);
            return pos + (leftSide - roomLeft);
        }
        if (col === 0)
            return 0;
        let pos = 0, r = 1, l = 0, j = 0;
        for (;;) {
            if (r < right) {
                pos = r++;
                j++;
            }
            if (j >= col)
                break;
            if (l < left) {
                pos = -++l;
                j++;
            }
            if (j >= col)
                break;
        }
        return pos;
    };

    const w = columns * keyWidth;
    const h = rows * rowHeight - mk.vgap;
    const firstX = left * keyWidth;
    // (a panel of one key, the preview's width, can be wider than its key at the edge)
    const x = Math.max(0, Math.min(Math.round(at - firstX - keyWidth / 2), m.width - w));
    const y = Math.round(k.y + mk.offset - h);
    const keys = more.map((key, i) => {
        const row = trunc(i / columns);
        let kx = column(i) * keyWidth + firstX;
        if (topRow(row))
            kx += topShift * trunc(keyWidth / 2);
        return {key, x: x + kx, y: y + (rows - 1 - row) * rowHeight, w: keyWidth, h: keyHeight};
    });
    return {x, y, w, h, keys, letterSize, labelSize, corner: mk.corner};
}

// The panel's key a finger at (@x, @y) (the keyboard's screen pixels) chooses: the nearest within @reach of it (above
// the panel a little further), or -1
export function moreKeyAt(panel, x, y, reach) {
    let best = -1, bestDist = (y < panel.y ? 2 : 1) * reach * reach;
    panel.keys.forEach((k, i) => {
        const dx = Math.max(k.x - x, 0, x - (k.x + k.w));
        const dy = Math.max(k.y - y, 0, y - (k.y + k.h));
        const dist = dx * dx + dy * dy;
        if (dist < bestDist) {
            best = i;
            bestDist = dist;
        }
    });
    return best;
}

// The keys of a page (rows of layout keys) placed in @m's keyboard, each key with:
//   key: the layout's key; row: its row
//   x, y, w, h: the key as drawn, in the keyboard's screen pixels (x from the keyboard's left edge, insets
//   excluded); where the layout says so (the phone's shift and delete keys) narrower than the span it takes
//   hitX, hitW: the span along the row a touch picks it in (the gaps split between neighbours, the row's ends
//   reaching the keyboard's edges)
// Rows: [{y, h, keys}] with y, h the band a touch picks the row in.
export function placeKeys(m, page) {
    const rows = [];
    let y = m.top;
    page.forEach((row, r) => {
        const keys = [];
        for (const key of row) {
            const keyX = m.left + key.x / 100 * m.baseWidth;
            const keyW = key.w / 100 * m.baseWidth;
            if (key.spacer)
                continue;
            const insetLeft = Math.round((key.vl ?? 0) / 100 * m.baseWidth);
            const insetRight = Math.round((key.vr ?? 0) / 100 * m.baseWidth);
            keys.push({
                key, row: r,
                x: Math.round(keyX + m.hgap / 2) + insetLeft, y,
                w: Math.round(keyW - m.hgap) - insetLeft - insetRight, h: m.rowHeight - m.vgap,
                hitX: keyX, hitW: keyW,
            });
        }
        if (keys.length > 0) {
            keys[0].hitW += keys[0].hitX;
            keys[0].hitX = 0;
            const last = keys[keys.length - 1];
            last.hitW = m.width - last.hitX;
        }
        rows.push({y, h: m.rowHeight, keys});
        y += m.rowHeight;
    });
    if (rows.length > 0) {
        rows[0].h += rows[0].y;
        rows[0].y = 0;
        const last = rows[rows.length - 1];
        last.h = m.height - last.y;
    }
    return rows;
}

// The key a touch at (@x, @y) of the keyboard (screen pixels, insets excluded) lands on, or null
export function keyAt(rows, x, y) {
    for (const row of rows) {
        if (y < row.y || y >= row.y + row.h)
            continue;
        for (const k of row.keys) {
            if (x >= k.hitX && x < k.hitX + k.hitW)
                return k;
        }
        return null;
    }
    return null;
}

// A key's background as drawn: the key grown by the bleed on every side
export function drawnRect(m, k) {
    return {x: k.x - m.bleed, y: k.y - m.bleed, w: k.w + 2 * m.bleed, h: k.h + 2 * m.bleed};
}

// The suggestion strip's dividers: x of each, in the strip's screen pixels from the keyboard's left edge, for
// three words in the strip between its margins, the middle one wider
export function stripDividers(m) {
    const n = m.suggestionsCount;
    const maxWidth = m.width - 2 * m.stripMargin;
    const available = maxWidth - m.suggestionPadding * 2 * n - m.divider.width * (n - 1);
    const side = (1 - m.centerSuggestion) / (n - 1);
    const xs = [];
    let x = m.stripMargin;
    for (let i = 0; i < n - 1; i++) {
        const weight = i === (n - 1) / 2 ? m.centerSuggestion : side;
        x += trunc(available * weight) + m.suggestionPadding * 2;
        xs.push(x);
        x += m.divider.width;
    }
    return xs;
}

// The suggestion strip's three words' places, left to right: [x, w] of each, in the strip's screen pixels from the
// keyboard's left edge (before a right-to-left language mirrors them), each between its dividers
export function stripSlots(m) {
    const xs = stripDividers(m);
    const edges = [m.stripMargin, ...xs.flatMap(x => [x, x + m.divider.width]), m.width - m.stripMargin];
    const slots = [];
    for (let i = 0; i < edges.length; i += 2)
        slots.push([edges[i], edges[i + 1] - edges[i]]);
    return slots;
}

// The tablet's navigation bar: where its hide chevron and input switcher centre, in the bar's screen pixels
export function navBarParts(m) {
    const d = m.density;
    return {
        hide: {cx: 42.75 * d, cy: 25.5 * d, size: 28 * d},
        switcher: {cx: m.monitorWidth - 43.25 * d, cy: 25.5 * d, size: 20 * d},
    };
}
