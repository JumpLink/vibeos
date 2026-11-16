// St gives the last visible child of an St.Widget the :last-child pseudo class, and a pseudo class that comes or goes
// restyles every actor inside the child: each St.DrawingArea in it draws its picture again, a new texture each, in the
// frame it happens in. Main.layoutManager.uiGroup is an St.Widget, and the shade and the volume dialog, on top of it,
// were its last visible child from when they showed to when they hid: every icon in the shade drew again as it opened
// and as it closed (2026-11-04). In the shade, the QS footer, there only while QS is open, took :last-child from the
// last tile and gave it back. An end mark is an empty actor kept visible after them, so none of them is ever last.
import Clutter from 'gi://Clutter';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

// An end mark after @parent's children; a child added later goes after it, so it is added last
export function addEndMark(parent) {
    const mark = new Clutter.Actor({name: 'vibeosEndMark'});
    parent.add_child(mark);
    return mark;
}

let uiMark = null;

// uiGroup's end mark, over the shade and the volume dialog
export class EndMark {
    constructor() {
        uiMark = addEndMark(Main.layoutManager.uiGroup);
    }

    destroy() {
        uiMark.destroy();
        uiMark = null;
    }
}

// Raise @actor over everything in uiGroup but its end mark
export function raiseToTop(actor) {
    const group = Main.layoutManager.uiGroup;
    if (!uiMark) {
        group.set_child_above_sibling(actor, null);
        return;
    }
    group.set_child_above_sibling(uiMark, null);
    group.set_child_below_sibling(actor, uiMark);
}
