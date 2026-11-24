// componentsync.js against a ComponentManager whose updates race as GNOME Shell's do (ui/components.js: a module
// loaded before a component is turned on, the record written after): the session starting on the lock screen while
// the user mode's update is still loading. Without ComponentSync the keyring stays on under the lock; with it, off
// while locked and on again once unlocked, whichever update finishes first. componentsync.sh runs it.
import GLib from 'gi://GLib';
import System from 'system';

import {ComponentSync} from './componentsync.js';

const USER = ['polkitAgent', 'keyring', 'automountManager'];
const LOCKED = ['polkitAgent'];

class Mode {
    constructor() {
        this.components = USER;
        this.isLocked = false;
        this._handlers = [];
    }

    connect(_signal, handler) {
        this._handlers.push(handler);
    }

    set(components, isLocked) {
        this.components = components;
        this.isLocked = isLocked;
        this._handlers.forEach(h => h());
    }
}

class Component {
    constructor() {
        this.enabled = false;
    }

    enable() {
        this.enabled = true;
    }

    disable() {
        this.enabled = false;
    }
}

// ui/components.js's ComponentManager, its imports held until release() (or each one, in the order given)
class Manager {
    constructor(mode) {
        this._mode = mode;
        this._allComponents = {};
        this._enabledComponents = [];
        this._pending = [];
        mode.connect('updated', () => {
            this._sessionModeUpdated().catch(logError);
        });
        this._sessionModeUpdated().catch(logError);
    }

    async _sessionModeUpdated() {
        const newEnabledComponents = this._mode.components;
        await Promise.allSettled([...newEnabledComponents
            .filter(name => !this._enabledComponents.includes(name))
            .map(name => this._enableComponent(name))]);
        this._enabledComponents
            .filter(name => !newEnabledComponents.includes(name))
            .forEach(name => this._disableComponent(name));
        this._enabledComponents = newEnabledComponents;
    }

    async _ensureComponent(name) {
        let component = this._allComponents[name];
        if (component)
            return component;
        const isLocked = this._mode.isLocked;
        await new Promise(resolve => this._pending.push(resolve));
        if (isLocked)
            return null;
        component = new Component();
        this._allComponents[name] = component;
        return component;
    }

    async _enableComponent(name) {
        const component = await this._ensureComponent(name);
        if (component)
            component.enable();
    }

    _disableComponent(name) {
        this._allComponents[name]?.disable();
    }

    release(reverse = false) {
        const pending = this._pending.splice(0);
        (reverse ? pending.reverse() : pending).forEach(resolve => resolve());
    }
}

const settle = async () => {
    for (let i = 0; i < 20; i++)
        // eslint-disable-next-line no-await-in-loop
        await new Promise(resolve => GLib.idle_add(GLib.PRIORITY_DEFAULT, () => resolve() ?? GLib.SOURCE_REMOVE));
};

let failed = 0;
const check = (what, ok) => {
    print(`${ok ? 'ok' : 'FAILED'}: ${what}`);
    if (!ok)
        failed++;
};

async function run(name, withSync, reverse) {
    const mode = new Mode();
    const manager = new Manager(mode);
    const sync = withSync ? new ComponentSync(manager, mode) : null;
    // the lock screen's pushMode('unlock-dialog'), the user mode's modules still loading
    mode.set(LOCKED, true);
    manager.release(reverse);
    await settle();
    manager.release(reverse);
    await settle();
    const keyringOn = !!manager._allComponents.keyring?.enabled;
    const automountOn = !!manager._allComponents.automountManager?.enabled;
    if (!withSync) {
        check(`${name}: the race leaves the keyring on under the lock (the bug reproduced)`, keyringOn);
        return;
    }
    check(`${name}: locked, the keyring off`, !keyringOn);
    check(`${name}: locked, the automounter off`, !automountOn);
    check(`${name}: locked, the record is the mode's`,
        JSON.stringify(manager._enabledComponents) === JSON.stringify(LOCKED));
    // unlocked: the user mode's components on again
    mode.set(USER, false);
    manager.release();
    await settle();
    check(`${name}: unlocked, the keyring on`, !!manager._allComponents.keyring?.enabled);
    check(`${name}: unlocked, the automounter on`, !!manager._allComponents.automountManager?.enabled);
    // locked again, nothing racing: off
    mode.set(LOCKED, true);
    await settle();
    check(`${name}: locked again, the keyring off`, !manager._allComponents.keyring.enabled);
    sync.destroy();
    check(`${name}: destroyed, the manager's own update back`,
        manager._sessionModeUpdated === Manager.prototype._sessionModeUpdated &&
        !Object.getOwnPropertyDescriptor(manager, '_enabledComponents').get);
}

const loop = new GLib.MainLoop(null, false);
(async () => {
    await run('unsynced', false, true);
    await run('user update first', true, false);
    await run('lock update first', true, true);
})().catch(e => {
    logError(e);
    failed++;
}).finally(() => loop.quit());
loop.run();
System.exit(failed ? 1 : 0);
