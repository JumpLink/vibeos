// What back means in a GTK app, asked of its accessibility tree: Android lets an app say whether it handles back
// itself; a GNOME app says it with a Back button in its header bar. The tree is walked a level at a time, every
// node of a level asked at once over the accessibility bus (nothing here blocks the compositor), and only into what
// reaches the top band of the window, where header bars are.
//
//   {kind: 'button', name, path}   a Back button is showing: back is the app's (click it)
//   {kind: 'dialog'}               a dialog is showing in the window: back closes it (Escape)
//   {kind: 'none'}                 nothing to go back to: back leaves the app (back to home)
//   {kind: 'unknown'}              not an accessible app, or no answer
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

Gio._promisify(Gio.DBusConnection.prototype, 'call');
Gio._promisify(Gio.DBusConnection, 'new_for_address');

const ATSPI = 'org.a11y.atspi';
const ROOT = '/org/a11y/atspi/accessible/root';
const COORD_WINDOW = 1;
const BAND = 40;                        // logical px from the window's top: the header bar
const MAX_DEPTH = 40;
const TIMEOUT_MS = 1000;
// AtspiStateType bits
const ACTIVE = 1, SENSITIVE = 24, SHOWING = 25, VISIBLE = 30;
const DIALOG_ROLES = new Set(['dialog', 'alert', 'file chooser']);

const has = (bits, bit) => bit < 32 ? (bits[0] & (1 << bit)) !== 0 : (bits[1] & (1 << (bit - 32))) !== 0;

export class BackProbe {
    constructor() {
        this._conn = null;
        this._connecting = null;
        this._pids = new Map();             // bus name → pid
        this._names = new Set([GLib.dgettext('libadwaita', 'Back'), GLib.dgettext('gtk40', 'Back'), 'Back']);
        this._generation = 0;
    }

    async _connection() {
        if (this._conn && !this._conn.is_closed())
            return this._conn;
        this._connecting ??= (async () => {
            const [address] = (await Gio.DBus.session.call('org.a11y.Bus', '/org/a11y/bus', 'org.a11y.Bus',
                'GetAddress', null, new GLib.VariantType('(s)'), Gio.DBusCallFlags.NONE, TIMEOUT_MS, null))
                .deepUnpack();
            this._conn = await Gio.DBusConnection.new_for_address(address,
                Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
                null, null);
            return this._conn;
        })().finally(() => (this._connecting = null));
        return this._connecting;
    }

    _call(name, path, iface, method, args, type) {
        return this._conn.call(name, path, iface, method, args, type ? new GLib.VariantType(type) : null,
            Gio.DBusCallFlags.NO_AUTO_START, TIMEOUT_MS, null).then(r => r.deepUnpack());
    }

    // the app's bus name on the accessibility bus, from its pid
    async _app(pid) {
        for (const [name, p] of this._pids) {
            if (p === pid)
                return name;
        }
        const [apps] = await this._call('org.a11y.atspi.Registry', ROOT, `${ATSPI}.Accessible`, 'GetChildren', null,
            '(a(so))');
        const unknown = apps.map(([name]) => name).filter(name => !this._pids.has(name));
        await Promise.all(unknown.map(name => this._call('org.freedesktop.DBus', '/org/freedesktop/DBus',
            'org.freedesktop.DBus', 'GetConnectionUnixProcessID', new GLib.Variant('(s)', [name]), '(u)')
            .then(([p]) => this._pids.set(name, p)).catch(() => {})));
        const live = new Set(apps.map(([name]) => name));
        for (const name of [...this._pids.keys()]) {
            if (!live.has(name))
                this._pids.delete(name);
        }
        for (const [name, p] of this._pids) {
            if (p === pid)
                return name;
        }
        return null;
    }

    async _node(name, path) {
        const acc = `${ATSPI}.Accessible`;
        const [[state], [role], [children], extents] = await Promise.all([
            this._call(name, path, acc, 'GetState', null, '(au)'),
            this._call(name, path, acc, 'GetRoleName', null, '(s)'),
            this._call(name, path, acc, 'GetChildren', null, '(a(so))'),
            this._call(name, path, `${ATSPI}.Component`, 'GetExtents', new GLib.Variant('(u)', [COORD_WINDOW]),
                '((iiii))').then(([e]) => e).catch(() => null),
        ]);
        return {path, state, role, children: children.map(([, p]) => p), extents};
    }

    // What back means in the app with this pid's active window now
    async probe(pid) {
        const generation = ++this._generation;
        try {
            await this._connection();
            const name = await this._app(pid);
            if (!name)
                return {kind: 'unknown'};
            const root = await this._node(name, ROOT);
            let toplevels = await Promise.all(root.children.map(p => this._node(name, p)));
            toplevels = toplevels.filter(n => has(n.state, SHOWING) || has(n.state, VISIBLE));
            const active = toplevels.filter(n => has(n.state, ACTIVE));
            let level = (active.length ? active : toplevels).flatMap(n => n.children);
            let found = null, dialog = false;
            for (let depth = 0; level.length && depth < MAX_DEPTH && generation === this._generation; depth++) {
                const nodes = await Promise.all(level.map(p => this._node(name, p).catch(() => null)));
                const next = [];
                for (const n of nodes) {
                    if (!n || !has(n.state, SHOWING) || !has(n.state, VISIBLE))
                        continue;
                    if (DIALOG_ROLES.has(n.role))
                        dialog = true;
                    if (n.role === 'button' && has(n.state, SENSITIVE) && !found) {
                        const [label] = await this._call(name, n.path, 'org.freedesktop.DBus.Properties', 'Get',
                            new GLib.Variant('(ss)', [`${ATSPI}.Accessible`, 'Name']), '(v)');
                        if (this._names.has(label.deepUnpack?.() ?? label))
                            found = n.path;
                    }
                    if (n.extents && n.extents[1] > BAND && !DIALOG_ROLES.has(n.role))
                        continue;
                    next.push(...n.children);
                }
                level = next;
            }
            if (generation !== this._generation)
                return {kind: 'unknown'};
            if (dialog)
                return {kind: 'dialog'};
            return found ? {kind: 'button', name, path: found} : {kind: 'none'};
        } catch (e) {
            if (!e.matches?.(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                log(`vibeos: back probe: ${e.message}`);
            return {kind: 'unknown'};
        }
    }

    // click the Back button the probe found
    async activate({name, path}) {
        try {
            await this._call(name, path, `${ATSPI}.Action`, 'DoAction', new GLib.Variant('(i)', [0]), '(b)');
            return true;
        } catch (e) {
            log(`vibeos: back: ${e.message}`);
            return false;
        }
    }

    destroy() {
        this._generation++;
        this._conn?.close(null, null);
        this._conn = null;
    }
}
