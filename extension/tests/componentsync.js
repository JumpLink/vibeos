// componentsync.js against a ComponentManager whose updates race as GNOME Shell's do (ui/components.js: a module
// loaded before a component is turned on, the record written after): the session starting on the lock screen while
// the user mode's update is still loading. Without ComponentSync the keyring stays on under the lock; with it, off
// while locked and on again once unlocked, whichever update finishes first. A session starting locked with the keyring
// not yet loaded still gets the shell's prompter on the bus (components/keyring.js claims it on its first enable()), so
// no standalone gcr-prompter opens a window behind the lock. componentsync.sh runs it.
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

    // components/keyring.js's disable() throws when the component is not on (its prompt is already being cancelled)
    disable() {
        if (!this.enabled)
            throw new Error('disabled twice');
        this.enabled = false;
    }
}

// Shell.KeyringPrompt (shell-keyring-prompt.c): a cancel before it has asked does nothing; one while it asks answers
// "cancelled", and gnome-keyring then closes it
class Prompt {
    constructor() {
        this.mode = 'none';
        this.shown = false;
        this.cancelled = false;
        this.closed = false;
        this._handlers = new Map();
        this._next = 1;
    }

    connect(signal, handler) {
        this._handlers.set(this._next, [signal, handler]);
        return this._next++;
    }

    disconnect(id) {
        this._handlers.delete(id);
    }

    _emit(signal) {
        [...this._handlers.values()].filter(([s]) => s === signal).forEach(([, h]) => h());
    }

    askPassword() {
        this.mode = 'password';
        this.shown = true;
        this._emit('show-password');
    }

    cancel() {
        if (this.mode === 'none') {
            if (this.shown)
                this.close();
            return;
        }
        this.mode = 'none';
        this.cancelled = true;
        this.close();
    }

    close() {
        if (this.closed)
            return;
        this.closed = true;
        this._emit('prompt-close');
    }
}

// components/keyring.js: the prompter's bus name claimed on the first enable() only, kept after disable(); a new
// prompt gets the modal dialog while on, the dummy that cancels while off; disable() cancels the prompt in progress
let prompterOwner = null;
class Keyring extends Component {
    constructor() {
        super();
        this._registered = false;
        this._currentPrompt = null;
        this.prompting = false;
    }

    get _enabled() {
        return this.enabled;
    }

    enable() {
        if (!this._registered) {
            prompterOwner = this;
            this._registered = true;
        }
        super.enable();
    }

    disable() {
        super.disable();
        if (this.prompting)
            this._currentPrompt.cancel();
        this._currentPrompt = null;
    }

    // a program begins prompting: gcr's new-prompt
    newPrompt() {
        const prompt = new Prompt();
        const dialog = {open: false};
        this.prompting = true;
        if (this._enabled) {
            prompt.connect('show-password', () => (dialog.open = true));
            prompt.connect('prompt-close', () => (dialog.open = false));
        } else {
            prompt.connect('show-password', () => prompt.cancel());
        }
        prompt.connect('prompt-close', () => (this.prompting = false));
        this._currentPrompt = prompt;
        return {prompt, dialog};
    }
}

// the session bus: who owns the prompter's name, what each process is called, which were sent SIGTERM
function fakeBus(owner, comms = {}) {
    const bus = {
        kills: [], watched: [], active: 0, handler: null,
        uniqueName: () => ':1.5',
        watch(name, appeared) {
            bus.watched.push(name);
            bus.active++;
            bus.handler = appeared;
            if (owner !== undefined)
                appeared(owner);
            return () => {
                bus.active--;
                bus.handler = null;
            };
        },
        pidOf: async o => Number(o.split('.')[1]) * 100,
        commOf: async pid => comms[pid] ?? 'other',
        terminate: pid => bus.kills.push(pid),
    };
    return bus;
}
// every instance made, whoever made it: one the manager's record lost is still on, and no sweep reaches it
let instances = [];
const load = async name => {
    const Base = name === 'keyring' ? Keyring : Component;
    return class extends Base {
        constructor() {
            super();
            instances.push([name, this]);
        }
    };
};

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
        component = new (await load(name))();
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
const errors = [];
const logged = globalThis.logError;
globalThis.logError = (e, ...rest) => {
    errors.push(e);
    logged(e, ...rest);
};
const check = (what, ok) => {
    print(`${ok ? 'ok' : 'FAILED'}: ${what}`);
    if (!ok)
        failed++;
};

async function run(name, withSync, reverse) {
    prompterOwner = null;
    instances = [];
    const mode = new Mode();
    const manager = new Manager(mode);
    const sync = withSync ? new ComponentSync(manager, mode, load, fakeBus()) : null;
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
    check(`${name}: locked, the shell's prompter on the bus`, prompterOwner === manager._allComponents.keyring);
    check(`${name}: locked, the automounter off`, !automountOn);
    const names = instances.map(([n]) => n);
    check(`${name}: each component made once`, names.length === new Set(names).size);
    check(`${name}: locked, no instance of the user mode's components on`,
        !instances.some(([n, c]) => !LOCKED.includes(n) && c.enabled));
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
    // syncs with nothing changed turn nothing off again
    mode.set(LOCKED, true);
    await settle();
    mode.set(LOCKED, true);
    await settle();
    check(`${name}: repeated syncs, no component turned off twice`, errors.length === 0);
    sync.destroy();
    check(`${name}: destroyed, the manager's own update back`,
        manager._sessionModeUpdated === Manager.prototype._sessionModeUpdated &&
        !Object.getOwnPropertyDescriptor(manager, '_enabledComponents').get);
}

