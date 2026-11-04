// Colours and type of the Android 16 SystemUI: the dark Material 3 roles of the reference phone's dynamic palette
// (measured on its stills, same wallpaper), and the variable Google Sans Flex standing in for Android's variable-*
// font families.
import Cogl from 'gi://Cogl';

import {sp} from './units.js';

// [r, g, b] 0..255; roles are the dark scheme
export const Color = {
    primary: [156, 207, 215],               // #9CCFD7, slider, active tiles, actions
    onPrimary: [13, 71, 78],                // #0D474E
    surface: [10, 15, 16],                  // #0A0F10, volume pill
    surfaceContainer: [20, 27, 28],         // a heads-up's card: tone 9 at High's hue (2025 dark: 9, 12, 15), not measured
    surfaceContainerHigh: [24, 32, 34],
    surfaceContainerHighest: [30, 39, 41],  // #1E2729, inactive volume track
    onSurface: [221, 231, 233],             // #DDE7E9, all notification text
    onSurfaceVariant: [190, 200, 202],
    tertiary: [182, 199, 234],
    accent1_100: [184, 235, 243],           // #B8EBF3
    accent1_800: [5, 54, 63],               // tone 20, fitted to the measured shade background
    neutral1_500_l4: [11, 13, 14],          // neutral1_500 re-lit to L* 4
    neutral1_500_l6: [17, 21, 22],          // ... to L* 6, fitted to the measured cards and inactive tiles
    notificationScrim: [5, 28, 28],         // fitted: the stack backdrop measures (11, 33, 35)
    white: [255, 255, 255],
    black: [0, 0, 0],
};

// surface_effect_N (dark): translucent fills drawn over the blurred shade
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

// The shade panel (behind scrim): accent1_800 α .32 over neutral1_500@L*4 α .32 over #404040 α .40, α 0.723
// (ShadeColors.kt:33-71)
export const SHADE_PANEL = over([Color.accent1_800, 0.32],
    over([Color.neutral1_500_l4, 0.32], [[64, 64, 64], 0.40]));

const FAMILY = 'Google Sans Flex';

// A Pango font description for a Clutter.Text: size in sp, the variable axes as Android's variable-* families set
// them (rounded terminals)
export function font(sizeSp, wght, {wdth = 100, rond = 100, opsz = sizeSp} = {}) {
    return `${FAMILY} ${sp(sizeSp).toFixed(3)}px @wght=${wght},wdth=${wdth},ROND=${rond},opsz=${opsz}`;
}

// The M3 type scale (sp, weight) of the roles SystemUI uses; *Emphasized are the variable-*-emphasized families
export const Type = {
    labelLarge: [14, 500],
    labelLargeEmphasized: [14, 700, {opsz: 16}],   // matched to the status bar clock's pixels
    labelMedium: [12, 500],
    labelSmallEmphasized: [11, 600],
    bodySmall: [12, 400],
    bodyMedium: [14, 400],
    bodyMediumEmphasized: [14, 500],
    titleSmallEmphasized: [14, 600],
    titleMediumEmphasized: [16, 600],
    displaySmallEmphasized: [36, 500],
};
