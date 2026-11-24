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
export class ComponentSync {
    // manager: Main.componentManager, mode: Main.sessionMode
    constructor(manager, mode) {
        this._manager = manager;
        this._mode = mode;
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
                } while (this._again);
            } finally {
                this._running = null;
            }
        })();
        return this._running;
    }

    destroy() {
        const enabled = this._enabled;
        delete this._manager._enabledComponents;
        this._manager._enabledComponents = enabled;
        delete this._manager._sessionModeUpdated;
    }
}
