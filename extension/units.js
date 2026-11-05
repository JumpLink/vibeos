// Android's pixels in the shell's: the reference phone (Android 16 on the same S10e) is 1080x2280 at 3 px per dp. The
// shell's scale is mutter's, from the panel's DPI (3 on the S10e since mutter allows a 360-wide logical screen; 2.5
// before, a 432x912 stage). Every size in the SystemUI spec goes through here.

// screen pixels per logical pixel, the primary monitor's as the extension starts
export const SCALE = global.display.get_monitor_scale(global.display.get_primary_monitor()) || 3;

// a physical (screen) pixel length of the 1080x2280 reference
export const px = v => v / SCALE;

// Android dp, and sp at font scale 1.0
export const dp = v => v * 3 / SCALE;
export const sp = dp;

// Cairo drawing in screen pixels: scale a logical-pixel context so 1 unit is 1 screen pixel
export function screenPixels(cr) {
    cr.scale(1 / SCALE, 1 / SCALE);
}
