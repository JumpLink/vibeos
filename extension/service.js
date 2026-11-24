// vibeos.Shell on the session bus: what the launcher asks of the shell.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {dismissKeyboard, keyboardOverlays} from './keyboard/keyboard.js';
import {LaunchTransition} from './launch.js';
import {Navigation} from './navigation.js';
import {setExpectWindow} from './settings.js';

const IFACE = `<node>
  <interface name="vibeos.Shell">
    <!-- start an app; from = the icon's rectangle on screen, in pixels, the window grows out of it; icon = the
         launcher's picture of the icon (a PNG, for the splash), or '' for the app's themed icon -->
    <method name="Launch">
      <arg type="s" name="app_id" direction="in"/>
      <arg type="(dddd)" name="from" direction="in"/>
      <arg type="s" name="icon" direction="in"/>
    </method>
    <!-- open a file in an app as a launch out of the file's box: an app opening one of its files (Files). uri: the
         file; app_id: the app for it (a desktop file id); from: the file's box in the caller's window, in its
         surface's px. Back at the root of the window it opens in takes it back into the file (back.js) -->
    <method name="Open">
      <arg type="s" name="uri" direction="in"/>
      <arg type="s" name="app_id" direction="in"/>
      <arg type="(dddd)" name="from" direction="in"/>
    </method>
    <!-- the icons on the launcher's current page and dock: app id, the icon's box on screen in pixels, its picture
         (a PNG, or '') -->
    <method name="Icons">
      <arg type="a(s(dddd)s)" name="icons" direction="in"/>
    </method>
    <!-- the launcher's app drawer opened (or started to) or closed -->
    <method name="Drawer">
      <arg type="b" name="open" direction="in"/>
    </method>
    <!-- Android's full-screen intent (Notification.Builder.setFullScreenIntent): an app's urgent screen, a ringing alarm
         or a call coming in, starts when the phone is asleep or locked, and the screen comes on for it; with the phone
         in use the app's heads-up notification is enough. app_id: a desktop file id; started: whether it started it.
         Over the lock screen it shows if its desktop file says X-Vibeos-ShowWhenLocked=true (whenlocked.js) -->
    <method name="LaunchFullScreen">
      <arg type="s" name="app_id" direction="in"/>
      <arg type="b" name="started" direction="out"/>
    </method>
    <!-- Android hands a key to the focused activity first, and a ringing alarm or timer stops on a volume key. Here
         the volume keys are the shell's, so an app asks for them: while a window of app_id (the caller's GApplication
         id) has the focus, a volume key does not change the volume but activates the app's "volume-key" action, with
         1 for up and -1 for down, when it is pressed. take = false gives them back, and so does the caller leaving
         the bus. -->
    <method name="TakeVolumeKeys">
      <arg type="s" name="app_id" direction="in"/>
      <arg type="b" name="take" direction="in"/>
    </method>
    <!-- how much of the window in front the on-screen keyboard covers: the logical pixels from the keys' top edge down
         to the window's bottom, 0 when the keyboard is down. A window the shell fits above the keys (most apps) is
         never covered; one whose app's desktop file says X-Vibeos-Keyboard=overlay is, and the app makes room itself
         (keyboard.js). Changes are announced as the keyboard starts to come up and to go down. -->
    <property name="KeyboardOverlap" type="u" access="read"/>
    <!-- the screen's brightness as the quick settings' slider shows it and sets it, 0..1; -1 when the phone has no
         backlight the shell drives. For Agent, which sets it when asked ("brightness to 50 percent") -->
    <property name="Brightness" type="d" access="readwrite"/>
    <!-- the apps open now, the ones recents shows a card for, the most recently used first: each app's id (its
         desktop file's, without ".desktop") and its name. For Agent, which answers "which apps are open?" -->
    <method name="RunningApps">
      <arg type="a(ss)" name="apps" direction="out"/>
    </method>
    <!-- the notifications in the shade now, the newest first: each one's app id (its desktop file's, without
         ".desktop", or the sender's name), the app's name, the title, the body, and when it came (Unix seconds). For
         Agent, which answers "what did I miss?" with counts and app names, and reads them only when asked -->
    <method name="Notifications">
      <arg type="a(ssssx)" name="notifications" direction="out"/>
    </method>
  </interface>
</node>`;

