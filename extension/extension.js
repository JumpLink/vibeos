// GNOME Phone: turns GNOME Shell (mobile) into the phone the Neo launcher is the home screen of.
import GLib from 'gi://GLib';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

import {Home} from './home.js';
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

        this._home = new Home(this);
        this._service = new Service(this._home);

        // Android's SystemUI: the status bar in place of the shell's top panel
        this._state = new SystemState();
        this._restorePanel = hideShellPanel();
        this._statusBar = new StatusBar(this._state, this._home);
        Main.layoutManager.addChrome(this._statusBar, {affectsStruts: true});
        // ... and the notification shade pulled from it, above everything
        this._shade = new Shade(this._state, this._statusBar);
        Main.layoutManager.addTopChrome(this._shade);
        // the volume keys show the volume dialog, over the shade too
        this._volume = new VolumeDialog();
        Main.layoutManager.addTopChrome(this._volume);
    }

    disable() {
        this._volume?.destroy();
        this._volume = null;
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
    }
}
