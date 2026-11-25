// Bluetooth's questions (bluetooth.js), as a phone asks them: an alert dialog over a dim, for a PIN or passkey to
// type, a code to compare or to type on the other device, a yes or no. A question turns the screen on, as Android's
// pairing request does. While the phone is locked it waits behind an urgent notification and a line on the lock
// screen, and the dialog opens the moment the phone is unlocked (a question is never answered for the user); a lock
// that comes while a dialog is up puts it back behind one. A pairing that BlueZ gave up on unanswered (a car waits
// 30 s for its PIN) leaves a notification once the phone is unlocked, whose tap starts it again from the phone.
//
// ask(question) -> {answer, close(reason), update(entered)}: `answer` resolves to the reply (the PIN typed, the
// passkey as a number, true) or null for no (Cancel, Don't allow, a notification swiped away); close() takes the
// question away unanswered, resolving null (reason 'canceled': BlueZ gave up on it).
//
// The dialog sits in modalDialogGroup, under the on-screen keyboard (the input panel group, which takes input through
// the dialog's grab), and moves up out of the keyboard's way.
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Pango from 'gi://Pango';
import Shell from 'gi://Shell';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as MessageTray from 'resource:///org/gnome/shell/ui/messageTray.js';

import {_} from './l10n.js';
import {animate, bezier} from './motion.js';
import {Color, cogl, font, rgba} from './palette.js';
import {openSettings} from './settings.js';
import {WIDTH, dp, sp} from './units.js';

// Material 3's alert dialog: 24 dp padding, 28 dp corners, 280-560 dp wide, the headline 24 sp, the text 14 sp,
// the buttons text buttons at the end
const MAX_W_DP = 400;
const MARGIN_DP = 24;
const PAD_DP = 24;
const CORNER_DP = 28;
const GAP_DP = 16;
const FIELD_H_DP = 56;
const BUTTON_H_DP = 40;
const BUTTON_PAD_DP = 12;
const BUTTONS_TOP_DP = 24;
const CHECK_DP = 18;
const PIN_MAX = 16;
const PASSKEY_LEN = 6;
const DIM = 0.32;
const ENTER_MS = 250;
const EXIT_MS = 150;
const STANDARD = bezier(0.2, 0, 0, 1);
const STANDARD_ACCELERATE = bezier(0.3, 0, 1, 1);

function title(question) {
    if (question.kind === 'microphone') {
        /* TRANSLATORS: an app records sound for the first time and asks for the microphone; %s is the app's name */
        return _('Allow %s to use the microphone?').replace('%s', question.name);
    }
    return question.kind === 'service'
        /* TRANSLATORS: a Bluetooth device asks to connect to one of the phone's services; %s is its name */
        ? _('Allow %s to connect?').replace('%s', question.name)
        /* TRANSLATORS: a Bluetooth device asks to pair; %s is its name */
        : _('Pair with %s?').replace('%s', question.name);
}

function text(str, sizeSp, wght, colour, width, params = {}) {
    const t = new Clutter.Text({
        text: str, font_name: font(sizeSp, wght), color: cogl(colour),
        line_wrap: true, line_wrap_mode: Pango.WrapMode.WORD_CHAR, ...params,
    });
    if (width)
        t.width = width;
    return t;
}

// One question on the screen. done(answer) once, from a button or close(). The microphone's (micprompt.js) answers
// 'yes' or 'no', and null when it is dismissed
export class Dialog {
    constructor(question, done) {
        this._question = question;
        this._done = done;
        this._grab = null;
        this._anim = null;
        this._closing = false;
        this._entry = null;
        this._alphanumeric = false;

        this.layer = new St.Widget({reactive: true, visible: false});
        this._panel = new St.BoxLayout({
            orientation: Clutter.Orientation.VERTICAL,
            reactive: true,
            style: `background-color: ${rgba(Color.surfaceContainerHigh)}; border-radius: ${dp(CORNER_DP)}px; ` +
                `padding: ${dp(PAD_DP)}px;`,
        });
        this.layer.add_child(this._panel);
        this.layer.connect('key-press-event', (_a, event) => {
            if (event.get_key_symbol() === Clutter.KEY_Escape) {
                this.answer(null);
                return Clutter.EVENT_STOP;
            }
            return Clutter.EVENT_PROPAGATE;
        });
        Main.layoutManager.modalDialogGroup.add_child(this.layer);
        this._build();
        const kb = Main.layoutManager.keyboardBox;
        kb.connectObject('notify::visible', () => this._place(), 'notify::height', () => this._place(), this);
        Main.layoutManager.connectObject('monitors-changed', () => this._place(), this);
    }

