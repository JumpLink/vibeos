// de.yesman.GnomePhone on the session bus: what the launcher asks of the shell.
import Gio from 'gi://Gio';
import Shell from 'gi://Shell';

import {LaunchTransition} from './launch.js';

const IFACE = `<node>
  <interface name="de.yesman.GnomePhone">
    <!-- start an app; from = the icon's rectangle on screen, in pixels, the window grows out of it; icon = the
         launcher's picture of the icon (a PNG, for the splash), or '' for the app's themed icon -->
    <method name="Launch">
      <arg type="s" name="app_id" direction="in"/>
      <arg type="(dddd)" name="from" direction="in"/>
      <arg type="s" name="icon" direction="in"/>
    </method>
  </interface>
</node>`;

export class Service {
    constructor(home) {
        this._home = home;
        this._launch = new LaunchTransition();
        this._impl = Gio.DBusExportedObject.wrapJSObject(IFACE, this);
        this._impl.export(Gio.DBus.session, '/de/yesman/GnomePhone');
        this._nameId = Gio.bus_own_name(Gio.BusType.SESSION, 'de.yesman.GnomePhone',
            Gio.BusNameOwnerFlags.REPLACE, null, null, null);
    }

    Launch(appId, from, icon) {
        const app = Shell.AppSystem.get_default().lookup_app(appId);
        if (!app) {
            console.warn(`gnome-phone: no app ${appId}`);
            return;
        }
        this._launch.launch(app, from, icon);
    }

    destroy() {
        this._launch.destroy();
        Gio.bus_unown_name(this._nameId);
        this._impl.unexport();
    }
}
