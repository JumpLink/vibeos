// What back means in a GTK app, asked of its accessibility tree: Android lets an app say whether it handles back
// itself; a GNOME app says it with a Back button in its header bar. The tree is walked a level at a time, every
// node of a level asked at once over the accessibility bus (nothing here blocks the compositor), and only into what
// reaches the top band of the window, where header bars are.
//
//   {kind: 'button', name, path}   a Back button is showing: back is the app's (click it)
//   {kind: 'dialog'}               a dialog is showing in the window: back closes it (Escape)
//   {kind: 'none'}                 nothing to go back to: back leaves the app (back to home)
//   {kind: 'unknown'}              not an accessible app, or no answer
//
// The Back button is known by its name, which is in the app's language: that need not be the shell's. Settings
// changes the session's language at once, and every app started after that speaks the new one while the shell goes
// on in the language it started in (owner, 11-25: in Russian back went home from every page, the shell still
// German). So the name is read in the app's language: the app says which one over the accessibility bus (its Locale,
// LC_MESSAGES as it set it) and in its environment (LANGUAGE, which gettext puts first), and the word is read from the
// catalogs it reads it from itself, on the disk: libadwaita's back button, GTK's, libvibeos's and VibeOS's apps'.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

Gio._promisify(Gio.DBusConnection.prototype, 'call');
Gio._promisify(Gio.DBusConnection, 'new_for_address');
Gio._promisify(Gio.File.prototype, 'load_contents_async');
Gio._promisify(Gio.File.prototype, 'enumerate_children_async');
Gio._promisify(Gio.FileEnumerator.prototype, 'next_files_async');

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

// ---- "Back" in the app's language ----------------------------------------------------------------------------------

const BACK = 'Back';
// the catalogs a Back button's name comes from: libadwaita's AdwBackButton, GTK's, and VibeOS's (libvibeos's top bar
// and search, each app's own toolbars)
const CATALOGS = /^(libadwaita|gtk40|libvibeos-1|vibeos-.+)\.mo$/;

// The directories gettext looks in for an app whose messages' locale is @locale ("ru_UA.UTF-8") and whose LANGUAGE
// is @language ("ru:de", or empty): ru_UA, ru. None for C and POSIX (C.UTF-8 too): no catalog is read there, LANGUAGE
// included.
export function catalogLanguages(locale, language = '') {
    const out = [];
    if (!locale || locale === 'C' || locale === 'POSIX' || locale.startsWith('C.'))
        return out;
    const add = name => {
        const m = /^([a-z]{2,3})(_[A-Za-z0-9]+)?(?:\.[^@]*)?(@.+)?$/.exec(name);
        if (!m)
            return;
        const [, ll, cc = '', mod = ''] = m;
        for (const dir of [ll + cc + mod, ll + cc, ll + mod, ll]) {
            if (!out.includes(dir))
                out.push(dir);
        }
    };
    for (const name of language.split(':'))
        add(name);
    add(locale);
    return out;
}

// @msgid's translation in a GNU .mo catalog (its bytes), or null: the first form of a plural, none for an entry with a
// context
export function translation(bytes, msgid) {
    if (bytes.length < 28)
        return null;
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const magic = view.getUint32(0, true);
    const le = magic === 0x950412de;
    if (!le && magic !== 0xde120495)
        return null;
    const want = new TextEncoder().encode(msgid);
    const n = view.getUint32(8, le), originals = view.getUint32(12, le), translations = view.getUint32(16, le);
    if (originals + n * 8 > bytes.length || translations + n * 8 > bytes.length)
        return null;
    for (let i = 0; i < n; i++) {
        const length = view.getUint32(originals + i * 8, le), at = view.getUint32(originals + i * 8 + 4, le);
        // (a plural's original is the singular, a NUL and the plural)
        if (length < want.length || at + length > bytes.length || (length > want.length && bytes[at + want.length]) ||
            want.some((b, j) => bytes[at + j] !== b))
            continue;
        const tLength = view.getUint32(translations + i * 8, le), tAt = view.getUint32(translations + i * 8 + 4, le);
        if (tAt + tLength > bytes.length)
            return null;
        let text = bytes.subarray(tAt, tAt + tLength);
        const nul = text.indexOf(0);
        if (nul >= 0)
            text = text.subarray(0, nul);
        return text.length ? new TextDecoder().decode(text) : null;
    }
    return null;
}

