// The on-screen keyboard's keys tick under the finger as it lands, as Gboard's do: feedbackd's shortest effect, the
// one the back gesture uses.
//
// The shell's Key class is not exported, so its press() is patched on the prototype of the first key a layout gets:
// Keyboard._addRowKeys builds every layout's keys (letters, shift, backspace, enter, the space bar), and a keyboard
// that already exists has its current layout looked through at once.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import * as Keyboard from 'resource:///org/gnome/shell/ui/keyboard.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';

function tick() {
    Gio.DBus.session.call('org.sigxcpu.Feedback', '/org/sigxcpu/Feedback', 'org.sigxcpu.Feedback',
        'TriggerFeedback', new GLib.Variant('(ssa{sv}i)', ['vibeos.Shell', 'button-released', {}, -1]),
        null, Gio.DBusCallFlags.NONE, -1, null, null);
}

function findKey(actor) {
    if (!actor)
        return null;
    if (typeof actor.press === 'function' && actor.keyButton)
        return actor;
    for (const child of actor.get_children()) {
        const key = findKey(child);
        if (key)
            return key;
    }
    return null;
}

export class KeyHaptics {
    constructor() {
        const proto = Keyboard.Keyboard.prototype;
        const addRowKeys = proto._addRowKeys;
        this._addRowKeys = addRowKeys;
        const patch = actor => this._patch(actor);
        proto._addRowKeys = function (keys, layout, ...rest) {
            const result = addRowKeys.call(this, keys, layout, ...rest);
            patch(layout);
            return result;
        };
        this._patch(Main.keyboard._keyboard);
    }

    _patch(actor) {
        if (this._keyProto)
            return;
        const key = findKey(actor);
        if (!key)
            return;
        this._keyProto = Object.getPrototypeOf(key);
        const press = this._keyProto.press;
        this._press = press;
        this._keyProto.press = function () {
            tick();
            return press.call(this);
        };
    }

    destroy() {
        Keyboard.Keyboard.prototype._addRowKeys = this._addRowKeys;
        if (this._keyProto)
            this._keyProto.press = this._press;
        this._keyProto = null;
    }
}
