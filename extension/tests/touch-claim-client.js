// touch-claim.js's app: a fullscreen window that says what it is given of every touch ("client touch begin", update,
// end, cancel), as an app's own swipe or tap would take it. Says "client up" once its window is shown; quits on any
// line on stdin.
import Gdk from 'gi://Gdk?version=4.0';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Gtk from 'gi://Gtk?version=4.0';

const app = new Gtk.Application({application_id: 'test.vibeos.TouchClaim'});
const NAMES = new Map([
    [Gdk.EventType.TOUCH_BEGIN, 'begin'],
    [Gdk.EventType.TOUCH_UPDATE, 'update'],
    [Gdk.EventType.TOUCH_END, 'end'],
    [Gdk.EventType.TOUCH_CANCEL, 'cancel'],
]);

app.connect('activate', () => {
    const win = new Gtk.ApplicationWindow({application: app, title: 'touch-claim'});
    win.set_child(new Gtk.Label({label: 'touch-claim', vexpand: true, hexpand: true}));
    const touches = new Gtk.EventControllerLegacy({propagation_phase: Gtk.PropagationPhase.CAPTURE});
    touches.connect('event', (_c, event) => {
        const name = NAMES.get(event.get_event_type());
        if (name)
            print(`client touch ${name}`);
        return false;
    });
    win.add_controller(touches);
    win.fullscreen();
    win.present();
    print('client up');

    const input = new Gio.DataInputStream({base_stream: new Gio.UnixInputStream({fd: 0})});
    input.read_line_async(GLib.PRIORITY_DEFAULT, null, () => app.quit());
});

app.run([]);
