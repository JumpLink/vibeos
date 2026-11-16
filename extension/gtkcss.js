// The phone's fixes to other projects' GTK 4 apps, as CSS in the user's ~/.config/gtk-4.0/gtk.css: the one stylesheet
// every GTK 4 app loads, libadwaita's included (they ignore GTK themes). Our rules sit between two marker comments at
// the end of the file, rewritten only when they differ and taken out on the docked desktop; the rest is the user's.
// GTK reads the file as an app starts: an app already running keeps the rules it started with.
import GLib from 'gi://GLib';

const BEGIN = '/* vibeos: begin. Written by the vibeos shell extension, edits in here are lost */';
const END = '/* vibeos: end */';

// The file picker GNOME's portal shows (Nautilus's) heads its sidebar with the asking app's title: "Open Image" from
// Image Viewer. On a phone the sidebar is a page of its own, and the title read as the name of the sidebar. Libadwaita
// names the sidebar .overlay-pane while it slides over the files (upright) and .sidebar-pane beside them (sideways)
const PHONE_CSS = `window.nautilus-file-chooser .overlay-pane headerbar label,
window.nautilus-file-chooser .sidebar-pane headerbar label {
  opacity: 0;
}`;

// phone: the phone's session (the rules in), or the docked desktop (the rules out)
export function syncGtkCss(phone) {
    const dir = GLib.build_filenamev([GLib.get_user_config_dir(), 'gtk-4.0']);
    const path = GLib.build_filenamev([dir, 'gtk.css']);
    let text = '';
    if (GLib.file_test(path, GLib.FileTest.EXISTS))
        text = new TextDecoder().decode(GLib.file_get_contents(path)[1]);

    const begin = text.indexOf(BEGIN);
    const end = begin >= 0 ? text.indexOf(END, begin) : -1;
    const user = end >= 0 ? text.slice(0, begin) + text.slice(end + END.length).replace(/^\n/, '') : text;
    // after the user's rules: an @import of theirs only works before every rule
    const next = phone ? `${user}${user && !user.endsWith('\n') ? '\n' : ''}${BEGIN}\n${PHONE_CSS}\n${END}\n` : user;
    if (next === text)
        return;
    GLib.mkdir_with_parents(dir, 0o700);
    GLib.file_set_contents(path, next);
}