    get _innerW() {
        const screenDp = WIDTH / 3;
        return dp(Math.min(MAX_W_DP, screenDp - 2 * MARGIN_DP) - 2 * PAD_DP);
    }

    _build() {
        const q = this._question;
        const w = this._innerW;
        const add = (actor, gapDp = GAP_DP) => {
            if (this._panel.get_n_children())
                actor.margin_top = dp(gapDp);
            this._panel.add_child(actor);
        };
        add(text(title(q), 24, 400, Color.onSurface, w));

        switch (q.kind) {
        case 'pin':
            add(this._field(PIN_MAX));
            add(text(_('Usually 0000 or 1234'), 12, 400, Color.onSurfaceVariant, w), 4);
            add(this._checkbox(_('PIN contains letters or symbols')));
            add(text(_('You may also need to type this PIN on the other device.'), 14, 400,
                Color.onSurfaceVariant, w));
            this._buttons([[_('Cancel'), null], [_('Pair'), () => this._typed(), true]]);
            break;
        case 'passkey':
            /* TRANSLATORS: the passkey (six digits) to type here, as the other device shows it */
            add(text(_('Type the passkey shown on the other device.'), 14, 400, Color.onSurfaceVariant, w));
            add(this._field(PASSKEY_LEN));
            this._buttons([[_('Cancel'), null], [_('Pair'), () => this._typed(), true]]);
            break;
        case 'confirm':
            add(text(_('Bluetooth pairing code'), 14, 400, Color.onSurfaceVariant, w));
            add(this._code(q.code), 8);
            this._buttons([[_('Cancel'), null], [_('Pair'), () => true]]);
            break;
        case 'display':
            /* TRANSLATORS: over the code shown here, for a keyboard (the other device) to type */
            add(text(_('Type this code on the other device, then press Return or Enter.'), 14, 400,
                Color.onSurfaceVariant, w));
            add(this._code(q.code), 8);
            this._buttons([[_('Cancel'), null]]);
            break;
        case 'service':
            this._buttons([[_("Don't allow"), null], [_('Allow'), () => true]]);
            break;
        case 'microphone':
            this._buttons([[_("Don't allow"), () => 'no'], [_('Allow'), () => 'yes']]);
            break;
        default:        // authorize: a device pairing without a code
            this._buttons([[_('Cancel'), null], [_('Pair'), () => true]]);
        }
    }

    _code(code) {
        const t = text(code, 36, 500, Color.onSurface, this._innerW, {line_alignment: Pango.Alignment.CENTER});
        t.x_align = Clutter.ActorAlign.CENTER;
        return t;
    }

    _field(max) {
        const entry = new St.Entry({
            can_focus: true,
            x_expand: true,
            height: dp(FIELD_H_DP),
            style: `border: ${dp(2)}px solid ${rgba(Color.primary)}; border-radius: ${dp(4)}px; ` +
                `padding: 0 ${dp(16)}px; background-color: transparent; color: ${rgba(Color.onSurface)}; ` +
                `caret-color: ${rgba(Color.primary)}; font-size: ${sp(16)}px; font-family: "Google Sans Flex";`,
        });
        const ct = entry.clutter_text;
        ct.max_length = max;
        ct.single_line_mode = true;
        ct.input_purpose = Clutter.InputContentPurpose.DIGITS;
        ct.connect('text-changed', () => {
            const t = ct.text;
            const kept = this._alphanumeric ? t : t.replace(/[^0-9]/g, '');
            if (kept !== t) {
                ct.text = kept;
                return;
            }
            this._syncDefault();
        });
        ct.connect('activate', () => {
            if (this._valid())
                this.answer(this._typed());
        });
        this._entry = entry;
        this._entryMax = max;
        return entry;
    }

