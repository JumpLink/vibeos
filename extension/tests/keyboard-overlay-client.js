// keyboard-overlay.js's window: a full-height window with a field at its top (a search field) and one at its bottom (a
// chat's compose box). Run as `gjs -m keyboard-overlay-client.js APP_ID`. Says what the window sees of its own size:
// "layout W H" for each size the toplevel is given (every configure that changes it), "alloc W H" for each size of
// its widgets, and "field top|bottom has the focus". Driven by lines on stdin: focus-top, focus-bottom, unfocus, bounds (prints
// "bounds top|bottom X Y W H", the field's box in the window), quit. Says "client up" once its window is shown.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Gtk from 'gi://Gtk?version=4.0';
import System from 'system';

const app = new Gtk.Application({application_id: System.programArgs[0] ?? 'test.vibeos.KeyboardOverlay'});

app.connect('activate', () => {
    const win = new Gtk.ApplicationWindow({application: app, title: 'keyboard-overlay'});
    const box = new Gtk.Box({orientation: Gtk.Orientation.VERTICAL, margin_top: 24, margin_bottom: 24,
        margin_start: 24, margin_end: 24});
    const top = new Gtk.Entry({placeholder_text: 'search'});
    const bottom = new Gtk.Entry({placeholder_text: 'message'});
    box.append(top);
    const page = new Gtk.Button({label: 'content', vexpand: true, focusable: true});
    box.append(page);
    box.append(bottom);
    win.set_child(box);
    for (const [name, entry] of [['top', top], ['bottom', bottom]]) {
        entry.connect('notify::has-focus', () => {
            if (entry.has_focus)
                print(`field ${name} has the focus`);
        });
    }
    // (a layout of the size it has already is no new size)
    let layout = '';
    win.connect('realize', () => win.get_surface().connect('layout', (_s, w, h) => {
        if (`${w} ${h}` !== layout) {
            layout = `${w} ${h}`;
            print(`layout ${layout}`);
        }
    }));
    let size = '';
    win.add_tick_callback(() => {
        const now = `${win.get_width()} ${win.get_height()}`;
        if (now !== size) {
            size = now;
            print(`alloc ${now}`);
        }
        return GLib.SOURCE_CONTINUE;
    });
    win.maximize();
    win.present();
    // no field has the focus until one is tapped (the first entry takes it, and a tap on a field that has it opens no
    // keyboard): the page has it
    page.grab_focus();
    print('client up');

    const input = new Gio.DataInputStream({base_stream: new Gio.UnixInputStream({fd: 0})});
    const read = () => input.read_line_async(GLib.PRIORITY_DEFAULT, null, (stream, res) => {
        const [line] = stream.read_line_finish_utf8(res);
        switch (line?.trim()) {
        case 'focus-top':
            top.grab_focus();
            break;
        case 'focus-bottom':
            bottom.grab_focus();
            break;
        case 'unfocus':
            page.grab_focus();
            break;
        case 'bounds':
            for (const [name, entry] of [['top', top], ['bottom', bottom]]) {
                const [, r] = entry.compute_bounds(win);
                print(`bounds ${name} ${r.get_x()} ${r.get_y()} ${r.get_width()} ${r.get_height()}`);
            }
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