export class Service {
    constructor(home) {
        this._home = home;
        this._launch = new LaunchTransition();
        this._nav = new Navigation(home);
        this._nav.onGestureBegin = () => {
            this._launch.cancelSplash();
            // the keyboard goes with the app (and the key a swipe from its bottom row landed on lets go)
            dismissKeyboard();
        };
        setExpectWindow(app => this._launch.expect(app));
        this._volumeKeys = new Map();   // app id -> {sender, watch}: who took the volume keys
        this._overlap = 0;
        this._sizedKeyboard = null;     // the keyboard whose height is watched
        Main.keyboard.connectObject('visibility-changed', () => this._updateOverlap(), this);
        this._impl = Gio.DBusExportedObject.wrapJSObject(IFACE, this);
        this._impl.export(Gio.DBus.session, '/vibeos/Shell');
        this._nameId = Gio.bus_own_name(Gio.BusType.SESSION, 'vibeos.Shell',
            Gio.BusNameOwnerFlags.REPLACE, null, null, null);
    }

    Launch(appId, from, icon) {
        const app = Shell.AppSystem.get_default().lookup_app(appId);
        if (!app) {
            console.warn(`vibeos: no app ${appId}`);
            return;
        }
        this._launch.launch(app, from, icon);
    }

    // The caller is known by its bus name's process: its window holds the box, at the window's surface. No window
    // of the caller's (it has none, or gave none): from a small box in the screen's middle.
    OpenAsync([uri, appId, from], invocation) {
        const app = Shell.AppSystem.get_default().lookup_app(appId);
        if (!app) {
            invocation.return_dbus_error('org.freedesktop.DBus.Error.InvalidArgs', `No app ${appId}`);
            return;
        }
        Gio.DBus.session.call('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
            'GetConnectionUnixProcessID', new GLib.Variant('(s)', [invocation.get_sender()]),
            new GLib.VariantType('(u)'), Gio.DBusCallFlags.NONE, -1, null, (bus, res) => {
                let pid = -1;
                try {
                    [pid] = bus.call_finish(res).deepUnpack();
                } catch {}
                const wins = global.display.get_tab_list(Meta.TabList.NORMAL, null).filter(w => w.get_pid() === pid);
                const opener = wins.find(w => w.has_focus()) ?? wins[0] ?? null;
                let box;
                if (opener && from[2] > 0 && from[3] > 0) {
                    const buf = opener.get_buffer_rect();
                    box = {x: buf.x + from[0], y: buf.y + from[1], w: from[2], h: from[3]};
                } else {
                    const area = Main.layoutManager.getWorkAreaForMonitor(Main.layoutManager.primaryIndex);
                    const s = Math.min(area.width, area.height) / 5;
                    box = {x: area.x + (area.width - s) / 2, y: area.y + (area.height - s) / 2, w: s, h: s};
                }
                this._launch.open(app, box, uri, opener);
                invocation.return_value(null);
            });
    }

    get KeyboardOverlap() {
        return this._overlap;
    }

    get Brightness() {
        return Main.brightnessManager.globalScale?.value ?? -1;
    }

    set Brightness(value) {
        const scale = Main.brightnessManager.globalScale;
        if (scale)
            scale.value = Math.clamp(value, 0, 1);
    }

    // The keys' top edge against the bottom of the window in front. The keyboard is measured, not where it is in its
    // slide: the app hears the end of the slide at its start, and moves with it.
    _updateOverlap() {
        const keyboard = Main.keyboard._keyboard ?? null;
        if (keyboard !== this._sizedKeyboard) {
            this._sizedKeyboard?.disconnectObject(this);
            this._sizedKeyboard = keyboard;
            keyboard?.connectObject('notify::height', () => this._updateOverlap(), 'destroy', () => {
                this._sizedKeyboard = null;
            }, this);
        }
        let overlap = 0;
        const window = global.display.focus_window;
        const monitor = Main.layoutManager.keyboardMonitor;
        if (Main.keyboard.visible && keyboard && monitor && window && keyboardOverlays(window)) {
            const top = monitor.y + monitor.height - keyboard.height;
            const frame = window.get_frame_rect();
            overlap = Math.max(0, Math.round(frame.y + frame.height - top));
        }
        if (overlap === this._overlap)
            return;
        this._overlap = overlap;
        this._impl.emit_property_changed('KeyboardOverlap', GLib.Variant.new_uint32(overlap));
    }

    // the navigation (a tablet's taskbar works with it)
    get navigation() {
        return this._nav;
    }

    Icons(icons) {
        this._nav.setIcons(icons);
    }

    Drawer(open) {
        this._nav.drawerOpen = open;
    }

    setKeyguard(keyguard) {
        this._keyguard = keyguard;
        this._nav.setKeyguard(keyguard);
    }