    _checkbox(label) {
        const row = new St.BoxLayout({style: `spacing: ${dp(16)}px;`});
        const box = new St.Widget({width: dp(CHECK_DP), height: dp(CHECK_DP), y_align: Clutter.ActorAlign.CENTER,
            layout_manager: new Clutter.BinLayout()});
        const tick = text('\u2713', CHECK_DP - 4, 700, Color.onPrimary, 0, {line_wrap: false,
            x_align: Clutter.ActorAlign.CENTER, y_align: Clutter.ActorAlign.CENTER});
        box.add_child(tick);
        const button = new St.Button({can_focus: true, button_mask: St.ButtonMask.PRIMARY, accessible_name: label,
            toggle_mode: true, child: row});
        row.add_child(box);
        row.add_child(text(label, 14, 400, Color.onSurface, this._innerW - dp(CHECK_DP + 16)));
        const sync = () => {
            const on = button.checked;
            tick.visible = on;
            box.style = on
                ? `background-color: ${rgba(Color.primary)}; border-radius: ${dp(2)}px;`
                : `border: ${dp(2)}px solid ${rgba(Color.onSurfaceVariant)}; border-radius: ${dp(2)}px;`;
        };
        button.connect('notify::checked', () => {
            sync();
            this._alphanumeric = button.checked;
            const ct = this._entry?.clutter_text;
            if (ct) {
                ct.input_purpose = this._alphanumeric ? Clutter.InputContentPurpose.NORMAL
                    : Clutter.InputContentPurpose.DIGITS;
                ct.text = this._alphanumeric ? ct.text : ct.text.replace(/[^0-9]/g, '');
                global.stage.set_key_focus(ct);
            }
        });
        sync();
        return button;
    }

    // [label, what the answer is (null: no), default]
    _buttons(list) {
        const row = new St.BoxLayout({x_align: Clutter.ActorAlign.END, style: `spacing: ${dp(8)}px;`});
        row.margin_top = dp(BUTTONS_TOP_DP);
        for (const [label, value, isDefault] of list) {
            // (in a bin: St.Button sets a Clutter.Text child's font and colour from its own style)
            const t = new St.Bin({child: text(label, 14, 500, Color.primary, 0, {line_wrap: false}),
                y_align: Clutter.ActorAlign.CENTER});
            const button = new St.Button({
                can_focus: true, button_mask: St.ButtonMask.PRIMARY, accessible_name: label, child: t,
                height: dp(BUTTON_H_DP),
                style: `padding: 0 ${dp(BUTTON_PAD_DP)}px; border-radius: ${dp(BUTTON_H_DP / 2)}px;`,
            });
            button.connect('clicked', () => this.answer(value ? value() : null));
            if (isDefault)
                this._default = button;
            row.add_child(button);
        }
        this._panel.add_child(row);
        this._syncDefault();
    }

    _typed() {
        const t = this._entry?.clutter_text.text ?? '';
        return this._question.kind === 'passkey' ? Number(t) : t;
    }

    _valid() {
        const t = this._entry?.clutter_text.text ?? '';
        return this._question.kind === 'passkey' ? t.length === PASSKEY_LEN && /^[0-9]+$/.test(t) : t.length > 0;
    }

    _syncDefault() {
        if (!this._default || !this._entry)
            return;
        const ok = this._valid();
        this._default.reactive = ok;
        this._default.opacity = ok ? 255 : 97;
    }

    // centred on the screen, or in what the keyboard leaves of it
    _place() {
        const monitor = Main.layoutManager.primaryMonitor;
        if (!monitor)
            return;
        this.layer.set_position(monitor.x, monitor.y);
        this.layer.set_size(monitor.width, monitor.height);
        const w = this._innerW + 2 * dp(PAD_DP);
        this._panel.width = w;
        const [, h] = this._panel.get_preferred_height(w);
        const kb = Main.layoutManager.keyboardBox;
        const free = kb.visible && kb.height > 0 ? Math.max(monitor.height - kb.height, h + 2 * dp(MARGIN_DP))
            : monitor.height;
        this._panel.set_position(Math.round((monitor.width - w) / 2),
            Math.max(dp(MARGIN_DP), Math.round((free - h) / 2)));
    }

    get showing() {
        return this.layer.visible && !this._closing;
    }

    show() {
        if (this.layer.visible)
            return;
        this._closing = false;
        this._place();
        this.layer.show();
        this._grab = Main.pushModal(this.layer, {actionMode: Shell.ActionMode.SYSTEM_MODAL});
        global.stage.set_key_focus(this._entry?.clutter_text ?? this._default ?? this.layer);
        this._animate(true);
    }

