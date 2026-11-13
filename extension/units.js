// The reference's pixels in the shell's: the shell is laid out in the screen pixels of the S10e (1080x2280), some
// sizes in units of 3 of them (dp() below). The shell's scale is mutter's, from the panel's DPI (3 on the S10e since
// mutter allows a 360-wide logical screen; 2.5 before, a 432x912 stage). Every size in the shell goes through here.

// screen pixels per logical pixel, the primary monitor's as the extension starts
export const SCALE = global.display.get_monitor_scale(global.display.get_primary_monitor()) || 3;

// the panel upright, in screen pixels: the primary monitor's as the extension starts (1080x2280 on the reference
// S10e, 1080x2400 on the POCO). The layout's lengths stay the reference's: a panel of another width is laid out
// in the same pixels, only the full-screen sizes and what is anchored to the bottom edge follow the panel.
const primary = global.display.get_primary_monitor();
const panel = primary >= 0 ? global.display.get_monitor_geometry(primary) : null;
export const SCREEN_W = Math.round(Math.min(panel?.width ?? 0, panel?.height ?? 0) * SCALE) || 1080;
export const SCREEN_H = Math.round(Math.max(panel?.width ?? 0, panel?.height ?? 0) * SCALE) || 2280;

// a physical (screen) pixel length of the 1080x2280 reference
export const px = v => v / SCALE;

// a length in units of 3 screen px (the reference's scale), and a text size in the same units
export const dp = v => v * 3 / SCALE;
export const sp = dp;

// Cairo drawing in screen pixels: scale a logical-pixel context so 1 unit is 1 screen pixel
export function screenPixels(cr) {
    cr.scale(1 / SCALE, 1 / SCALE);
}
