// Home's rule and a window asking for attention, on copies of home.js and attention.js with stand-ins for GNOME Shell
// (home-attention.sh):
//
//   Home    every app window on the phone's screen goes to recents (minimized), the swiped one's neighbours too; one
//           on a TV stays, as do the launcher, a dialog, and what is in recents already. A swipe's windows go without
//           the minimize animation. The launcher is activated, and the phone counts as home from then on.
//   no banner   GNOME's handler ("“Window” is ready") hears nothing while attention.js is on, for either signal, and a
//           window that asks gets nothing: not activated, no banner.
//   launched    a window of the app just launched from the launcher comes to the front (not while its splash is up:
//           the splash brings it), for ATTENTION_MS and no longer.
//   intent  a window asking within INTENT_MS of a tap in an app comes to the front; not when the phone went home
//           after that tap (the swipe's own touch is the last one then), and again after a fresh touch.
//   off     GNOME's handler hears them again once attention.js is destroyed.
import GLib from 'gi://GLib';
import System from 'system';

import {Main} from './stand-ins.js';

let failures = 0;
function check(name, ok, detail = '') {
    print(ok ? 'PASS' : 'FAIL', name, detail);
    if (!ok)
        failures++;
}

// ---- stand-ins of mutter's ----------------------------------------------------------------------------------------

const NORMAL = 0, DIALOG = 3, PHONE = 0, TV = 1;
class Win {
    constructor(name, {app = null, type = NORMAL, monitor = PHONE, minimized = false, focus = false} = {}) {
        Object.assign(this, {name, app, type, monitor, minimized, focus, activations: 0});
        this.actor = {win: name};
    }

    get_window_type() {
        return this.type;
    }

    get_monitor() {
        return this.monitor;
    }

    get_compositor_private() {
        return this.actor;
    }

    minimize() {
        this.minimized = true;
    }

    activate() {
        this.activations++;
        // an activation is a user time, as mutter's is
        display.userTime = display.now;
    }

    has_focus() {
        return this.focus;
    }
}

// the display: its signals (connectObject as GNOME Shell adds it), its user time and its clock (ms)
const display = {
    handlers: [],
    userTime: 0,
    now: 100000,
    connectObject(...args) {
        const tracker = args.pop();
        for (let i = 0; i < args.length; i += 2)
            this.handlers.push({name: args[i], fn: args[i + 1], tracker});
    },
    disconnectObject(tracker) {
        this.handlers = this.handlers.filter(h => h.tracker !== tracker);
    },
    emit(name, win) {
        for (const h of this.handlers.filter(x => x.name === name))
            h.fn(this, win);
    },
    get_last_user_time() {
        return this.userTime;
    },
    get_current_time_roundtrip() {
        return this.now;
    },
};
let windows = [];
globalThis.global = {
    display,
    workspace_manager: {get_active_workspace: () => ({list_windows: () => windows})},
    get_current_time: () => display.now,
};

// GNOME's handler, as main.js makes it: connected to both signals, a banner for each window that asks
const banners = [];
const gnome = {
    _onWindowDemandsAttention(_display, win) {
        if (win && !win.has_focus())
            banners.push(win.name);
    },
};
display.connectObject(
    'window-demands-attention', gnome._onWindowDemandsAttention.bind(gnome),
    'window-marked-urgent', gnome._onWindowDemandsAttention.bind(gnome),
    gnome);
Main.windowAttentionHandler = gnome;

const {Home} = await import('./home.js');
const {Attention, ATTENTION_MS, INTENT_MS} = await import('./attention.js');

// ---- Home ---------------------------------------------------------------------------------------------------------

const launcher = new Win('launcher');
const home = Object.create(Home.prototype);
home._window = launcher;
home.homeAt = 0;
const fresh = () => {
    windows = [
        launcher,
        new Win('swiped'),
        new Win('quick-switched from'),
        new Win('opened from a recents card earlier'),
        new Win('on the TV', {monitor: TV}),
        new Win('a dialog', {type: DIALOG}),
        new Win('in recents', {minimized: true}),
    ];
    Main.wm.skipped = [];
    launcher.activations = 0;
};
const named = name => windows.find(w => w.name === name);
const phoneApps = ['swiped', 'quick-switched from', 'opened from a recents card earlier'];

