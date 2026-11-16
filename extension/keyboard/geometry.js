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
// under the keys, the tablet's keyboard draws its own navigation bar: the hide chevron, the input switcher, the
// gesture handle
const TABLET_NAV_BAR_DP = 56;
// the suggestion strip's dividers between its three words
const STRIP_DIVIDER_DP = {width: 1, height: 24};
const KEYBOARD_ROWS = 4;

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

    const strip = pixels(g.stripHeight * density);
    const navBar = tablet ? pixels(TABLET_NAV_BAR_DP * density) : 0;
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
        suggestionsCount: g.suggestionsCount,
        centerSuggestion: g.centerSuggestion,
        divider: {width: pixels(STRIP_DIVIDER_DP.width * density), height: pixels(STRIP_DIVIDER_DP.height * density)},
    };
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

// The tablet's navigation bar: where its hide chevron and input switcher centre, and its gesture handle, in the
// bar's screen pixels
export function navBarParts(m) {
    const d = m.density;
    return {
        hide: {cx: 42.75 * d, cy: 25.5 * d, size: 28 * d},
        switcher: {cx: m.monitorWidth - 43.25 * d, cy: 25.5 * d, size: 20 * d},
        handle: {w: 220 * d, h: 4 * d, y: 38 * d},
    };
}