    // power.js's Power: whether the screen is on, and waking it
    setPower(power) {
        this._power = power;
    }

    LaunchFullScreen(appId) {
        if (this._power?.interactive && !this._keyguard?.showing)
            return false;
        // an app id ("vibeos.Clock.Ringing") or a desktop id ("vibeos.Clock.Ringing.desktop")
        const appSystem = Shell.AppSystem.get_default();
        const app = appSystem.lookup_app(appId) ?? appSystem.lookup_app(`${appId}.desktop`);
        if (!app) {
            console.warn(`vibeos: no app ${appId} to launch full-screen`);
            return false;
        }
        app.activate();
        this._power?.wakeUp('full-screen');
        return true;
    }

    RunningApps() {
        const tracker = Shell.WindowTracker.get_default();
        const seen = new Set();
        const apps = [];
        // recents' order: the tab list is the most recently used first (every workspace's)
        for (const win of global.display.get_tab_list(Meta.TabList.NORMAL, null)) {
            if (!this._nav.isTask(win))
                continue;
            const app = tracker.get_window_app(win);
            const id = app?.get_id().replace(/\.desktop$/, '');
            if (!id || seen.has(id))
                continue;
            seen.add(id);
            apps.push([id, app.get_name()]);
        }
        return apps;
    }

    Notifications() {
        const list = [];
        for (const source of Main.messageTray.getSources()) {
            const id = (source.app?.get_id() ?? source.policy?.id ?? '').replace(/\.desktop$/, '');
            for (const n of source.notifications) {
                const when = n.datetime?.to_unix() ?? 0;
                list.push([id, source.title ?? '', n.title ?? '', n.body ?? n.bannerBodyText ?? '', when]);
            }
        }
        list.sort((a, b) => b[4] - a[4]);
        return list;
    }

    TakeVolumeKeysAsync([appId, take], invocation) {
        this._giveVolumeKeysBack(appId);
        if (take) {
            const sender = invocation.get_sender();
            const watch = Gio.bus_watch_name_on_connection(invocation.get_connection(), sender,
                Gio.BusNameWatcherFlags.NONE, null, () => this._giveVolumeKeysBack(appId));
            this._volumeKeys.set(appId, {sender, watch});
        }
        invocation.return_value(null);
    }

    _giveVolumeKeysBack(appId) {
        const taker = this._volumeKeys.get(appId);
        if (!taker)
            return;
        Gio.bus_unwatch_name(taker.watch);
        this._volumeKeys.delete(appId);
    }

    // volume.js: whether the app in front took the volume keys; if so, a function that hands it one (+1 or -1)
    volumeKeysTaker() {
        const app = Shell.WindowTracker.get_default().focus_app;
        const appId = app?.get_id().replace(/\.desktop$/, '');
        const taker = appId && this._volumeKeys.get(appId);
        if (!taker)
            return null;
        // the GApplication's object path: its id as a path, vibeos.Clock.Ringing at /vibeos/Clock/Ringing
        const path = `/${appId.replaceAll('.', '/')}`;
        return direction => Gio.DBus.session.call(taker.sender, path, 'org.freedesktop.Application',
            'ActivateAction', new GLib.Variant('(sava{sv})', ['volume-key', [new GLib.Variant('i', direction)], {}]),
            null, Gio.DBusCallFlags.NONE, -1, null, null);
    }

    // the side button's recents (keylaunch.js)
    toggleRecents() {
        this._nav.toggleRecents();
    }

    // An app a key opens (keylaunch.js), with the launch's animation out of a box in the screen's middle, as an app
    // opened from another; one already in front only stays there
    launchApp(app) {
        const focus = global.display.focus_window;
        if (focus && Shell.WindowTracker.get_default().get_window_app(focus) === app) {
            Main.activateWindow(focus);
            return;
        }
        const index = Main.layoutManager.primaryIndex;
        const area = Main.layoutManager.getWorkAreaForMonitor(index);
        const scale = global.display.get_monitor_scale(index);
        const s = Math.min(area.width, area.height) / 5;
        this._launch.launch(app, [(area.x + (area.width - s) / 2) * scale, (area.y + (area.height - s) / 2) * scale,
            s * scale, s * scale], '');
    }

    destroy() {
        Main.keyboard.disconnectObject(this);
        this._sizedKeyboard?.disconnectObject(this);
        setExpectWindow(null);
        this._launch.destroy();
        this._nav.destroy();
        for (const appId of [...this._volumeKeys.keys()])
            this._giveVolumeKeysBack(appId);
        Gio.bus_unown_name(this._nameId);
        this._impl.unexport();
    }
}