fresh();
display.now += 1000;
display.userTime = display.now - 300;       // the Home key's press
home.goHome();
check('Home key: every app window on the phone\'s screen to recents', phoneApps.every(n => named(n).minimized),
    phoneApps.filter(n => !named(n).minimized).join(', '));
check('Home key: the TV\'s window stays', !named('on the TV').minimized);
check('Home key: the launcher, a dialog stay', !launcher.minimized && !named('a dialog').minimized);
check('Home key: the launcher activated', launcher.activations === 1);
check('Home key: minimized with the animation', Main.wm.skipped.length === 0, `${Main.wm.skipped.length} skipped`);
check('Home key: home from the launcher\'s activation on', home.homeAt === display.userTime,
    `homeAt ${home.homeAt}, user time ${display.userTime}`);

// the swipe (navigation.js flight(), sideways.js _home()): its own window first, then the rule, without animations
fresh();
named('swiped').minimize();
home.appsToRecents(true);
check('swipe: the windows under the swiped one to recents too', phoneApps.every(n => named(n).minimized),
    phoneApps.filter(n => !named(n).minimized).join(', '));
check('swipe: without the minimize animation',
    Main.wm.skipped.length === 2 && !Main.wm.skipped.includes(named('on the TV').actor),
    `${Main.wm.skipped.length} skipped`);
check('swipe: the TV\'s window stays', !named('on the TV').minimized);

// ---- attention ----------------------------------------------------------------------------------------------------

const launch = {app: null, at: 0, splash: null};
const attention = new Attention(() => launch, home);
const messages = {name: 'Messages'}, contacts = {name: 'Contacts'};
const ask = (win, signal = 'window-demands-attention') => {
    const before = Main.activated.length;
    display.emit(signal, win);
    return Main.activated.length > before;
};

// long after any touch, not launched: an app started from a shell over ssh, as the release check did
display.now += 60000;
let asked = ask(new Win('Messages', {app: messages}));
check('no banner: GNOME\'s handler hears nothing', banners.length === 0, banners.join(', '));
check('no banner: a window that asks gets nothing', !asked);
asked = ask(new Win('Contacts', {app: contacts}), 'window-marked-urgent');
check('no banner: nor one marked urgent', banners.length === 0 && !asked, banners.join(', '));

launch.app = contacts;
launch.at = GLib.get_monotonic_time();
launch.splash = contacts;
check('launched: not while its splash is up (the splash brings it)', !ask(new Win('Contacts', {app: contacts})));
launch.splash = null;
check('launched: the app just launched from the launcher comes to the front', ask(new Win('Contacts', {app: contacts})));
check('launched: a focused window is left alone', !ask(new Win('Contacts', {app: contacts, focus: true})));
check('launched: another app gets nothing', !ask(new Win('Messages', {app: messages})));
launch.at = GLib.get_monotonic_time() - (ATTENTION_MS + 1000) * 1000;
check('launched: not after ATTENTION_MS', !ask(new Win('Contacts', {app: contacts})));

// a tap in Contacts on Send SMS: Messages asks a moment later
display.now += 60000;
display.userTime = display.now - 400;
check('intent: a window asking right after a tap comes to the front', ask(new Win('Messages', {app: messages})));
display.now += INTENT_MS;
check('intent: not long after it', !ask(new Win('Messages', {app: messages})));

// a swipe home: the touch began on the app 300 ms before the launcher was activated; an app asks right after
display.now += 60000;
display.userTime = display.now - 300;
fresh();
named('swiped').minimize();
home.appsToRecents(true);
display.now += 20;
launcher.activate();
home.markHome();
display.now += 500;
check('intent: nothing comes up by itself after Home', !ask(new Win('Messages', {app: messages})));
display.now += 300;
display.userTime = display.now - 50;           // the next touch, on the launcher
check('intent: after a fresh touch it does again', ask(new Win('Messages', {app: messages})));
check('no banner: none all along', banners.length === 0, banners.join(', '));

attention.destroy();
ask(new Win('Messages', {app: messages}));
ask(new Win('Contacts', {app: contacts}), 'window-marked-urgent');
check('off: GNOME\'s handler hears both signals again', banners.length === 2, banners.join(', '));
check('off: attention.js hears none', display.handlers.every(h => h.tracker === gnome),
    `${display.handlers.length} handlers`);

print(failures ? `FAILED ${failures}` : 'ALL PASSED');
System.exit(failures ? 1 : 0);
