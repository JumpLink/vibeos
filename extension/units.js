// Android's pixels in the shell's: the reference phone (Android 16 on the same S10e) is 1080x2280 at 3 px per dp, the
// shell runs at scale 2.5, so its stage is 432x912 logical pixels. Every size in the SystemUI spec goes through here.

// screen pixels per logical pixel
export const SCALE = 2.5;

// a physical (screen) pixel length of the 1080x2280 reference
export const px = v => v / SCALE;

// Android dp, and sp at font scale 1.0
export const dp = v => v * 3 / SCALE;
export const sp = dp;

// Cairo drawing in screen pixels: scale a logical-pixel context so 1 unit is 1 screen pixel
export function screenPixels(cr) {
    cr.scale(1 / SCALE, 1 / SCALE);
}
