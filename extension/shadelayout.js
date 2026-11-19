// Where the parts of a tablet's shade go (a smallest width of 600 dp or more), for the screen as it is turned now.
// Turned sideways the shade is split: the quick settings fill the left half (the brightness row, every tile, the
// footer at the bottom) and the notifications sit in a rounded panel on the right half, under a header row that spans
// the screen; a second pull adds nothing. Upright it is one centred column 640 dp wide: two rows of tiles over the
// notification panel, and the rest of the quick settings on a second pull, which takes the panel down off the screen.
// Screen px (3 to a dp); null on a phone.
import {HEIGHT, TABLET, WIDTH} from './units.js';

const DP = 3;
const COLUMN_W = 640 * DP;              // upright: the shade's column
const COLUMN_MARGIN = 16 * DP;          // ... and its least margin on a narrow tablet
const PANEL_RADIUS = 32 * DP;
const ROW_INSET_LAND = 16 * DP;         // the cards' inset in the notification panel, at its top and its sides
const ROW_INSET_PORT = 40 * DP;
const ROW_END_INSET = 16 * DP;          // the stack scrolls to end this far above the panel's bottom
const AUTO_SIZE = 52 * DP;              // the auto-brightness button
const AUTO_GAP = 13 * DP;               // between the track and the button
const FOOTER_PITCH = 48 * DP;           // between the footer's buttons

export function tabletShade() {
    if (!TABLET)
        return null;
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
                qqsRows: 0, qqsTop: 160 * DP, gridTop: 160 * DP, brightnessCY: 120 * DP,
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
            qqsRows: 2, qqsTop: 56 * DP, gridTop: 152 * DP, brightnessCY: 112 * DP,
            footerCY: H - 62 * DP, powerCX, settingsCX: powerCX - FOOTER_PITCH,
        },
        panel: {x: columnX, top: 248 * DP, width: columnW, height: H, radius: PANEL_RADIUS, roundBottom: false},
        rows: {top: ROW_INSET_PORT, left, width, end: H, scrollEnd: H - ROW_END_INSET, clipX: columnX, clipW: columnW},
    };
}