    // off the screen for now (the phone locked), the question still open
    hide() {
        if (!this.layer.visible)
            return;
        this._anim?.stop();
        this._anim = null;
        this.layer.hide();
        if (this._grab)
            Main.popModal(this._grab);
        this._grab = null;
    }

    answer(value) {
        if (this._closing)
            return;
        const done = this._done;
        this._done = null;
        done?.(value);
        this.close();
    }

    close() {
        if (this._closing)
            return;
        this._closing = true;
        if (!this.layer.visible) {
            this.destroy();
            return;
        }
        this._animate(false, () => this.destroy());
    }

    _animate(enter, onDone = null) {
        this._anim?.stop();
        this._anim = animate(this.layer, {
            from: 0, to: 1,
            duration: enter ? ENTER_MS : EXIT_MS,
            interpolator: enter ? STANDARD : STANDARD_ACCELERATE,
            onUpdate: p => {
                const alpha = enter ? p : 1 - p;
                this._panel.opacity = Math.round(alpha * 255);
                const s = enter ? 0.9 + 0.1 * p : 1;
                this._panel.set_pivot_point(0.5, 0.5);
                this._panel.set_scale(s, s);
                this.layer.background_color = cogl(Color.black, DIM * alpha);
            },
            onDone: () => {
                this._anim = null;
                onDone?.();
            },
        });
    }

    destroy() {
        this.hide();
        Main.layoutManager.keyboardBox.disconnectObject(this);
        Main.layoutManager.disconnectObject(this);
        this.layer.destroy();
        const done = this._done;
        this._done = null;
        done?.(null);
    }
}

// how long a pairing BlueZ gave up on can be started again from its notification (a car stays in its pairing mode
// for a few minutes)
const RETRY_KEEP_S = 300;

function notice(question) {
    return (question.kind === 'service'
        /* TRANSLATORS: the lock screen's line while a Bluetooth device waits to connect; %s is its name */
        ? _('Unlock to let %s connect')
        /* TRANSLATORS: the lock screen's line while a Bluetooth device waits to pair; %s is its name */
        : _('Unlock to pair with %s'))
        .replace('%s', question.name);
}

function failed(question) {
    /* TRANSLATORS: a notification's title when a pairing timed out; %s is the device's name */
    return _("Couldn't pair with %s").replace('%s', question.name);
}

// keyguard: keyguard.js's Keyguard ('changed', showing, dismissThen(), setNotice()); power: power.js's Power, set once
// it exists, which turns the screen on for a question
export class PairingUI {
    constructor(keyguard) {
        this._keyguard = keyguard;
        this.power = null;
        this._current = null;       // {question, dialog, notification, resolve, answered}
        this._retry = null;         // a pairing BlueZ gave up on, unanswered: {question, notification, timer}
        this._source = null;
        this._notice = null;
        keyguard.connectObject('changed', () => this._sync(), this);
    }

    get showing() {
        return !!this._current?.dialog?.showing;
    }

    ask(question) {
        this._finish(this._current, null);
        this._dropRetry();
        let resolve;
        const answer = new Promise(r => (resolve = r));
        const current = {question, dialog: null, notification: null, resolve, answered: false};
        this._current = current;
        this._sync();
        // as Android's pairing request: the screen comes on for it (a car asks once, and waits 30 s at most)
        this.power?.wakeUp('a Bluetooth request');
        return {
            answer,
            close: reason => this._close(current, reason),
            update: () => {},
        };
    }

    // BlueZ took the question away. One that it gave up on before anyone answered (the other device's time ran out,
    // the phone locked all along) can be started again: a notification does it, posted once the phone is unlocked so
    // that it shows then
    _close(current, reason) {
        const open = this._current === current && !current.answered;
        this._finish(current, null);
        if (open && reason === 'canceled' && current.question.retry)
            this._offerRetry(current.question);
    }

    // locked: the question behind a notification and a line on the lock screen; unlocked: the dialog, and a pairing
    // to start again in a notification
    _sync() {
        const locked = this._keyguard.showing;
        const current = this._current;
        if (current) {
            if (locked) {
                current.dialog?.hide();
                this._notify(current);
            } else {
                this._unnotify(current);
                if (!current.dialog)
                    current.dialog = new Dialog(current.question, value => this._finish(current, value));
                current.dialog.show();
            }
        }
        if (this._retry && !locked && !this._retry.notification)
            this._notifyRetry(this._retry);
        this._syncNotice();
    }

