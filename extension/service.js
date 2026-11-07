// de.yesman.GnomePhone on the session bus: what the launcher asks of the shell.
import Gio from 'gi://Gio';
import Shell from 'gi://Shell';

import {LaunchTransition} from './launch.js';
import {Navigation} from './navigation.js';

const IFACE = `<node>
  <interface name="de.yesman.GnomePhone">
    <!-- start an app; from = the icon's rectangle on screen, in pixels, the window grows out of it; icon = the
         launcher's picture of the icon (a PNG, for the splash), or '' for the app's themed icon -->
    <method name="Launch">
      <arg type="s" name="app_id" direction="in"/>
      <arg type="(dddd)" name="from" direction="in"/>
      <arg type="s" name="icon" direction="in"/>
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
         Over the lock screen it shows if its desktop file says X-GnomePhone-ShowWhenLocked=true (whenlocked.js) -->
    <method name="LaunchFullScreen">
      <arg type="s" name="app_id" direction="in"/>
      <arg type="b" name="started" direction="out"/>
    </method>
  </interface>
</node>`;

export class Service {
    constructor(home) {
        this._home = home;
        this._launch = new LaunchTransition();
        this._nav = new Navigation(home);
        this._nav.onGestureBegin = () => this._launch.cancelSplash();
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
        const app = Shell.AppSystem.get_default().lookup_app(appId);
        if (!app) {
            console.warn(`gnome-phone: no app ${appId} to launch full-screen`);
            return false;
        }
        app.activate();
        this._power?.wakeUp('full-screen');
        return true;
    }

    destroy() {
        this._launch.destroy();
        this._nav.destroy();
        Gio.bus_unown_name(this._nameId);
        this._impl.unexport();
    }
}
