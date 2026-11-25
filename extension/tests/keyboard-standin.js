// What the VibeOS shell has and a stock one has not, for the keyboard tests (which then run on a stock shell too): the
// keyboard's bottom panel box (which the extension's keyboard takes away) and its map of popovers; mutter's
// is_alien; and the keyboard's own fit of a window above the keys, as the VibeOS shell does it (keyboard.js
// _animateWindow): the focused window, maximized, is unmaximized and sized from the top of the work area down to the
// keys, and maximized again when they go. Nothing is changed in the VibeOS shell itself.
import Meta from 'gi://Meta';
import St from 'gi://St';

import * as KeyboardUI from 'resource:///org/gnome/shell/ui/keyboard.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';

export function standInForTheMobileShell() {
    if (Meta.Window.prototype.is_alien)
        return;
    Meta.Window.prototype.is_alien = () => false;
    const proto = KeyboardUI.Keyboard.prototype;
    for (const [name, make] of [['_bottomPanelBox', () => new St.Bin()], ['_focusPopovers', () => new Map()]]) {
        Object.defineProperty(proto, name, {
            configurable: true,
            get() {
                return this[`__${name}`] ??= make();
            },
            set(value) {
                this[`__${name}`] = value;
            },
        });
    }
    proto._animateWindow = function (window, show) {
        if (show && window.maximized_vertically && !window.__keyboardMoved) {
            const area = window.get_work_area_current_monitor();
            const monitor = Main.layoutManager.keyboardMonitor;
            const height = monitor.y + monitor.height - this.allocation.get_height() - area.y;
            window.unmaximize();
            window.move_resize_frame(false, area.x, area.y, area.width, height);
            window.__keyboardMoved = true;
        } else if (!show && window.__keyboardMoved) {
            window.maximize();
            delete window.__keyboardMoved;
        }
    };
}

