// Where the parts of the shade go, for the screen as it is turned now. Screen px (3 to a dp).
//
// A tablet's (a smallest width of 600 dp or more): turned sideways the shade is split: the quick settings fill the left
// half (the brightness and volume rows, every tile, the footer at the bottom) and the notifications sit in a rounded
// panel on the right half, under a header row that spans the screen; a second pull adds nothing. Upright it is one
// centred column 640 dp wide: two rows of tiles over the notification panel, and the rest of the quick settings on a
// second pull, which takes the panel down off the screen.
//
// A phone's: the header row on the status bar's line, two rows of two large tiles under it, and the notification
// panel under them across the screen, its cards 16 dp in from its sides. The second pull takes the panel down off the
// screen and shows the rest: a big clock with the date under it, the carrier and the icons at the right, the brightness
// row, the large tiles in two columns, the small ones in four, and the settings and power buttons at the bottom right.
// Turned sideways a phone's shade is the same column, centred, with no second pull.
import {HEIGHT, SCREEN_W, TABLET, WIDTH} from './units.js';

const DP = 3;
const COLUMN_W = 640 * DP;              // upright: the shade's column
const COLUMN_MARGIN = 16 * DP;          // ... and its least margin on a narrow tablet
const PANEL_RADIUS = 32 * DP;
const ROW_INSET_LAND = 16 * DP;         // the cards' inset in the notification panel, at its top and its sides
const ROW_INSET_PORT = 40 * DP;
const ROW_END_INSET = 16 * DP;          // the stack scrolls to end this far above the panel's bottom
const AUTO_SIZE = 52 * DP;              // the auto-brightness button
const VOLUME_ROW = 60 * DP;             // the volume row under the brightness one, and the tiles that far down
const AUTO_GAP = 13 * DP;               // between the track and the button
const FOOTER_PITCH = 48 * DP;           // between the footer's buttons

// a phone's
const PHONE_SIDE = 16 * DP;             // the tiles' and the cards' margin on each side
const PHONE_TILE_GAP = 8 * DP;          // between tiles, across and down
const PHONE_TILE_PITCH = 80 * DP;       // a row of tiles and the gap under it
const PHONE_PANEL_GAP = 24 * DP;        // from the second row of tiles to the notification panel
const PHONE_TRAVEL = 2190;              // the finger's way that opens the shade
const PHONE_GRID_TOP = 214 * DP;        // all the quick settings: the first row of tiles
const PHONE_BRIGHTNESS_CY = 174 * DP;
const PHONE_FOOTER_UP = 52 * DP;        // the settings and power buttons' centre above the screen's bottom
const PHONE_POWER_END = 36 * DP;        // ... the power button's centre from the column's end
const PHONE_FOOTER_PITCH = 48 * DP;
const PHONE_CLOCK_INK_TOP = 54 * DP;    // the big clock's digits
const PHONE_DATE_INK_TOP = 99 * DP;     // the date under it
const PHONE_DATE_GAP = 13 * DP;         // first pull: the date this far past the clock's digits
const PHONE_CARRIER_END = 21 * DP;      // the carrier's end from the column's end, and its top
const PHONE_CARRIER_INK_TOP = 63 * DP;
const PHONE_ICONS_END = 22 * DP;        // the system icons' end from the column's end, and their centre line
const PHONE_ICONS_CY = 103 * DP;
const PHONE_FOOTER_PILL_Y = 12 * DP;    // the notification footer's buttons: their top under the last card,
const PHONE_FOOTER_PILL_H = 48 * DP;    // ... and their height

// The shade's layout: a tablet's, or a phone's whose header row is the status bar's line, @barHeight tall
export function shadeLayout(barHeight) {
    return TABLET ? tabletShade() : phoneShade(barHeight);
}

