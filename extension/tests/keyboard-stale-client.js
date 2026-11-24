// keyboard-stale.js's window: a URL field, as the Browser's address bar, and a button the focus can move to (as the
// address bar hands the focus to the page on Enter). Driven by lines on stdin: focus, unfocus, text (prints the
// field's text), quit. Says "client up" once its window is shown.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Gtk from 'gi://Gtk?version=4.0';

const app = new Gtk.Application({application_id: 'test.vibeos.KeyboardStale'});

app.connect('activate', () => {
    const win = new Gtk.ApplicationWindow({application: app, title: 'keyboard-stale'});
    const box = new Gtk.Box({orientation: Gtk.Orientation.VERTICAL});
    const entry = new Gtk.Entry({input_purpose: Gtk.InputPurpose.URL});
    const page = new Gtk.Button({label: 'page', vexpand: true, focusable: true});
    box.append(entry);
    box.append(page);
    win.set_child(box);
    win.maximize();
    win.present();
    print('client up');

    const input = new Gio.DataInputStream({base_stream: new Gio.UnixInputStream({fd: 0})});
    const read = () => input.read_line_async(GLib.PRIORITY_DEFAULT, null, (stream, res) => {
        const [line] = stream.read_line_finish_utf8(res);
        switch (line?.trim()) {
        case 'focus':
            entry.grab_focus();
            break;
        case 'unfocus':
            page.grab_focus();
            break;
        case 'text':
            print(`client text "${entry.get_text()}"`);
            break;
        default:
            app.quit();
            return;
        }
        read();
    });
    read();
});

app.run([]);
