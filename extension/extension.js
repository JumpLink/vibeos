// VibeOS: turns GNOME Shell (mobile) into the phone the VibeOS launcher is the home screen of.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

import {ScreenshotChord} from './chord.js';
import {CollectWhenStill} from './gc.js';
import {Cutout, CutoutBand} from './cutout.js';
import {Desktop, isDesktopSession} from './dock.js';
import {DisplayChangeOnPanel} from './displaychange.js';
import {EndMark} from './endmark.js';
import {syncGtkCss} from './gtkcss.js';
import {HeadsUp} from './headsup.js';
import {Home} from './home.js';
import {KeyboardSwap} from './keyboard/keyboard.js';
import {KeyHaptics} from './keyhaptics.js';
import {AppSwitchKey} from './keys.js';
import {Keyguard} from './keyguard.js';
import {NoWindowMipmaps} from './mipmaps.js';
import {AndroidKeyboard} from './osk.js';
import {TouchFocusKeyboard} from './oskfocus.js';
import {overWindows, restoreUnredirect} from './scanout.js';
import {Power} from './power.js';
import {PowerMenu} from './powermenu.js';
import {Rotation} from './rotation.js';
import {Service} from './service.js';
import {Shade} from './shade.js';
import {SystemState} from './state.js';
import {StatusBar, hideShellPanel} from './statusbar.js';
import {VolumeDialog} from './volume.js';
import {WhenLocked} from './whenlocked.js';

// how long the boot splash waits for the lock screen's wallpaper after the lock at most (a POCO X3 decodes the
// 1080x2280 default in ~70 ms)
const WALLPAPER_WAIT_MS = 2000;

export default class VibeosExtension extends Extension {
    enable() {
        // ~/.config/vibeos/debug: the shell's Eval and Screenshot D-Bus calls are open to every client, for
        // development only
        const debug = GLib.build_filenamev([GLib.get_user_config_dir(), 'vibeos', 'debug']);
        if (GLib.file_test(debug, GLib.FileTest.EXISTS))
            global.context.unsafe_mode = true;

        this._keepThroughLock();

        // Android apps' keyboard requests open GNOME's on-screen keyboard, docked as on the phone; without it the rest
        // still comes up
        try {
            this._androidKeyboard = new AndroidKeyboard();
        } catch (e) {
            logError(e, 'vibeos: the on-screen keyboard for Android apps');
            this._androidKeyboard = null;
        }
        // a text field a tap focuses opens the keyboard (WebKit's page fields never ask for it themselves)
        try {
            this._touchFocusKeyboard = new TouchFocusKeyboard();
        } catch (e) {
            logError(e, 'vibeos: the keyboard for a tapped text field');
            this._touchFocusKeyboard = null;
        }

        // the phone's rules for GTK 4 apps (gtkcss.js), taken out again on the docked desktop
        try {
            syncGtkCss(!isDesktopSession());
        } catch (e) {
            logError(e, 'vibeos: the GTK apps\' stylesheet');
        }

        // docked to a display: GNOME's own desktop there, and none of the phone's UI (dock.js)
        if (isDesktopSession()) {
            this._desktop = new Desktop();
            return;
        }

        // the on-screen keyboard: VibeOS's own in place of GNOME's while vibeos.Shell keyboard is 'vibeos'
        try {
            this._keyboardSwap = new KeyboardSwap();
            this._keyboardSwap.connect('changed', () => this._keyHaptics?.pause(this._keyboardSwap.active));
        } catch (e) {
            logError(e, 'vibeos: the on-screen keyboard');
            this._keyboardSwap = null;
        }

        // The session starts on the lock screen. Until it is up the shell draws nothing and the boot splash stays on
        // the screen, where the shell would otherwise show its overview and top panel for the seconds this takes
        // (holdStartup() is patches/gnome-shell/0004; a shell without it shows them). The shell stops waiting after
        // 15 s, a phone held sideways that long included.
        this._startupHold = Main.layoutManager.holdStartup?.() ?? null;

        // the lock screen and home are portrait, the session starts locked, and everything is laid out for an upright
        // panel: a shell started sideways turns upright first
        this._rotation = new Rotation();
        this._rotation.hold();
        this._buildId = this._rotation.whenUpright(() => {
            this._buildId = 0;
            this._build();
        });
    }

