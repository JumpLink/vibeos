// back-probe.sh's app: a libadwaita window in the language its environment says, at its first page or with a second
// one pushed (its header bar's back button showing). BACKPROBE_OWN=1 adds a button of its own named "Back" in the
// vibeos-backtest catalog (as VibeOS's apps name theirs with their own catalogs), bound to BACKPROBE_LOCALEDIR.
import Adw from 'gi://Adw?version=1';
import GLib from 'gi://GLib';
import Gtk from 'gi://Gtk?version=4.0';

const pushed = GLib.getenv('BACKPROBE_PAGE') === 'pushed';
const own = GLib.getenv('BACKPROBE_OWN') === '1';
if (own)
    imports.gettext.bindtextdomain('vibeos-backtest', GLib.getenv('BACKPROBE_LOCALEDIR'));

const app = new Adw.Application({application_id: 'test.vibeos.BackProbe'});
app.connect('activate', () => {
    const win = new Adw.ApplicationWindow({application: app, default_width: 360, default_height: 640});
    const nav = new Adw.NavigationView();
    const page = (title, header) => {
        const view = new Adw.ToolbarView();
        view.add_top_bar(header);
        view.set_content(new Gtk.Label({label: title}));
        return new Adw.NavigationPage({title, child: view, tag: title});
    };
    const header = new Adw.HeaderBar();
    if (own) {
        const button = new Gtk.Button({icon_name: 'go-previous-symbolic'});
        button.update_property([Gtk.AccessibleProperty.LABEL], [GLib.dgettext('vibeos-backtest', 'Back')]);
        header.pack_start(button);
    }
    nav.add(page('One', header));
    win.set_content(nav);
    win.present();
    GLib.timeout_add(GLib.PRIORITY_DEFAULT, 200, () => {
        if (pushed)
            nav.push(page('Two', new Adw.HeaderBar()));
        // the accessibility tree a moment to follow
        GLib.timeout_add(GLib.PRIORITY_DEFAULT, 500, () => {
            print('READY');
            return GLib.SOURCE_REMOVE;
        });
        return GLib.SOURCE_REMOVE;
    });
});
app.run([]);