// the session starts locked: the manager loads nothing, the prompter is claimed all the same
async function startLocked(withSync) {
    prompterOwner = null;
    const mode = new Mode();
    mode.components = LOCKED;
    mode.isLocked = true;
    const manager = new Manager(mode);
    const sync = withSync ? new ComponentSync(manager, mode, load, fakeBus()) : null;
    manager.release();
    await settle();
    if (!withSync) {
        check('started locked, unsynced: no prompter on the bus (gcr-prompter would start)', prompterOwner === null);
        return;
    }
    const keyring = manager._allComponents.keyring;
    check("started locked: the shell's prompter on the bus", !!keyring && prompterOwner === keyring);
    check('started locked: the keyring off', !keyring?.enabled);
    mode.set(USER, false);
    manager.release();
    await settle();
    check('started locked, then unlocked: the keyring on', !!manager._allComponents.keyring?.enabled);
    sync.destroy();
}

// a program begins prompting while unlocked, the phone locks, and only then the prompt asks for the password: the
// dialog made while the keyring was on would open under the lock screen
async function promptUnderLock(withSync) {
    prompterOwner = null;
    const mode = new Mode();
    const manager = new Manager(mode);
    const sync = withSync ? new ComponentSync(manager, mode, load, fakeBus()) : null;
    manager.release();
    await settle();
    const keyring = manager._allComponents.keyring;
    const early = keyring.newPrompt();
    mode.set(LOCKED, true);
    await settle();
    early.prompt.askPassword();
    await settle();
    if (!withSync) {
        check('prompt under the lock, unsynced: its dialog open under the lock (the bug reproduced)', early.dialog.open);
        return;
    }
    check('prompt under the lock: its dialog closed again', !early.dialog.open);
    check('prompt under the lock: the prompt cancelled', early.prompt.cancelled);
    check('prompt under the lock: the keyring no longer prompting', !keyring.prompting);
    mode.set(USER, false);
    manager.release();
    await settle();
    const later = keyring.newPrompt();
    later.prompt.askPassword();
    check('unlocked: a new prompt opens its dialog', later.dialog.open && !later.prompt.cancelled);
    // the prompt in progress when the phone locks, already asking: disable() itself cancels it
    mode.set(LOCKED, true);
    await settle();
    check('locked while asking: its dialog closed', !later.dialog.open && later.prompt.cancelled);
    check('prompt under the lock: no component turned off twice', errors.length === 0);
    sync.destroy();
    check("destroyed: the keyring's own disable() back", !Object.hasOwn(keyring, 'disable'));
}

// the standalone gcr-prompter (D-Bus-started before the extension) holds the name; the shell's request only queued
async function standalonePrompter(what, owner, comms, later) {
    prompterOwner = null;
    const mode = new Mode();
    mode.components = LOCKED;
    mode.isLocked = true;
    const manager = new Manager(mode);
    const bus = fakeBus(later ? undefined : owner, comms);
    const sync = new ComponentSync(manager, mode, load, bus);
    manager.release();
    await settle();
    check(`${what}: the prompter's name watched`, bus.watched.join() === 'org.gnome.keyring.SystemPrompter');
    if (later) {
        check(`${what}: no owner yet, no kill`, bus.kills.length === 0);
        bus.handler(owner);
        await settle();
    }
    return {bus, sync};
}

async function evict() {
    let {bus, sync} = await standalonePrompter('owner is the shell', ':1.5', {500: 'gnome-shell'});
    check('owner is the shell: nothing ended', bus.kills.length === 0);
    sync.destroy();
    ({bus, sync} = await standalonePrompter('owner is gcr-prompter', ':1.9', {900: 'gcr-prompter'}));
    check('owner is gcr-prompter: SIGTERM to its pid', JSON.stringify(bus.kills) === '[900]');
    check('owner is gcr-prompter: watched until destroy', bus.active === 1);
    sync.destroy();
    check('destroyed: the watch removed', bus.active === 0);
    ({bus, sync} = await standalonePrompter('owner is something else', ':1.9', {900: 'bash'}));
    check('owner is something else: nothing ended', bus.kills.length === 0);
    sync.destroy();
    ({bus, sync} = await standalonePrompter('owner appears later', ':1.7', {700: 'gcr-prompter'}, true));
    check('owner appears later: SIGTERM to its pid', JSON.stringify(bus.kills) === '[700]');
    sync.destroy();
}

const loop = new GLib.MainLoop(null, false);
(async () => {
    await run('unsynced', false, true);
    await run('user update first', true, false);
    await run('lock update first', true, true);
    await startLocked(false);
    await startLocked(true);
    await evict();
    await promptUnderLock(false);
    await promptUnderLock(true);
})().catch(e => {
    logError(e);
    failed++;
}).finally(() => loop.quit());
loop.run();
System.exit(failed ? 1 : 0);
