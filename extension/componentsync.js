import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

// The shell's components (the keyring's password dialog, the automounter, ...) kept to the session mode as it is now.
// GNOME Shell's ComponentManager follows the mode with an async update that loads a component's module before turning
// it on and only then records which ones are on (ui/components.js, _sessionModeUpdated()). The session starts on the
// lock screen: its 'unlock-dialog' mode is pushed while the update for the 'user' mode is still loading. The second
// update compares against the record not yet written and turns nothing off, and whichever finishes last writes the
// record. The keyring's prompter then stays on under the lock screen, and the first program asking for the locked
// keyring opens its dialog there: a modal under the lock screen, holding the grab, every touch going to it (GNOME's
// own lock leaves the keyring off, where a prompt is cancelled at once).
// Here the updates run one at a time; after each, every loaded component the mode does not list is turned off (each
// component's disable() may run twice) and the record is the mode's list. A record written by an update that was
// already under way when the extension came on starts another round.
// The shell's keyring prompter claims org.gnome.keyring.SystemPrompter only the first time it is turned on
// (components/keyring.js, enable()), and the manager does not even load a component while locked. A session that starts
// on the lock screen leaves the name free, D-Bus starts the standalone gcr-prompter for the first program asking, and its
// window waits behind the lock screen until the phone is unlocked, though unlocking has opened the keyring meanwhile.
// So while the mode leaves the keyring off, it is loaded and turned on and off once: the name is the shell's, and its
// prompter, off, cancels each prompt as GNOME's own lock does.
// gnome-keyring D-Bus-starts /usr/libexec/gcr-prompter about three seconds after the shell, before this extension is on.
// It owns the name without allowing replacement, so the shell's request only queues behind it and its window keeps asking
// after the unlock. Its owner is therefore asked to leave (SIGTERM, only for a process named gcr-prompter): the name then
// passes to the shell's queued request. The name is watched for as long as the sync lives, for one that starts later.
const PROMPTER = 'org.gnome.keyring.SystemPrompter';
Gio._promisify(Gio.DBusConnection.prototype, 'call', 'call_finish');

const loadComponent = async name => (await import(`resource:///org/gnome/shell/ui/components/${name}.js`)).Component;

// the session bus, as far as the check needs it; the test stands in for it
const sessionBus = {
    uniqueName: () => Gio.DBus.session.get_unique_name(),

    // appeared(owner) on every owner of the name, now and later; returns the function that stops watching
    watch(name, appeared) {
        const id = Gio.bus_watch_name_on_connection(Gio.DBus.session, name, Gio.BusNameWatcherFlags.NONE,
            (_connection, _name, owner) => appeared(owner), null);
        return () => Gio.bus_unwatch_name(id);
    },

    async pidOf(owner) {
        const reply = await Gio.DBus.session.call('org.freedesktop.DBus', '/org/freedesktop/DBus',
            'org.freedesktop.DBus', 'GetConnectionUnixProcessID', new GLib.Variant('(s)', [owner]),
            new GLib.VariantType('(u)'), Gio.DBusCallFlags.NONE, -1, null);
        return reply.deepUnpack()[0];
    },

    async commOf(pid) {
        const [, bytes] = GLib.file_get_contents(`/proc/${pid}/comm`);
        return new TextDecoder().decode(bytes).trim();
    },

    terminate(pid) {
        GLib.spawn_command_line_async(`kill -TERM ${Number(pid)}`);
    },
};

export class ComponentSync {
    // manager: Main.componentManager, mode: Main.sessionMode, load: a component's class by name, bus: see sessionBus
    constructor(manager, mode, load = loadComponent, bus = sessionBus) {
        this._manager = manager;
        this._mode = mode;
        this._load = load;
        this._bus = bus;
        this._unwatch = null;
        this._running = null;
        this._again = false;
        this._update = manager._sessionModeUpdated;
        manager._sessionModeUpdated = () => this._sync();

        this._enabled = manager._enabledComponents;
        Object.defineProperty(manager, '_enabledComponents', {
            configurable: true,
            enumerable: true,
            get: () => this._enabled,
            set: value => {
                this._enabled = value;
                if (!this._running)
                    this._sync().catch(logError);
            },
        });
        this._sync().catch(logError);
    }

    _wanted() {
        return this._mode.components ?? [];
    }

    // every loaded component the mode does not list, off; the record, the mode's list
    _sweep() {
        const want = this._wanted();
        for (const [name, component] of Object.entries(this._manager._allComponents ?? {})) {
            if (want.includes(name))
                continue;
            // one that fails to turn off keeps none of the others on
            try {
                component.disable();
            } catch (e) {
                logError(e, `vibeos: turning the shell's ${name} off`);
            }
        }
        this._enabled = [...want];
    }

    // the keyring's prompter on the bus while the mode leaves it off (see above)
    async _claimPrompter() {
        if (this._wanted().includes('keyring'))
            return;
        let keyring = this._manager._allComponents.keyring;
        if (!keyring) {
            const Component = await this._load('keyring');
            keyring = this._manager._allComponents.keyring ??= new Component();
        }
        if (keyring._registered === false && !this._wanted().includes('keyring')) {
            keyring.enable();
            keyring.disable();
        }
    }

    // from the first time the keyring was turned on (its request for the name is made), every owner of the name that is
    // not the shell's is a standalone prompter to end
    _guardPrompter() {
        if (this._unwatch || this._manager._allComponents.keyring?._registered !== true)
            return;
        this._unwatch = this._bus.watch(PROMPTER, owner => {
            this._evict(owner).catch(e => logError(e, 'vibeos: the standalone keyring prompter'));
        });
    }

    async _evict(owner) {
        if (!owner || owner === this._bus.uniqueName())
            return;
        const pid = await this._bus.pidOf(owner);
        // never end anything but the prompter itself
        if ((await this._bus.commOf(pid)) === 'gcr-prompter')
            this._bus.terminate(pid);
    }

    _sync() {
        if (this._running) {
            this._again = true;
            return this._running;
        }
        this._running = (async () => {
            try {
                do {
                    this._again = false;
                    // eslint-disable-next-line no-await-in-loop
                    await this._update.call(this._manager);
                    this._sweep();
                    // eslint-disable-next-line no-await-in-loop
                    await this._claimPrompter().catch(e => logError(e, "vibeos: the keyring's prompter"));
                    this._guardPrompter();
                } while (this._again);
            } finally {
                this._running = null;
            }
        })();
        return this._running;
    }

    destroy() {
        this._unwatch?.();
        this._unwatch = null;
        const enabled = this._enabled;
        delete this._manager._enabledComponents;
        this._manager._enabledComponents = enabled;
        delete this._manager._sessionModeUpdated;
    }
}
