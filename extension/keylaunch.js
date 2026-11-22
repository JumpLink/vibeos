// What the keys that open an app open (Settings > System > Gestures): a double press of the power key (power.js) and
// the side button (keys.js AppSwitchKey). vibeos.Shell power-double-press-app and side-key-app hold a desktop file id,
// 'recents' (the side button only) or '' for nothing, and are read at each press, so a change in Settings counts at
// once. An app that is not installed counts as nothing for the double press, and as recent apps for the side button,
// the button's job before it could open an app.
//
// An app opens the way Android opens the one a key asks for: the shade and recents close, the screen is on (power.js
// wakes it for the double press), and on a locked phone the bouncer comes up first and the app opens once the phone
// is unlocked, as KeyguardManager.requestDismissKeyguard has it.
import Gio from 'gi://Gio';
import Shell from 'gi://Shell';

const DOUBLE_PRESS_KEY = 'power-double-press-app';
const SIDE_KEY = 'side-key-app';
const RECENTS = 'recents';

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
