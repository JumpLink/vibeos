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

// The shade panel (behind the scrim): three translucent layers flattened into one colour and alpha
export const SHADE_PANEL = over([Color.accent1_800, 0.32],
    over([Color.neutral1_500_l4, 0.32], [[64, 64, 64], 0.40]));

const FAMILY = 'Google Sans Flex';

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