    _syncNotice() {
        let text = null;
        if (this._keyguard.showing) {
            if (this._current)
                text = notice(this._current.question);
            else if (this._retry)
                text = failed(this._retry.question);
        }
        if (text === this._notice)
            return;
        this._notice = text;
        this._keyguard.setNotice?.(text);
    }

    _finish(current, value) {
        if (!current || current.answered)
            return;
        current.answered = true;
        if (this._current === current)
            this._current = null;
        this._unnotify(current);
        const dialog = current.dialog;
        current.dialog = null;
        dialog?.close();
        current.resolve(value);
        this._syncNotice();
    }

    _ensureSource() {
        if (!this._source) {
            this._source = new MessageTray.Source({title: _('Bluetooth'),
                icon: new Gio.ThemedIcon({name: 'bluetooth-active-symbolic'})});
            this._source.connect('destroy', () => (this._source = null));
            Main.messageTray.add(this._source);
        }
        return this._source;
    }

    _notify(current) {
        if (current.notification)
            return;
        const source = this._ensureSource();
        // urgent: past Do Not Disturb, and its icon on the lock screen
        const notification = new MessageTray.Notification({
            source,
            title: title(current.question),
            /* TRANSLATORS: a Bluetooth device's question waits behind this notification while the phone is locked */
            body: _('Unlock the phone to answer.'),
            urgency: MessageTray.Urgency.CRITICAL,
        });
        current.notification = notification;
        // swiped away: no (a tap unlocks, and the dialog opens then)
        let tapped = false;
        notification.connect('activated', () => {
            tapped = true;
            this._keyguard.dismissThen(() => this._sync());
        });
        notification.connect('destroy', (_n, reason) => {
            if (current.notification !== notification)
                return;
            current.notification = null;
            if (!tapped && reason === MessageTray.NotificationDestroyedReason.DISMISSED)
                this._finish(current, null);
        });
        source.addNotification(notification);
    }

    _unnotify(current) {
        const notification = current.notification;
        current.notification = null;
        notification?.destroy(MessageTray.NotificationDestroyedReason.SOURCE_CLOSED);
    }

    _offerRetry(question) {
        this._dropRetry();
        const retry = {question, notification: null, timer: 0};
        retry.timer = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, RETRY_KEEP_S, () => {
            retry.timer = 0;
            this._dropRetry(retry);
            return GLib.SOURCE_REMOVE;
        });
        this._retry = retry;
        this._sync();
    }

    // a tap: the pairing again from the phone (its question comes up at once), or, the device forgotten by then,
    // Settings' Bluetooth page, where it is paired anew
    _notifyRetry(retry) {
        const source = this._ensureSource();
        const notification = new MessageTray.Notification({
            source,
            title: failed(retry.question),
            /* TRANSLATORS: under "Couldn't pair with …": a tap starts the pairing again */
            body: _('Tap to try again.'),
            urgency: MessageTray.Urgency.HIGH,
        });
        retry.notification = notification;
        notification.connect('activated', () => {
            // (the tray takes the notification away itself)
            retry.notification = null;
            this._dropRetry(retry);
            this._keyguard.dismissThen(() => {
                let started = false;
                try {
                    started = retry.question.retry();
                } catch (e) {
                    logError(e, 'vibeos: Bluetooth pairing again');
                }
                if (!started)
                    openSettings('bluetooth');
            });
        });
        notification.connect('destroy', () => {
            if (retry.notification !== notification)
                return;
            retry.notification = null;
            this._dropRetry(retry);
        });
        source.addNotification(notification);
    }

    _dropRetry(retry = this._retry) {
        if (!retry || this._retry !== retry)
            return;
        this._retry = null;
        if (retry.timer)
            GLib.source_remove(retry.timer);
        retry.timer = 0;
        const notification = retry.notification;
        retry.notification = null;
        notification?.destroy(MessageTray.NotificationDestroyedReason.SOURCE_CLOSED);
        this._syncNotice();
    }

    destroy() {
        this._keyguard.disconnectObject(this);
        this._finish(this._current, null);
        this._dropRetry();
        this._keyguard.setNotice?.(null);
        this._source?.destroy();
        this._source = null;
    }
}