// What a Back button is called in an app's language: read from the catalogs once a language, in the background
export class BackNames {
    // @dirs: where catalogs are (default: the system's data directories' locale/)
    constructor(dirs = GLib.get_system_data_dirs().map(d => GLib.build_filenamev([d, 'locale']))) {
        this._dirs = [...new Set(dirs)];
        this._languages = new Map();        // a catalog directory's name → the promise of its words for Back
        // the shell's own language, and English (an app in a language it has no catalog for)
        this._own = new Set([BACK, GLib.dgettext('libadwaita', BACK), GLib.dgettext('gtk40', BACK)]);
    }

    // every name a Back button has in an app whose messages' locale is @locale and whose LANGUAGE is @language
    async names(locale, language = '') {
        const words = await Promise.all(catalogLanguages(locale, language).map(l => this._language(l)));
        return new Set([...this._own, ...words.flat()]);
    }

    _language(name) {
        let words = this._languages.get(name);
        if (!words) {
            words = this._read(name);
            this._languages.set(name, words);
        }
        return words;
    }

    async _read(name) {
        const words = new Set();
        await Promise.all(this._dirs.map(async dir => {
            const where = Gio.File.new_for_path(GLib.build_filenamev([dir, name, 'LC_MESSAGES']));
            const files = [];
            try {
                const list = await where.enumerate_children_async('standard::name', Gio.FileQueryInfoFlags.NONE,
                    GLib.PRIORITY_LOW, null);
                for (let got; (got = await list.next_files_async(64, GLib.PRIORITY_LOW, null)).length;)
                    files.push(...got.map(info => info.get_name()).filter(f => CATALOGS.test(f)));
                list.close(null);
            } catch {
                return;         // no catalogs of that language here
            }
            await Promise.all(files.map(async f => {
                try {
                    const [bytes] = await where.get_child(f).load_contents_async(null);
                    const word = translation(bytes, BACK);
                    if (word)
                        words.add(word);
                } catch {}
            }));
        }));
        return [...words];
    }
}

// the variables of the process @pid's environment as it was started (an exec in place starts it again)
async function environment(pid) {
    const env = new Map();
    try {
        const [bytes] = await Gio.File.new_for_path(`/proc/${pid}/environ`).load_contents_async(null);
        for (const line of new TextDecoder().decode(bytes).split('\0')) {
            const eq = line.indexOf('=');
            if (eq > 0)
                env.set(line.slice(0, eq), line.slice(eq + 1));
        }
    } catch {}
    return env;
}

// a call to a bus name that is gone: its process ended, or started over in place (Settings after a language change)
const gone = e => e?.matches?.(Gio.DBusError, Gio.DBusError.NAME_HAS_NO_OWNER) ||
    e?.matches?.(Gio.DBusError, Gio.DBusError.SERVICE_UNKNOWN);

export class BackProbe {
    constructor() {
        this._conn = null;
        this._connecting = null;
        this._pids = new Map();             // bus name → pid
        this._backNames = new BackNames();
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

    // what a Back button is called in the app at @name (process @pid): in the language it says it is in, and the
    // shell's own
    async _namesIn(name, pid) {
        const [locale, env] = await Promise.all([
            this._call(name, ROOT, 'org.freedesktop.DBus.Properties', 'Get',
                new GLib.Variant('(ss)', [`${ATSPI}.Accessible`, 'Locale']), '(v)')
                .then(([v]) => v.deepUnpack?.() ?? v).catch(() => null),
            environment(pid),
        ]);
        // (no Locale from the toolkit: what its environment says)
        return this._backNames.names(locale || env.get('LC_ALL') || env.get('LC_MESSAGES') || env.get('LANG'),
            env.get('LANGUAGE'));
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
            let name = await this._app(pid);
            if (!name)
                return {kind: 'unknown'};
            let root;
            try {
                root = await this._node(name, ROOT);
            } catch (e) {
                if (!gone(e))
                    throw e;
                // The process started over in place (Settings after a language change): the same pid, a new name.
                // The old one would be asked forever (owner, 11-25: "NameHasNoOwner :1.7" on every back in Settings).
                this._pids.delete(name);
                name = await this._app(pid);
                if (!name)
                    return {kind: 'unknown'};
                root = await this._node(name, ROOT);
            }
            let [toplevels, names] = await Promise.all([
                Promise.all(root.children.map(p => this._node(name, p))),
                this._namesIn(name, pid),
            ]);
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
                        if (names.has(label.deepUnpack?.() ?? label))
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
