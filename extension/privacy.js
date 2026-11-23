// Settings' Microphone and Camera switches (Security and privacy) for every app: org.gnome.desktop.privacy
// disable-microphone and disable-camera, mirrored into the permission store ("vibeos-privacy"/"microphone" and
// "vibeos-privacy"/"camera", app "all": "blocked" or "allowed"), which vibeos-sandbox's WirePlumber script
// (sandbox-access.lua) watches: while one is blocked, no PipeWire client but PipeWire's and WirePlumber's own sees a
// microphone (or a camera) or gets linked to one, and the links one had are taken away. The camera portal refuses by
// itself (its lockdown reads disable-camera). The store keeps the last state across a restart, and this writes it
// again whenever the shell starts or a switch changes. The status bar shows an indicator while either is blocked
// (statusbar.js, from SystemState's micOff and cameraOff).
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

const STORE = 'org.freedesktop.impl.portal.PermissionStore';
const STORE_PATH = '/org/freedesktop/impl/portal/PermissionStore';
const TABLE = 'vibeos-privacy';
// the key each entry follows
const ENTRIES = {microphone: 'disable-microphone', camera: 'disable-camera'};

export class PrivacySwitches {
    // @changed: called after either switch changed
    constructor(changed) {
        this._changed = changed;
        this._cancellable = new Gio.Cancellable();
        const schema = Gio.SettingsSchemaSource.get_default()?.lookup('org.gnome.desktop.privacy', true);
        this._settings = schema ? new Gio.Settings({settings_schema: schema}) : null;
        this._keys = Object.entries(ENTRIES).filter(([, key]) => schema?.has_key(key));
        for (const [entry, key] of this._keys) {
            this._settings.connectObject(`changed::${key}`, () => {
                this._mirror(entry, key);
                this._changed();
            }, this);
            this._mirror(entry, key);
        }
    }

    get micOff() {
        return this._blocked('microphone');
    }

    get cameraOff() {
        return this._blocked('camera');
    }

    _blocked(entry) {
        const key = ENTRIES[entry];
        return this._keys.some(([e]) => e === entry) && this._settings.get_boolean(key);
    }

    _mirror(entry, key) {
        const value = this._settings.get_boolean(key) ? 'blocked' : 'allowed';
        Gio.DBus.session.call(STORE, STORE_PATH, STORE, 'SetPermission',
            new GLib.Variant('(sbssas)', [TABLE, true, entry, 'all', [value]]), null,
            Gio.DBusCallFlags.NONE, -1, this._cancellable, (bus, res) => {
                try {
                    bus.call_finish(res);
                } catch (e) {
                    if (!e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                        logError(e, `vibeos: the ${entry} switch into the permission store`);
                }
            });
    }

    destroy() {
        // the store keeps the state: the switches go on holding without the shell
        this._cancellable.cancel();
        this._settings?.disconnectObject(this);
    }
}
