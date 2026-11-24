// An app's logo as a notification shows it: the app's own icon in a circle. The shade's rows and heads-ups draw it
// beside the text, the status bar after the clock.
import Clutter from 'gi://Clutter';
import Cogl from 'gi://Cogl';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import St from 'gi://St';

import {SCREEN_SCALE} from './units.js';

// The app a notification source is for, or null: a source of org.freedesktop.Notifications keeps it in app, one of
// org.gtk.Notifications (an app's GNotification, the portal's for a sandboxed app) in _app
export function sourceApp(source) {
    return source?.app ?? source?._app ?? null;
}

// An app's icon is cut to a circle, as Android 16+ masks every app's launcher icon in a notification
// (notificationsRedesignAppIcons). A VibeOS icon is a colour tile with a white glyph, so the circle out of it is the
// coloured disc with the glyph that Lineage shows; drawn as it is, it was a square among round icons.
const CIRCLE_DECLARATIONS = `
uniform vec4 bounds;    // the offscreen texture in actor coordinates
uniform vec3 circle;    // centre x, y and radius, in actor coordinates
uniform float aa;       // actor units per screen pixel
`;

const CIRCLE_CODE = `
vec2 p = bounds.xy + cogl_tex_coord_in[0].xy * bounds.zw;
cogl_color_out *= clamp(0.5 - (length(p - circle.xy) - circle.z) / aa, 0.0, 1.0);
`;

const CircleMaskEffect = GObject.registerClass(
class CircleMaskEffect extends Clutter.ShaderEffect {
    vfunc_get_static_snippet() {
        return Cogl.Snippet.new(Cogl.SnippetHook.FRAGMENT, CIRCLE_DECLARATIONS, CIRCLE_CODE);
    }

    // @size: the square actor's side; @diameter: the circle's, centred in it
    setGeometry(size, diameter) {
        // the offscreen texture is the paint box enlarged the way _clutter_actor_box_enlarge_for_effects does
        // (whole pixels, 3 px of slack), as launch.js's CropEffect measures it
        const s = Math.round(size), far = Math.ceil(size + 0.75);
        this.set_uniform_float('bounds', 4, [far - s - 3, far - s - 3, s + 3, s + 3]);
        this.set_uniform_float('circle', 3, [size / 2, size / 2, diameter / 2]);
        this.set_uniform_float('aa', 1, [1 / SCREEN_SCALE]);
        this.queue_repaint();
    }
});

let iconTheme = null;
const tileCover = new Map();

// How much of its box an app icon's tile covers, the circle being cut from inside the tile: all of it for VibeOS's
// own icons (a rounded square the size of the viewBox, drawn first), less for one inset as the GNOME icon grid
// (data-vibeos-inset or the older data-neo-inset, scale 0.86: Software's, and some store apps'). null: not a tile, an
// icon of any shape.
function tileCoverOf(gicon) {
    const key = gicon?.to_string?.() ?? '';
    if (tileCover.has(key))
        return tileCover.get(key);
    let cover = null;
    try {
        let path = null;
        if (gicon instanceof Gio.FileIcon) {
            path = gicon.get_file().get_path();
        } else if (gicon) {
            iconTheme ??= new St.IconTheme();
            path = iconTheme.lookup_by_gicon(gicon, 128, St.IconLookupFlags.FORCE_SVG)?.get_filename() ?? null;
        }
        if (path?.endsWith('.svg')) {
            const head = new TextDecoder().decode(GLib.file_get_contents(path)[1].subarray(0, 4096));
            const inset = head.match(/data-(?:vibeos|neo)-inset[^>]*?scale\(([0-9.]+)\)/);
            const box = head.match(/viewBox="0 0 ([0-9.]+) ([0-9.]+)"/);
            const first = head.match(/<(rect|circle|ellipse|path|g|use|image)\b([^>]*)>/);
            const attr = (name) => Number(first?.[2].match(new RegExp(`\\b${name}="([0-9.]+)"`))?.[1] ?? NaN);
            if (inset)
                cover = Number(inset[1]) || null;
            else if (box && first?.[1] === 'rect' && !(attr('x') > 0) && !(attr('y') > 0) &&
                     attr('width') === Number(box[1]) && attr('height') === Number(box[2]))
                cover = 1;
        }
    } catch (e) {
        logError(e, `notification icon ${key}`);
    }
    tileCover.set(key, cover);
    return cover;
}

// The app's icon in a circle of @size: a VibeOS tile enlarged until the circle fills with it; any other icon
// (a third-party one, whatever its shape) smaller on a white disc, as Android shows a legacy launcher icon
export function appCircle(gicon, size) {
    const box = new St.Widget({width: size, height: size});
    const cover = tileCoverOf(gicon);
    if (cover) {
        const side = Math.round(size / cover);
        const icon = new St.Icon({gicon, icon_size: side, width: side, height: side});
        icon.set_position((size - side) / 2, (size - side) / 2);
        const mask = new CircleMaskEffect();
        icon.add_effect(mask);
        mask.setGeometry(side, size);
        box.add_child(icon);
    } else {
        box.style = `background-color: white; border-radius: ${(size / 2).toFixed(2)}px;`;
        const side = Math.round(size * 0.72);
        const icon = new St.Icon({gicon, icon_size: side});
        icon.set_position((size - side) / 2, (size - side) / 2);
        box.add_child(icon);
    }
    return box;
}
