// de.yesman.GnomePhone on the session bus: what the launcher asks of the shell.
import Gio from 'gi://Gio';
import Shell from 'gi://Shell';

const IFACE = `<node>
  <interface name="de.yesman.GnomePhone">
    <!-- start an app; from = the icon's rectangle on screen, in pixels, the window grows out of it -->
    <method name="Launch">
      <arg type="s" name="app_id" direction="in"/>
      <arg type="(dddd)" name="from" direction="in"/>
    </method>
  </interface>
</node>`;

export class Service {
    constructor(home) {
        this._home = home;
        this._impl = Gio.DBusExportedObject.wrapJSObject(IFACE, this);
        this._impl.export(Gio.DBus.session, '/de/yesman/GnomePhone');
        this._nameId = Gio.bus_own_name(Gio.BusType.SESSION, 'de.yesman.GnomePhone',
            Gio.BusNameOwnerFlags.REPLACE, null, null, null);
    }

    Launch(appId, _from) {
        const app = Shell.AppSystem.get_default().lookup_app(appId);
        if (!app) {
            console.warn(`gnome-phone: no app ${appId}`);
            return;
        }
        app.activate();
    }

    destroy() {
        Gio.bus_unown_name(this._nameId);
        this._impl.unexport();
    }
}
