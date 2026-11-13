// Mutter mipmaps a window it draws below half its size, and rebuilds the chain (a render pass for each of a dozen
// levels) whenever the window's content changes while it is that small. That is when apps draw: their first frames
// as they grow out of their icon, and GTK's unfocused-window transition as one flies home. On the S10e it added 12
// GPU passes to each of those frames, and a vblank was missed in most launches (2026-11-04). The reference phone
// samples a scaled window linearly, never from mipmaps, so windows here are not mipmapped either.
import Meta from 'gi://Meta';

export class NoWindowMipmaps {
    constructor() {
        global.get_window_actors().forEach(noMipmaps);
        global.window_group.connectObject('child-added', (_group, actor) => {
            if (!(actor instanceof Meta.WindowActor))
                return;
            noMipmaps(actor);
            // a surface that comes later (Xwayland's) has a texture of its own
            actor.connectObject('first-frame', () => noMipmaps(actor), this);
        }, this);
    }

    destroy() {
        global.window_group.disconnectObject(this);
        for (const actor of global.get_window_actors()) {
            actor.disconnectObject(this);
            actor.get_texture()?.set_create_mipmaps(true);
        }
    }
}

function noMipmaps(actor) {
    actor.get_texture()?.set_create_mipmaps(false);
}
