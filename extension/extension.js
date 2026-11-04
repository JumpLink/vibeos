// GNOME Phone: turns GNOME Shell (mobile) into the phone the Neo launcher is the home screen of.
import GLib from 'gi://GLib';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

import {CollectWhenStill} from './gc.js';
import {EndMark} from './endmark.js';
import {HeadsUp} from './headsup.js';
import {Home} from './home.js';
import {NoWindowMipmaps} from './mipmaps.js';
import {overWindows} from './scanout.js';
import {Service} from './service.js';
import {Shade} from './shade.js';
import {SystemState} from './state.js';
import {StatusBar, hideShellPanel} from './statusbar.js';
import {VolumeDialog} from './volume.js';

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
        // the volume keys show the volume dialog, over the shade too
        this._volume = new VolumeDialog();
        Main.layoutManager.addTopChrome(this._volume);
        overWindows(this._volume);
        // ... and over them both, the end of uiGroup
        this._endMark = new EndMark();
    }

    disable() {
        this._endMark?.destroy();
        this._endMark = null;
        this._volume?.destroy();
        this._volume = null;
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
