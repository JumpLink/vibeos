// A switch for a development log, flipped from a shell on the device: on while a file of the given name is in the
// session's runtime directory (touch it to start, remove it to stop; the directory goes with the session). Off, all it
// costs is a watch on that directory; a check of `on` is a property read.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

export class FlagFile {
    // onChange(on) after each flip
    constructor(name, onChange = null) {
        this._name = name;
        this._onChange = onChange;
        this._file = Gio.File.new_for_path(GLib.build_filenamev([GLib.get_user_runtime_dir(), name]));
        this.on = this._file.query_exists(null);
        try {
            this._monitor = this._file.get_parent().monitor_directory(Gio.FileMonitorFlags.NONE, null);
            this._monitor.connect('changed', (_m, file) => {
                if (file.get_basename() === this._name)
                    this._sync();
            });
        } catch (e) {
            logError(e, `vibeos: no watch for ${name}`);
            this._monitor = null;
        }
    }

    _sync() {
        const on = this._file.query_exists(null);
        if (on === this.on)
            return;
        this.on = on;
        this._onChange?.(on);
    }

    destroy() {
        this._monitor?.cancel();
        this._monitor = null;
        this._onChange = null;
    }
}
