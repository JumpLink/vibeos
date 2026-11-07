// GNOME Phone: turns GNOME Shell (mobile) into the phone the Neo launcher is the home screen of.
import GLib from 'gi://GLib';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

import {ScreenshotChord} from './chord.js';
import {CollectWhenStill} from './gc.js';
import {EndMark} from './endmark.js';
import {HeadsUp} from './headsup.js';
import {Home} from './home.js';
import {KeyHaptics} from './keyhaptics.js';
import {Keyguard} from './keyguard.js';
import {NoWindowMipmaps} from './mipmaps.js';
import {overWindows} from './scanout.js';
import {Power} from './power.js';
import {PowerMenu} from './powermenu.js';
import {Service} from './service.js';
import {Shade} from './shade.js';
import {SystemState} from './state.js';
import {StatusBar, hideShellPanel} from './statusbar.js';
import {VolumeDialog} from './volume.js';
import {WhenLocked} from './whenlocked.js';

export default class GnomePhoneExtension extends Extension {
    enable() {
        // ~/.config/gnome-phone/debug: the shell's Eval and Screenshot D-Bus calls are open to every client, for
        // development only
        const debug = GLib.build_filenamev([GLib.get_user_config_dir(), 'gnome-phone', 'debug']);
        if (GLib.file_test(debug, GLib.FileTest.EXISTS))
            global.context.unsafe_mode = true;

        this._gc = new CollectWhenStill();
        this._mipmaps = new NoWindowMipmaps();
        this._home = new Home(this);
        this._service = new Service(this._home);

        // Android's SystemUI: the status bar in place of the shell's top panel
        this._state = new SystemState();
        this._restorePanel = hideShellPanel();
        this._statusBar = new StatusBar(this._state, this._home);
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
        // ... and over them all, the end of uiGroup
        this._endMark = new EndMark();
        // the on-screen keyboard's keys tick under the finger
        this._keyHaptics = new KeyHaptics();
        // locked from the start, as a phone after boot, once the extension system is done with this
        this._lockId = GLib.idle_add(GLib.PRIORITY_DEFAULT, () => {
            this._lockId = 0;
            this._keyguard.lock();
            return GLib.SOURCE_REMOVE;
        });
    }

    disable() {
        if (this._lockId)
            GLib.source_remove(this._lockId);
        this._lockId = 0;
        this._keyHaptics?.destroy();
        this._keyHaptics = null;
        this._endMark?.destroy();
        this._endMark = null;
        this._power?.destroy();
        this._power = null;
        this._powerMenu?.destroy();
        this._powerMenu = null;
        this._volume?.destroy();
        this._volume = null;
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
        this._statusBar?.destroy();
        this._statusBar = null;
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
    }
}