    // The VibeOS session turns the extension on through its session mode (vibeos.json). The unlock-dialog
    // mode the lock screen pushes over it lists no extensions of its own, so the shell would turn this one off at
    // every lock, its lock screen with it, and on again as that pops the mode: a loop. While the session is locked,
    // the extension stays among the mode's. (Turned on in the user's settings instead, it stays on by its
    // session-modes alone.)
    _keepThroughLock() {
        const manager = Main.extensionManager;
        if (!Main.sessionMode.enabledExtensions?.includes(this.uuid) || Object.hasOwn(manager, '_getModeExtensions'))
            return;
        const uuid = this.uuid;
        manager._getModeExtensions = function () {
            const extensions = Object.getPrototypeOf(this)._getModeExtensions.call(this);
            return Main.sessionMode.isLocked && !extensions.includes(uuid) ? [...extensions, uuid] : extensions;
        };
        this._keptThroughLock = true;
    }

    _build() {
        this._gc = new CollectWhenStill();
        this._mipmaps = new NoWindowMipmaps();
        this._home = new Home(this);
        this._service = new Service(this._home);

        // the system UI: the status bar in place of the shell's top panel
        this._state = new SystemState();
        this._restorePanel = hideShellPanel();
        // the camera hole: under the status bar upright, on a short edge turned, where apps keep clear of it
        this._cutout = new Cutout();
        this._statusBar = new StatusBar(this._state, this._home, this._cutout);
        this._cutoutBand = new CutoutBand(this._cutout, this._statusBar);
        Main.layoutManager.addChrome(this._cutoutBand, {affectsStruts: true});
        overWindows(this._cutoutBand);
        Main.layoutManager.addChrome(this._statusBar, {affectsStruts: true});
        overWindows(this._statusBar);
        // ... and the notification shade pulled from it, above everything
        this._shade = new Shade(this._state, this._statusBar);
        Main.layoutManager.addTopChrome(this._shade);
        overWindows(this._shade);
        // heads-up notifications in place of GNOME's banners, over the shade too: it takes them along as it opens
        this._headsUp = new HeadsUp(this._shade);
        Main.layoutManager.addTopChrome(this._headsUp.layer);
        overWindows(this._headsUp.layer);
        // the lock screen, over them all (the heads-ups hold back while it is up)
        this._keyguard = new Keyguard(this._state);
        Main.layoutManager.addTopChrome(this._keyguard.actor);
        overWindows(this._keyguard.actor);
        this._service.setKeyguard(this._keyguard);
        // an app's window over the lock screen: the ringing alarm, a call coming in
        this._whenLocked = new WhenLocked(this._keyguard);
        this._rotation.follow(this._keyguard, this._home);
        // the shell's "Keep these display settings?" on the built-in panel, where a finger can answer it
        this._displayChange = new DisplayChangeOnPanel();
        // the volume keys show the volume dialog, over the shade and the lock screen too
        this._volume = new VolumeDialog();
        // ... unless the app in front took them (a ringing timer stops on one)
        this._volume.keysTaker = () => this._service.volumeKeysTaker();
        Main.layoutManager.addTopChrome(this._volume);
        overWindows(this._volume);
        // power and volume down together: a screenshot
        this._chord = new ScreenshotChord();
        this._volume.chord = this._chord;
        // the power menu a long press of the power key opens, over the lock screen too
        this._powerMenu = new PowerMenu({
            secure: () => this._keyguard._secure,
            lockdown: () => this._keyguard.lockdown(),
        });
        Main.layoutManager.addTopChrome(this._powerMenu.layer);
        overWindows(this._powerMenu.layer);
        // the screen off, the AOD or black, over everything: the power key, the double tap, the timeout
        this._power = new Power(this._keyguard, this._state, this._powerMenu, this._chord);
        Main.layoutManager.addTopChrome(this._power.layer);
        overWindows(this._power.layer);
        this._service.setPower(this._power);
        // ... and while it is off, the volume keys do nothing (an app that took them still gets them: a call)
        this._volume.screenOff = () => this._power && !this._power.interactive;
        // the Bixby key: recents, with the screen on (the lock screen's own action mode keeps it out there)
        this._appSwitch = new AppSwitchKey(() => {
            if (this._power.interactive)
                this._service.toggleRecents();
        });
        // ... and over them all, the end of uiGroup
        this._endMark = new EndMark();
        // GNOME's on-screen keyboard's keys tick under the finger (paused while VibeOS's keyboard is in use)
        this._keyHaptics = new KeyHaptics();
        this._keyHaptics.pause(this._keyboardSwap?.active ?? false);
        // The shell's haptics never start feedbackd (NO_AUTO_START), they only find it running: Calls' and Chatty's
        // daemons started it at login, the Phone and Messaging apps in their place only on a ring. So it starts here.
        Gio.DBus.session.call('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
            'StartServiceByName', new GLib.Variant('(su)', ['org.sigxcpu.Feedback', 0]), null,
            Gio.DBusCallFlags.NONE, -1, null, null);
        // locked from the start, as a phone after boot, once the extension system is done with this
        this._lockId = GLib.idle_add(GLib.PRIORITY_DEFAULT, () => {
            this._lockId = 0;
            try {
                this._keyguard.lock();
            } finally {
                // ... and the first frame the shell draws is the lock screen, with its wallpaper (the keyguard decodes
                // it in a worker thread)
                this._releaseStartupWithWallpaper();
            }
            return GLib.SOURCE_REMOVE;
        });
    }

