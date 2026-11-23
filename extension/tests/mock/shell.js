// What power-keys.js's modules take from GNOME Shell, as little as they use of it: none of it draws or grabs
// anything. The test sets `apps` (desktop id -> app) and reads `launched`.
export const state = {apps: new Map(), launched: []};

export const Clutter = {
    Actor: class {
        constructor(props) {
            Object.assign(this, props);
        }

        show() {}

        hide() {}

        destroy() {}
    },
    get_current_event: () => null,
    EventFlags: {FLAG_REPEATED: 1},
};

export const Meta = {};

export const Shell = {
    AppSystem: {get_default: () => ({lookup_app: id => state.apps.get(id) ?? null})},
    Screenshot: class {},
};

export const St = {
    Widget: class {},
    Settings: {get: () => ({enable_animations: false})},
};

export const Main = {
    layoutManager: {
        primaryMonitor: {x: 0, y: 0, width: 1080, height: 2280},
        uiGroup: {add_child() {}, set_child_above_sibling() {}},
    },
};
