// Accessibility's High contrast in the shell: org.gnome.desktop.a11y.interface high-contrast, which Settings'
// "High contrast" switch writes. GNOME Shell's own stylesheet already follows it (main.js loads its high-contrast
// variant: the polkit and system dialogs); this switches the colours the extension draws with (palette.js
// setHighContrast) and has everything that draws them draw again at once. A colour a module baked into an actor's
// style when it was made (a rgba() string) takes effect when that actor is made again.
import Gio from 'gi://Gio';
import St from 'gi://St';

import * as Signals from 'resource:///org/gnome/shell/misc/signals.js';

import {setHighContrast} from './palette.js';

const SCHEMA = 'org.gnome.desktop.a11y.interface';
const KEY = 'high-contrast';

// every drawing area under `actor` drawn again, and every actor painted again
function repaintAll(actor) {
    if (actor instanceof St.DrawingArea)
        actor.queue_repaint();
    else
        actor.queue_redraw();
    for (const child of actor.get_children())
        repaintAll(child);
}

export class HighContrast extends Signals.EventEmitter {
    constructor() {
        super();
        const schema = Gio.SettingsSchemaSource.get_default()?.lookup(SCHEMA, true);
        if (!schema?.has_key(KEY))
            return;
        this._settings = new Gio.Settings({settings_schema: schema});
        this._settings.connectObject(`changed::${KEY}`, () => this._sync(), this);
        this._sync();
    }

    _sync() {
        if (!setHighContrast(this._settings.get_boolean(KEY)))
            return;
        repaintAll(global.stage);
        this.emit('changed');
    }

    destroy() {
        this._settings?.disconnectObject(this);
        setHighContrast(false);
    }
}
