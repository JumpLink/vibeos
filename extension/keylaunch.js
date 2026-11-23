// What the keys that open an app open (Settings > System > Gestures): a double press of the power key (power.js) and
// the side button (keys.js AppSwitchKey). vibeos.Shell power-double-press-app and side-key-app hold a desktop file id,
// 'recents' (the side button only) or '' for nothing, and are read at each press, so a change in Settings counts at
// once. An app that is not installed counts as nothing for the double press, and as recent apps for the side button,
// the button's job before it could open an app.
//
// An app opens the way Android opens the one a key asks for: the shade and recents close, the screen is on (power.js
// wakes it for the double press), and on a locked phone the bouncer comes up first and the app opens once the phone
// is unlocked, as KeyguardManager.requestDismissKeyguard has it.
//
// Holding the power key (vibeos.Shell power-hold-agent, on by default) talks to Agent, as Android's "press and hold
// for the assistant": Agent listens from the long press until the key is let go, then answers. Agent is told through
// its GApplication action "hold" ((bb): held, locked), which D-Bus activation queues for it while it starts. On an
// unlocked phone its window opens as an app a key opens; on a locked one nothing is unlocked and no window shows:
// Agent listens and answers aloud, and only reads (no alarm set, no call, no message) until the phone is unlocked.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Shell from 'gi://Shell';

const DOUBLE_PRESS_KEY = 'power-double-press-app';
const SIDE_KEY = 'side-key-app';
const HOLD_KEY = 'power-hold-agent';
const RECENTS = 'recents';
const AGENT = 'vibeos.Agent.desktop';
const AGENT_NAME = 'vibeos.Agent';
const AGENT_PATH = '/vibeos/Agent';

export class KeyLaunch {
    // @service: service.js's Service (launchApp, toggleRecents); @keyguard; @power: power.js's Power (interactive);
    // @shade: closed for the app
    constructor({service, keyguard, power, shade}) {
        this._service = service;
        this._keyguard = keyguard;
        this._power = power;
        this._shade = shade;
        const schema = Gio.SettingsSchemaSource.get_default()?.lookup('vibeos.Shell', true);
        this._settings = schema ? new Gio.Settings({settings_schema: schema}) : null;
    }

    // a key's value, or null when the installed schema is older than the key
    _value(key) {
        return this._settings?.settings_schema.has_key(key) ? this._settings.get_string(key) : null;
    }

    // a switch's value, or `fallback` when the installed schema is older than the key
    _flag(key, fallback) {
        return this._settings?.settings_schema.has_key(key) ? this._settings.get_boolean(key) : fallback;
    }

    _app(id) {
        return id ? Shell.AppSystem.get_default().lookup_app(id) : null;
    }

    // power.js: whether a second press of the power key opens an app (the first press's lock then waits for it)
    get doublePressArmed() {
        return !!this._app(this._value(DOUBLE_PRESS_KEY));
    }

    // the power key's second press, the screen already waking
    doublePress() {
        const app = this._app(this._value(DOUBLE_PRESS_KEY));
        if (app)
            this._open(app, 'power key double press');
    }

    // power.js: whether holding the power key talks to Agent (else it opens the power menu)
    get holdArmed() {
        return this._flag(HOLD_KEY, false) && !!this._app(AGENT);
    }

    // the power key held long enough (held), and let go (!held): Agent listens in between
    hold(held) {
        if (held) {
            // the phone as it is at the hold's start: a hold begun locked stays a locked one to its end
            this._holdLocked = this._keyguard.showing;
            log(`vibeos: power key held: Agent listens${this._holdLocked ? ', the phone locked' : ''}`);
            if (!this._holdLocked) {
                if (this._shade?.isOpen)
                    this._shade.close();
                if (this._service.navigation.recentsShown)
                    this._service.navigation.recentsBack();
                this._service.launchApp(this._app(AGENT));
            }
        }
        // after the launch's own activation, on the same connection: Agent gets them in this order
        Gio.DBus.session.call(AGENT_NAME, AGENT_PATH, 'org.freedesktop.Application', 'ActivateAction',
            new GLib.Variant('(sava{sv})', ['hold', [new GLib.Variant('(bb)', [held, !!this._holdLocked])], {}]),
            null, Gio.DBusCallFlags.NONE, -1, null, (bus, res) => {
                try {
                    bus.call_finish(res);
                } catch (e) {
                    log(`vibeos: Agent's hold (${held}): ${e.message}`);
                }
            });
    }

    // a press of the side button: with the screen on only, as before it could open an app
    sideKey() {
        if (!this._power.interactive)
            return;
        const value = this._value(SIDE_KEY) ?? RECENTS;
        if (value === '')
            return;
        const app = value === RECENTS ? null : this._app(value);
        if (app)
            this._open(app, 'side button');
        else if (!this._keyguard.showing)
            this._service.toggleRecents();
    }

    _open(app, why) {
        log(`vibeos: ${why}: ${app.get_id()}${this._keyguard.showing ? ', after the unlock' : ''}`);
        if (this._shade?.isOpen)
            this._shade.close();
        if (this._service.navigation.recentsShown)
            this._service.navigation.recentsBack();
        this._keyguard.dismissThen(() => this._service.launchApp(app));
    }

    destroy() {
        this._settings = null;
    }
}
