// The microphone's question for an app in vibeos-sandbox (sandbox/data/sandbox-access.lua). Sound has no portal to
// ask through, as the camera and location have: the first time a sandboxed app records and has no answer stored,
// WirePlumber's script holds its capture stream and puts a name on the session bus,
// org.freedesktop.ReserveDevice1.VibeOSMicAsk<the app id in hex>, for as long as the question is open. This watches
// for such names and asks "Allow <app> to use the microphone?" in the dialog Bluetooth's questions use. The answer is
// written to the permission store, where the script follows it at once:
//   Allow         "vibeos-sandbox"/"microphone", the app: "yes"
//   Don't allow   the same: "no"
//   dismissed     "vibeos-sandbox-ask"/"microphone", the app: "dismissed" (the back gesture, 30 s without an answer,
//                 the phone locked or locking): the stream fails this once and nothing is remembered
// The reply entry is taken away again once the question is over (the name gone). The name, not this, ends a
// question: when the script's own time runs out, or the app goes, the dialog closes with no answer written.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import {Dialog} from './pairdialog.js';

const NAME_PREFIX = 'org.freedesktop.ReserveDevice1.VibeOSMicAsk';
const STORE = 'org.freedesktop.impl.portal.PermissionStore';
const STORE_PATH = '/org/freedesktop/impl/portal/PermissionStore';
const ANSWER_TABLE = 'vibeos-sandbox';
const REPLY_TABLE = 'vibeos-sandbox-ask';
const ID = 'microphone';
// a little under the script's own 35 s
const TIMEOUT_S = 30;

// the app id a question's bus name carries, or null
export function appOfName(name) {
    if (!name?.startsWith(NAME_PREFIX))
        return null;
    const hex = name.slice(NAME_PREFIX.length);
    if (!hex || hex.length % 2 || !/^[0-9a-f]+$/.test(hex))
        return null;
    const bytes = new Uint8Array(hex.length / 2);
    for (let i = 0; i < bytes.length; i++)
        bytes[i] = parseInt(hex.slice(2 * i, 2 * i + 2), 16);
    try {
        return new TextDecoder('utf-8', {fatal: true}).decode(bytes);
    } catch (e) {
        return null;
    }
}

// the name the app is known by: its desktop file's (vibeos-sandbox writes it as <app id>.desktop), or its id
function appName(appId) {
    return Gio.DesktopAppInfo.new(`${appId}.desktop`)?.get_name() || appId;
}

export class MicrophonePrompts {
    // keyguard: keyguard.js's Keyguard ('changed', showing); bus: the session bus; timeoutS: how long a question
    // waits (a test gives its own)
    constructor(keyguard, bus = Gio.DBus.session, timeoutS = TIMEOUT_S) {
        this._keyguard = keyguard;
        this._bus = bus;
        this._timeoutS = timeoutS;
        this._cancellable = new Gio.Cancellable();
        this._open = new Map();     // app id -> {appId, dialog, timer, done}
        keyguard.connectObject('changed', () => this._sync(), this);
        this._subscription = bus.signal_subscribe('org.freedesktop.DBus', 'org.freedesktop.DBus', 'NameOwnerChanged',
            '/org/freedesktop/DBus', null, Gio.DBusSignalFlags.NONE,
            (_c, _s, _p, _i, _n, params) => {
                const [name, , owner] = params.deepUnpack();
                if (name.startsWith(NAME_PREFIX))
                    owner ? this._appeared(name) : this._vanished(name);
            });
        // the questions already open (the shell started after the script)
        bus.call('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus', 'ListNames', null, null,
            Gio.DBusCallFlags.NONE, -1, this._cancellable, (b, res) => {
                try {
                    const [names] = b.call_finish(res).deepUnpack();
                    names.forEach(n => n.startsWith(NAME_PREFIX) && this._appeared(n));
                } catch (e) {
                    if (!e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                        logError(e, 'vibeos: the microphone questions');
                }
            });
    }

    get showing() {
        return [...this._open.values()].some(q => q.dialog?.showing);
    }

    _appeared(name) {
        const appId = appOfName(name);
        if (!appId || this._open.has(appId))
            return;
        const question = {appId, dialog: null, timer: 0, done: false};
        this._open.set(appId, question);
        // an answer left over from the last time is not this question's
        this._store('DeletePermission', '(sss)', [REPLY_TABLE, ID, appId], true);
        // the phone is locked: not asked, and not answered for the user
        if (this._keyguard.showing) {
            this._answered(question, null);
            return;
        }
        question.dialog = new Dialog({kind: 'microphone', name: appName(appId)},
            value => this._answered(question, value));
        question.timer = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, this._timeoutS, () => {
            question.timer = 0;
            this._answered(question, null);
            return GLib.SOURCE_REMOVE;
        });
        question.dialog.show();
    }

    // the script took the question away: the dialog goes without an answer, and the reply entry with it
    _vanished(name) {
        const appId = appOfName(name);
        const question = appId ? this._open.get(appId) : null;
        if (!question)
            return;
        this._open.delete(appId);
        this._end(question);
        this._store('DeletePermission', '(sss)', [REPLY_TABLE, ID, appId], true);
    }

    // the phone locked with a question open
    _sync() {
        if (!this._keyguard.showing)
            return;
        for (const question of [...this._open.values()])
            this._answered(question, null);
    }

    // the user's answer ('yes', 'no'), or null for none: written once
    _answered(question, value) {
        if (question.done)
            return;
        this._end(question);
        if (value === 'yes' || value === 'no')
            this._store('SetPermission', '(sbssas)', [ANSWER_TABLE, true, ID, question.appId, [value]]);
        else
            this._store('SetPermission', '(sbssas)', [REPLY_TABLE, true, ID, question.appId, ['dismissed']]);
    }

    // the dialog and the timer away; nothing more is written for this question
    _end(question) {
        question.done = true;
        if (question.timer)
            GLib.source_remove(question.timer);
        question.timer = 0;
        const dialog = question.dialog;
        question.dialog = null;
        dialog?.close();
    }

    _store(method, signature, args, quiet = false) {
        this._bus.call(STORE, STORE_PATH, STORE, method, new GLib.Variant(signature, args), null,
            Gio.DBusCallFlags.NONE, -1, this._cancellable, (b, res) => {
                try {
                    b.call_finish(res);
                } catch (e) {
                    if (!quiet && !e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                        logError(e, `vibeos: the microphone answer into the permission store (${method})`);
                }
            });
    }

    destroy() {
        this._keyguard.disconnectObject(this);
        if (this._subscription)
            this._bus.signal_unsubscribe(this._subscription);
        this._subscription = 0;
        for (const question of this._open.values())
            this._end(question);
        this._open.clear();
        this._cancellable.cancel();
    }
}
