// The keyboard's colours: the AOSP keyboard's Material You roles, taken from the system theme vibeos.Theme stores
// (the tonal palettes' stops in light mode, the scheme's roles in dark), light or dark as the system is.
import Gio from 'gi://Gio';

const THEME_SCHEMA = 'vibeos.Theme';
const INTERFACE_SCHEMA = 'org.gnome.desktop.interface';
const STOPS = [0, 10, 50, 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000];

// each colour of the keyboard: light from a palette's stop, dark from a role of the dark scheme
const LIGHT = {
    background: ['neutral1', 50],
    letter: ['neutral1', 0],
    letterPressed: ['neutral1', 100],
    functional: ['accent2', 100],
    functionalPressed: ['neutral2', 200],
    action: ['accent1', 200],
    actionPressed: ['accent1', 400],
    text: ['neutral1', 900],
    functionalText: ['neutral1', 900],
    icon: ['neutral1', 900],
    hint: ['neutral1', 400],
    tool: ['accent1', 500],
};
const DARK = {
    background: 'surface_container',
    letter: 'surface_container_highest',
    letterPressed: 'surface_container',
    functional: 'secondary_container',
    functionalPressed: 'on_secondary',
    action: 'secondary_container',
    actionPressed: 'on_secondary',
    text: 'on_surface',
    functionalText: 'on_secondary_container',
    icon: 'on_secondary_container',
    hint: 'on_surface_variant',
    tool: ['accent1', 500],
};

// a system whose theme was never stored: vibeos.Theme's default seed's colours
const FALLBACK = {
    light: {
        background: 0xedf1f2, letter: 0xffffff, letterPressed: 0xdfe3e4, functional: 0xcde7ec,
        functionalPressed: 0xbec8ca, action: 0x9ccfd7, actionPressed: 0x6799a0, text: 0x181c1d,
        functionalText: 0x181c1d, icon: 0x181c1d, hint: 0x8d9192, tool: 0x4d7f86,
    },
    dark: {
        background: 0x131b1c, letter: 0x1e2729, letterPressed: 0x131b1c, functional: 0x273f43,
        functionalPressed: 0x2c4448, action: 0x273f43, actionPressed: 0x2c4448, text: 0xdde7e9,
        functionalText: 0xaac4c8, icon: 0xaac4c8, hint: 0xa2adaf, tool: 0x4d7f86,
    },
};

const rgb = v => [(v >> 16) & 255, (v >> 8) & 255, v & 255];

// The keyboard's colours ([r, g, b] 0..255) from a stored theme: @palettes {name: [stop colours]} of the mode,
// @roles {name: 0xAARRGGBB} of the mode; null where the theme lacks one
export function keyboardColors(palettes, roles, dark) {
    const table = dark ? DARK : LIGHT;
    const out = {};
    for (const [name, source] of Object.entries(table)) {
        let v;
        if (Array.isArray(source))
            v = palettes?.[source[0]]?.[STOPS.indexOf(source[1])];
        else
            v = roles?.[source];
        if (v === undefined)
            return null;
        out[name] = rgb(v);
    }
    return withFixed(out, dark);
}

// the colours that are not the theme's: the strip's dividers white at 15 %, the navigation bar's icons and handle
function withFixed(c, dark) {
    return {
        ...c,
        divider: [[255, 255, 255], 0.15],
        navIcon: dark ? [[255, 255, 255], 1] : [[0, 0, 0], 0.6],
        dark,
    };
}

export function fallbackColors(dark) {
    const out = {};
    for (const [name, v] of Object.entries(FALLBACK[dark ? 'dark' : 'light']))
        out[name] = rgb(v);
    return withFixed(out, dark);
}

function lookup(id) {
    const schema = Gio.SettingsSchemaSource.get_default()?.lookup(id, true);
    return schema ? new Gio.Settings({settings_schema: schema}) : null;
}

// The colours now, and @onChanged() whenever the theme or light and dark change
export class KeyboardPalette {
    constructor(onChanged) {
        this._theme = lookup(THEME_SCHEMA);
        this._interface = lookup(INTERFACE_SCHEMA);
        this._ids = [];
        const changed = () => {
            this._colors = null;
            onChanged();
        };
        if (this._theme)
            this._ids.push([this._theme, this._theme.connect('changed', changed)]);
        if (this._interface)
            this._ids.push([this._interface, this._interface.connect('changed::color-scheme', changed)]);
        this._colors = null;
    }

    // dark unless the system asks for light (VibeOS defaults to dark; GNOME's "default" is its light)
    get dark() {
        return this._interface ? this._interface.get_string('color-scheme') === 'prefer-dark' : true;
    }

    get colors() {
        if (this._colors)
            return this._colors;
        const dark = this.dark;
        let colors = null;
        if (this._theme) {
            const mode = dark ? 'dark' : 'light';
            const palettes = this._theme.get_value(`${mode}-palettes`).deepUnpack();
            const roles = this._theme.get_value(`${mode}-roles`).deepUnpack();
            colors = keyboardColors(palettes, roles, dark);
        }
        this._colors = colors ?? fallbackColors(dark);
        return this._colors;
    }

    destroy() {
        for (const [settings, id] of this._ids)
            settings.disconnect(id);
        this._ids = [];
    }
}