    _releaseStartupWithWallpaper() {
        const hold = this._startupHold;
        if (!hold)
            return;
        this._startupWaitId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, WALLPAPER_WAIT_MS, () => {
            this._startupWaitId = 0;
            this._releaseStartup();
            return GLib.SOURCE_REMOVE;
        });
        this._keyguard.wallpaperLoaded.then(() => {
            if (this._startupHold === hold)
                this._releaseStartup();
        });
    }

    _releaseStartup() {
        if (this._startupWaitId)
            GLib.source_remove(this._startupWaitId);
        this._startupWaitId = 0;
        this._startupHold?.();
        this._startupHold = null;
    }

    disable() {
        if (this._keptThroughLock)
            delete Main.extensionManager._getModeExtensions;
        this._keptThroughLock = false;
        this._androidKeyboard?.destroy();
        this._androidKeyboard = null;
        this._touchFocusKeyboard?.destroy();
        this._touchFocusKeyboard = null;
        this._desktop?.destroy();
        this._desktop = null;
        this._rotation?.cancelWait(this._buildId);
        this._buildId = 0;
        if (this._lockId)
            GLib.source_remove(this._lockId);
        this._lockId = 0;
        this._releaseStartup();
        this._keyHaptics?.destroy();
        this._keyHaptics = null;
        this._keyboardSwap?.destroy();
        this._keyboardSwap = null;
        this._endMark?.destroy();
        this._endMark = null;
        this._appSwitch?.destroy();
        this._appSwitch = null;
        this._power?.destroy();
        this._power = null;
        this._powerMenu?.destroy();
        this._powerMenu = null;
        this._volume?.destroy();
        this._volume = null;
        this._displayChange?.destroy();
        this._displayChange = null;
        this._chord?.destroy();
        this._chord = null;
        this._whenLocked?.destroy();
        this._whenLocked = null;
        this._keyguard?.destroy();
        this._keyguard = null;
        this._headsUp?.destroy();
        this._headsUp = null;
        this._shade?.destroy();
        this._shade = null;
        this._cutoutBand?.destroy();
        this._cutoutBand = null;
        this._statusBar?.destroy();
        this._statusBar = null;
        this._cutout?.destroy();
        this._cutout = null;
        this._restorePanel?.();
        this._restorePanel = null;
        this._state?.destroy();
        this._state = null;

        this._service?.destroy();
        this._service = null;
        this._home?.destroy();
        this._home = null;
        this._mipmaps?.destroy();
        this._mipmaps = null;
        this._gc?.destroy();
        this._gc = null;
        this._rotation?.destroy();
        this._rotation = null;
        // every actor that held the display composited is gone by now
        restoreUnredirect();
    }
}
