// Colours and type of the shell's system UI: the colour roles of its dark palette, and the variable Google Sans Flex
// its text is set in.
import Cogl from 'gi://Cogl';

import {sp} from './units.js';

// [r, g, b] 0..255; roles are the dark scheme
export const Color = {
    primary: [156, 207, 215],               // sliders, active tiles, actions
    onPrimary: [13, 71, 78],
    surface: [10, 15, 16],                  // the volume pill
    surfaceContainerLow: [15, 21, 22],      // the power menu's panel
    surfaceContainer: [20, 27, 28],         // a heads-up's card
    surfaceContainerHigh: [24, 32, 34],
    surfaceContainerHighest: [30, 39, 41],  // the inactive volume track
    onSurface: [221, 231, 233],             // all notification text
    onSurfaceVariant: [190, 200, 202],
    tertiary: [182, 199, 234],
    accent1_100: [184, 235, 243],
    accent1_800: [5, 54, 63],               // the shade's tint
    neutral1_500_l4: [11, 13, 14],          // a near-black neutral, under the shade's tint
    neutral1_500_l6: [17, 21, 22],          // ... a shade lighter: the cards and inactive tiles
    notificationScrim: [5, 28, 28],         // behind the notification stack
    white: [255, 255, 255],
    black: [0, 0, 0],
    // the battery's fill by its charge, saturated mid-tones that read on the dark bar and over a light app
    batteryGood: [0x18, 0xcc, 0x47],        // above 50 %: Android's charging green
    batteryMedium: [0xf5, 0xb8, 0x00],      // 30 to 50 %
    batteryLow: [0xff, 0x7a, 0x00],         // 15 to 29 %
    batteryCritical: [0xff, 0x0e, 0x01],    // below 15 %: Android's battery error red
};

// translucent fills drawn over the blurred shade
export const SurfaceEffect = {
    0: [Color.accent1_800, 0.50],
    1: [Color.neutral1_500_l6, 0.54],
    2: [Color.accent1_100, 0.15],
    3: [Color.accent1_100, 0.10],
};

export function rgba(c, a = 1) {
    return `rgba(${c[0]}, ${c[1]}, ${c[2]}, ${a})`;
}

export function cogl(c, a = 1) {
    return new Cogl.Color({red: c[0], green: c[1], blue: c[2], alpha: Math.round(a * 255)});
}

export function setSource(cr, c, a = 1) {
    cr.setSourceRGBA(c[0] / 255, c[1] / 255, c[2] / 255, a);
}

// a over b, both [rgb, alpha]: the opaque-equivalent colour and the combined alpha
export function over([ca, aa], [cb, ab]) {
    const a = aa + ab * (1 - aa);
    if (a === 0)
        return [[0, 0, 0], 0];
    return [ca.map((v, i) => (v * aa + cb[i] * ab * (1 - aa)) / a), a];
}

export function mix(a, b, t) {
    return a.map((v, i) => v + (b[i] - v) * t);
}

// A tablet's shade: an opaque scrim under the quick settings, their pill tiles (a two-target tile's icon circle when
// off, a tile that is unavailable) and the notification cards
export const TabletShade = {
    scrim: [17, 53, 58],
    tile: [16, 35, 38],
    tileTarget: [40, 65, 69],
    unavailable: [20, 47, 50],
    card: Color.surfaceContainer,
};

// A tablet's taskbar: the stashed handle, the pill's fill and outline, its dividers and the all-apps glyph
export const TabletTaskbar = {
    handleLight: [255, 255, 255, 0.92],     // the handle over a dark app
    handleDark: [0, 0, 0, 0.6],             // over a light one
    fill: [14, 21, 22],
    outline: [51, 59, 60],
    divider: [137, 148, 150],
    glyph: [186, 201, 204],
};

// The shade panel (behind the scrim): three translucent layers flattened into one colour and alpha
const shadePanel = () => over([Color.accent1_800, 0.32], over([Color.neutral1_500_l4, 0.32], [[64, 64, 64], 0.40]));
export const SHADE_PANEL = shadePanel();

// High contrast (Accessibility: org.gnome.desktop.a11y.interface high-contrast, followed by contrast.js): the same
// roles with the text at full strength, the surfaces under it black or nearly, the tracks and the primary colour
// lighter, and the translucent fills over the blurred shade nearly opaque, so that text and icons stand out from
// whatever is behind them. Switched in place: the arrays stay the same objects, so a module that took one at load
// (qs.js PHONE_TILE = SurfaceEffect[1]) draws with the new values too.
const HIGH_CONTRAST = {
    primary: [190, 238, 246],
    onPrimary: [0, 38, 43],
    surface: [0, 0, 0],
    surfaceContainerLow: [0, 0, 0],
    surfaceContainer: [6, 8, 9],
    surfaceContainerHigh: [10, 13, 14],
    surfaceContainerHighest: [84, 98, 101],
    onSurface: [255, 255, 255],
    onSurfaceVariant: [240, 246, 247],
    neutral1_500_l4: [0, 0, 0],
    neutral1_500_l6: [5, 7, 8],
    notificationScrim: [0, 0, 0],
};
const HIGH_CONTRAST_EFFECT = {0: 0.88, 1: 0.92, 2: 0.30, 3: 0.24};
const normal = Object.fromEntries(Object.entries(Color).map(([k, c]) => [k, [...c]]));
const normalEffect = Object.fromEntries(Object.entries(SurfaceEffect).map(([k, [, a]]) => [k, a]));
let highContrast = false;

export function isHighContrast() {
    return highContrast;
}

// true when it changed
export function setHighContrast(on) {
    on = !!on;
    if (on === highContrast)
        return false;
    highContrast = on;
    for (const [key, rgb] of Object.entries(normal))
        Color[key].splice(0, 3, ...(on ? HIGH_CONTRAST[key] ?? rgb : rgb));
    for (const key of Object.keys(SurfaceEffect))
        SurfaceEffect[key][1] = on ? HIGH_CONTRAST_EFFECT[key] : normalEffect[key];
    const [rgb, a] = shadePanel();
    SHADE_PANEL[0].splice(0, 3, ...rgb);
    SHADE_PANEL[1] = a;
    return true;
}

// Google Sans Flex, and for the letters it has none of (Cyrillic) VibeOS Roboto, as Android's system UI falls back
// to Roboto
const FAMILY = 'Google Sans Flex, VibeOS Roboto';

// A Pango font description for a Clutter.Text: size in sp, the variable axes set for rounded terminals
export function font(sizeSp, wght, {wdth = 100, rond = 100, opsz = sizeSp} = {}) {
    return `${FAMILY} ${sp(sizeSp).toFixed(3)}px @wght=${wght},wdth=${wdth},ROND=${rond},opsz=${opsz}`;
}

// The type scale of the shell's text roles: [size (sp(), units of 3 screen px), weight, variable axes]
export const Type = {
    labelLarge: [14, 500],
    labelLargeEmphasized: [14, 700, {opsz: 16}],
    labelMedium: [12, 500],
    labelSmallEmphasized: [11, 600],
    bodySmall: [12, 400],
    bodyMedium: [14, 400],
    bodyMediumEmphasized: [14, 500],
    titleSmallEmphasized: [14, 600],
    titleMediumEmphasized: [16, 600],
    displaySmallEmphasized: [36, 500],
};