function tabletShade() {
    const W = WIDTH, H = HEIGHT;
    // the header row across the top: the clock, the date and the icons on the status bar's line (qs.js), the rest of
    // the shade under it
    const header = {width: W, height: 56 * DP};

    if (W > H) {
        const left = 40 * DP, width = W / 2 - 2 * left;
        const panel = {x: W / 2, top: 56 * DP, right: W - 24 * DP, bottom: H - 40 * DP};
        const powerCX = left + width - 20 * DP;
        return {
            landscape: true,
            width: W,
            height: H,
            travel: 300 * DP,
            qsTravel: 0,
            header,
            qs: {
                left, width, trackEnd: left + width - AUTO_SIZE - AUTO_GAP, autoSize: AUTO_SIZE,
                qqsRows: 0, qqsTop: 160 * DP + VOLUME_ROW, gridTop: 160 * DP + VOLUME_ROW, brightnessCY: 120 * DP,
                volumeCY: 120 * DP + VOLUME_ROW,
                footerCY: H - 60 * DP, powerCX, settingsCX: powerCX - FOOTER_PITCH,
            },
            panel: {...panel, width: panel.right - panel.x, height: panel.bottom - panel.top, radius: PANEL_RADIUS,
                roundBottom: true},
            rows: {
                top: ROW_INSET_LAND,
                left: panel.x + ROW_INSET_LAND,
                width: panel.right - panel.x - 2 * ROW_INSET_LAND,
                end: panel.bottom,
                scrollEnd: panel.bottom - ROW_END_INSET,
                clipX: panel.x,
                clipW: panel.right - panel.x,
            },
        };
    }

    const columnW = Math.min(COLUMN_W, W - 2 * COLUMN_MARGIN);
    const columnX = (W - columnW) / 2;
    const left = columnX + ROW_INSET_PORT, width = columnW - 2 * ROW_INSET_PORT;
    const powerCX = left + width + 4 * DP;
    return {
        landscape: false,
        width: W,
        height: H,
        travel: 300 * DP,
        qsTravel: 400 * DP,
        header,
        qs: {
            left, width, trackEnd: left + width - AUTO_SIZE - AUTO_GAP, autoSize: AUTO_SIZE,
            qqsRows: 2, qqsTop: 56 * DP, gridTop: 152 * DP + VOLUME_ROW, brightnessCY: 112 * DP,
            volumeCY: 112 * DP + VOLUME_ROW,
            footerCY: H - 62 * DP, powerCX, settingsCX: powerCX - FOOTER_PITCH,
        },
        panel: {x: columnX, top: 248 * DP, width: columnW, height: H, radius: PANEL_RADIUS, roundBottom: false},
        rows: {top: ROW_INSET_PORT, left, width, end: H, scrollEnd: H - ROW_END_INSET, clipX: columnX, clipW: columnW},
    };
}

function phoneShade(barHeight) {
    const W = WIDTH, H = HEIGHT;
    const landscape = W > H;
    // sideways the upright column, centred
    const columnW = Math.min(W, SCREEN_W);
    const columnX = (W - columnW) / 2, columnEnd = columnX + columnW;
    const left = columnX + PHONE_SIDE, width = columnW - 2 * PHONE_SIDE;
    const qqsTop = barHeight;
    const panelTop = qqsTop + 2 * PHONE_TILE_PITCH - PHONE_TILE_GAP + PHONE_PANEL_GAP;
    const powerCX = columnEnd - PHONE_POWER_END;
    return {
        phone: true,
        landscape,
        width: W,
        height: H,
        travel: PHONE_TRAVEL,
        qsTravel: landscape ? 0 : H - panelTop,
        header: {
            width: W,
            height: barHeight,
            dateGap: PHONE_DATE_GAP,
            // all the quick settings: the big clock's start and the top of its digits, the date's, the carrier's end
            // and top, and the icons' end and centre line
            clockX: left,
            clockInkTop: PHONE_CLOCK_INK_TOP,
            dateX: left,
            dateInkTop: PHONE_DATE_INK_TOP,
            carrierEnd: columnEnd - PHONE_CARRIER_END,
            carrierInkTop: PHONE_CARRIER_INK_TOP,
            iconsEnd: columnEnd - PHONE_ICONS_END,
            iconsCY: PHONE_ICONS_CY,
        },
        qs: {
            left, width, gap: PHONE_TILE_GAP, pitch: PHONE_TILE_PITCH, columns: 2, smallColumns: 4,
            trackEnd: left + width - AUTO_SIZE - AUTO_GAP, autoSize: AUTO_SIZE,
            qqsRows: 2, qqsTop, gridTop: PHONE_GRID_TOP, brightnessCY: PHONE_BRIGHTNESS_CY,
            footerCY: H - PHONE_FOOTER_UP, powerCX, settingsCX: powerCX - PHONE_FOOTER_PITCH,
        },
        panel: {x: columnX, top: panelTop, width: columnW, height: H, radius: PANEL_RADIUS, roundBottom: false},
        rows: {
            top: PHONE_SIDE, left, width, end: H, scrollEnd: H - ROW_END_INSET, clipX: columnX, clipW: columnW,
            // translucent cards over the blurred screen, and the footer's taller buttons
            translucent: true, footerPillY: PHONE_FOOTER_PILL_Y, footerPillH: PHONE_FOOTER_PILL_H,
        },
    };
}
