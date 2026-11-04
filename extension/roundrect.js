// A flat rounded rectangle painted by a shader, each corner its own radius. For shapes whose corners move every frame
// (a notification card as a swipe rounds it): St redraws a whole background with cairo and uploads it again for each
// new border-radius, this only changes two uniforms.
import Clutter from 'gi://Clutter';
import Cogl from 'gi://Cogl';
import GObject from 'gi://GObject';

import {SCALE} from './units.js';

const DECLARATIONS = `
uniform vec2 size;      // the actor's, in its own units
uniform vec4 radii;     // top left, top right, bottom right, bottom left
uniform vec4 color;     // premultiplied, the paint opacity in it
uniform float aa;       // actor units per screen pixel
`;

const CODE = `
vec2 half_size = size * 0.5;
vec2 d0 = cogl_tex_coord_in[0].xy * size - half_size;
float r = d0.x < 0.0 ? (d0.y < 0.0 ? radii.x : radii.w) : (d0.y < 0.0 ? radii.y : radii.z);
vec2 q = abs(d0) - half_size + vec2(r);
float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
cogl_color_out = color * clamp(0.5 - d / aa, 0.0, 1.0);
`;

let template = null;

// one pipeline compiled once; each shape paints with a copy of it
function pipelineFor(context) {
    if (!template) {
        template = Cogl.Pipeline.new(context);
        // a white texel: the rectangle's texture coordinates are what the shader measures with
        const white = Cogl.Texture2D.new_from_data(context, 1, 1, Cogl.PixelFormat.RGBA_8888_PRE, 4,
            new Uint8Array([255, 255, 255, 255]));
        template.set_layer_texture(0, white);
        template.add_snippet(Cogl.Snippet.new(Cogl.SnippetHook.FRAGMENT, DECLARATIONS, CODE));
    }
    return template.copy();
}

export const RoundRect = GObject.registerClass(
class RoundRect extends Clutter.Actor {
    constructor(params = {}) {
        super(params);
        this._rgba = [0, 0, 0, 0];      // 0..1, straight alpha
        this._radii = [0, 0, 0, 0];     // actor units
        this._pipeline = null;
    }

    // @rgba [r, g, b] 0..255 and alpha 0..1
    setColor(r, g, b, a) {
        const rgba = [r / 255, g / 255, b / 255, a];
        if (rgba.every((v, i) => v === this._rgba[i]))
            return;
        this._rgba = rgba;
        this.queue_redraw();
    }

    setRadii(topLeft, topRight, bottomRight, bottomLeft) {
        const radii = [topLeft, topRight, bottomRight, bottomLeft];
        if (radii.every((v, i) => v === this._radii[i]))
            return;
        this._radii = radii;
        this.queue_redraw();
    }

    vfunc_paint_node(root, paintContext) {
        const [w, h] = this.allocation.get_size();
        const alpha = this._rgba[3] * this.get_paint_opacity() / 255;
        if (w <= 0 || h <= 0 || alpha <= 0)
            return;
        if (!this._pipeline) {
            this._pipeline = pipelineFor(paintContext.get_framebuffer().get_context());
            const p = this._pipeline;
            this._uniforms = ['size', 'radii', 'color', 'aa'].map(name => p.get_uniform_location(name));
        }
        const p = this._pipeline, [size, radii, color, aa] = this._uniforms;
        const [r, g, b] = this._rgba;
        // a corner never larger than half a side, as CSS shrinks them
        const fit = Math.min(1, w / Math.max(this._radii[0] + this._radii[1], this._radii[2] + this._radii[3], 1e-3),
            h / Math.max(this._radii[0] + this._radii[3], this._radii[1] + this._radii[2], 1e-3));
        p.set_uniform_float(size, 2, 1, [w, h]);
        p.set_uniform_float(radii, 4, 1, this._radii.map(v => v * fit));
        p.set_uniform_float(color, 4, 1, [r * alpha, g * alpha, b * alpha, alpha]);
        p.set_uniform_1f(aa, 1 / SCALE);
        const node = Clutter.PipelineNode.new(p);
        node.add_rectangle(new Clutter.ActorBox({x1: 0, y1: 0, x2: w, y2: h}));
        root.add_child(node);
    }
});
