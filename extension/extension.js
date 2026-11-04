// GNOME Phone: turns GNOME Shell (mobile) into the phone the Neo launcher is the home screen of.
import GLib from 'gi://GLib';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

import {Home} from './home.js';
import {Service} from './service.js';

export default class GnomePhoneExtension extends Extension {
    enable() {
        // ~/.config/gnome-phone/debug: the shell's Eval and Screenshot D-Bus calls are open to every client, for
        // development only
        const debug = GLib.build_filenamev([GLib.get_user_config_dir(), 'gnome-phone', 'debug']);
        if (GLib.file_test(debug, GLib.FileTest.EXISTS))
            global.context.unsafe_mode = true;

        this._home = new Home(this);
        this._service = new Service(this._home);
    }

    disable() {
        this._service?.destroy();
        this._service = null;
        this._home?.destroy();
        this._home = null;
    }
}
