// VibeOS's on-screen keyboard: the AOSP keyboard's layouts, sizes and colours on GNOME Shell's keyboard.
//
// VibeosKeyboard is a subclass of the shell's Keyboard. It keeps everything that is not looks: which field has the
// focus, the input source and the field's purpose, committing text (to the field's text input, or as key events
// where there is none), moving the focused window clear of the keys, the swipe down that closes it, the shell's
// open and close. It replaces the looks: the shell's key actors, suggestions bar, emoji panel and bottom line go,
// and the keys are drawn as the AOSP keyboard draws them (renderer.js), at its sizes (geometry.js), in its colours
// (palette.js).
//
// KeyboardSwap puts it in place of the shell's, and back, as vibeos.Shell keyboard says ('vibeos' or 'gnome').
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import St from 'gi://St';

import * as Signals from 'resource:///org/gnome/shell/misc/signals.js';
import * as KeyboardUI from 'resource:///org/gnome/shell/ui/keyboard.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as InputSources from 'resource:///org/gnome/shell/ui/status/keyboard.js';

import {formFactor, keyAt, measure, placeKeys} from './geometry.js';
import {KeyboardPalette} from './palette.js';
import {KeysCanvas, NavCanvas, PressedKey, StripCanvas} from './renderer.js';

// shown, the keyboard slides up decelerating; hidden, it slides down accelerating
const SHOW_MS = 200;
const HIDE_MS = 190;
// a second tap on shift this soon locks the capitals
const DOUBLE_TAP_MS = 300;

// an input source's XKB layout -> the keyboard's language
const LOCALES = {us: 'en_US', gb: 'en_GB', de: 'de', fr: 'fr', it: 'it', es: 'es', ru: 'ru', ua: 'uk', il: 'iw'};
const RTL = new Set(['iw']);

const DIR = GLib.path_get_dirname(GLib.filename_from_uri(import.meta.url)[0]);
const data = new Map();

function layoutData(name) {
    if (!data.has(name)) {
        const file = Gio.File.new_for_path(GLib.build_filenamev([DIR, 'layouts', `${name}.json`]));
        const [, bytes] = file.load_contents(null);
        data.set(name, JSON.parse(new TextDecoder().decode(bytes)));
    }
    return data.get(name);
}

function localeOf(xkbId) {
    return LOCALES[(xkbId ?? '').split('+')[0]] ?? 'en_US';
}

function inputSources() {
    const manager = InputSources.getInputSourceManager();
    return Object.values(manager.inputSources ?? {}).sort((a, b) => a.index - b.index);
}

export const VibeosKeyboard = GObject.registerClass(
class VibeosKeyboard extends KeyboardUI.Keyboard {
    // The shell's keyboard is set up, then its looks are taken away and ours put in
    _setupKeyboard() {
        this._kb = {locale: 'en_US', purpose: Clutter.InputContentPurpose.NORMAL, page: 'alphabet', view: null,
            touches: new Map(), lastShift: 0, manualShift: false, capitals: null};
        this._layers = {default: 'alphabet', shift: 'alphabet-shifted'};
        super._setupKeyboard();

        this._suggestions.destroy();
        this._suggestions = null;
        this._emojiSelection.disconnectObject(this);
        this._aspectContainer.destroy();
        this._aspectContainer = null;
        this._emojiSelection = null;
        this._bottomPanelBox.destroy();
        this._bottomPanelBox = null;
        // the shell theme's keyboard frame (its padding, background and top line) is not ours: every pixel is drawn
        this.style = 'padding: 0; margin: 0; border: 0; background-color: transparent; box-shadow: none;';

        this._kbStrip = new StripCanvas({reactive: true});
        this._kbKeys = new St.Widget({reactive: true});
        this._kbKeysCanvas = new KeysCanvas();
        this._kbPressed = new PressedKey();
        this._kbPressed.hide();
        this._kbKeys.add_child(this._kbKeysCanvas);
        this._kbKeys.add_child(this._kbPressed);
        this._kbNav = new NavCanvas({reactive: true});
        this.add_child(this._kbStrip);
        this.add_child(this._kbKeys);
        this.add_child(this._kbNav);

        this._kbKeys.connect('touch-event', (_actor, event) => this._onTouch(event));
        this._kbKeys.connect('button-press-event', (_actor, event) => this._onButton(event, true));
        this._kbKeys.connect('button-release-event', (_actor, event) => this._onButton(event, false));
        this._kbNav.connect('touch-event', (_actor, event) => this._onNavEvent(event));
        this._kbNav.connect('button-release-event', (_actor, event) => this._onNavEvent(event));
        // the strip has nothing to tap yet, but a tap there stays on the keyboard
        this._kbStrip.connect('button-press-event', () => Clutter.EVENT_STOP);

        this._kbPalette = new KeyboardPalette(() => this._kbRebuild());
        // turned, a camera hole's band takes a side of the work area, and the keys stay clear of it
        global.display.connectObject('workareas-changed', () => this._kbRebuild(), this);
        // the field reports its text after every key and every move of the cursor: the capitals follow
        Main.inputMethod.connectObject('surrounding-text-set', () => this._updateLevelFromHints(false), this);
        this._kbRebuild();
    }

    _onDestroy() {
        this._kbPalette?.destroy();
        this._kbPalette = null;
        super._onDestroy();
    }

    // ---- what the shell asks of its keyboard, answered with ours ----

    _updateLayout(groupName, purpose) {
        this._kb.locale = localeOf(groupName);
        this._kb.purpose = purpose ?? Clutter.InputContentPurpose.NORMAL;
        // as the shell keeps it, so that a change of the field's hints alone does not start the keyboard over
        this._emojiVisible = this._shouldShowEmoji();
        this._modifiers.clear();
        this._kbRebuild();
    }

    // 'default' and 'shift' are the letters, lower and upper case (the shell's automatic capitals ask for them);
    // in a number or phone field 'default' is the symbols page, which has the digits
    _setActiveLevel(level) {
        const home = this._kbHomePage();
        this._kbSetPage(level === 'shift' && home === 'alphabet' ? 'alphabet-shifted' : home);
    }

    get _currentPage() {
        return this._kb?.page ?? null;
    }

    set _currentPage(_page) {
    }

    _relayout() {
        this._kbRebuild();
    }

    _setEmojiActive(active) {
        this._emojiActive = active;
    }

    _toggleEmoji() {
    }

    // The field's capitals, on the letters only: all capitals or none where the field says so; where it asks for
    // them, at the start of its text and of every sentence (of every word, for a title), as its text before the
    // cursor shows. A shift the user chose stays until the next key.
    _updateLevelFromHints(userInputHappened) {
        const page = this._kb?.page;
        if (!page || page.startsWith('symbols') || page === 'alphabet-locked' || this._kb.manualShift)
            return;
        const H = Clutter.InputContentHintFlags;
        const hints = this._contentHints ?? 0;
        if (hints & H.LOWERCASE)
            this._setActiveLevel('default');
        else if (hints & H.UPPERCASE)
            this._setActiveLevel('shift');
        else if (hints & (H.AUTO_CAPITALIZATION | H.TITLECASE))
            this._kbAutoCapitals(hints & H.TITLECASE);
        else if (userInputHappened && page === 'alphabet-shifted')
            this._setActiveLevel('default');
    }

    _kbAutoCapitals(title) {
        const [text, cursor] = Main.inputMethod.getSurroundingText();
        // the text is forgotten whenever the input method is reset, until the field reports it again: until then
        // the capitals stay as they last were
        if (text === null || text === undefined) {
            if (this._kb.capitals !== null)
                this._setActiveLevel(this._kb.capitals ? 'shift' : 'default');
            return;
        }
        const before = GLib.utf8_substring(text, 0, cursor);
        const trimmed = before.trimEnd();
        let capitals;
        if (trimmed.length === 0)
            capitals = true;
        else if (trimmed === before)
            capitals = false;
        else
            capitals = title || /[.!?]$/.test(trimmed);
        this._kb.capitals = capitals;
        this._setActiveLevel(capitals ? 'shift' : 'default');
    }

    resetSuggestions() {
    }

    setSuggestionsVisible(visible) {
        this._suggestionsVisible = visible;
    }

    addSuggestion(_text, _callback) {
    }

    vfunc_get_preferred_height(_forWidth) {
        const view = this._kb?.view;
        const h = view ? view.m.total / view.scale : 0;
        return [h, h];
    }

    // opened on the letters, in capitals where the field asks for them (at its start, after a full stop)
    _open() {
        super._open();
        if (this._keyboardRequested)
            this._updateLevelFromHints(false);
    }

    // the strip, the keys and the navigation bar one under the other at their exact heights: a box layout rounds
    // them to whole logical pixels, which on the tablet's half pixels moved the navigation bar by a screen pixel
    vfunc_allocate(box) {
        this.set_allocation(box);
        const view = this._kb?.view;
        const width = box.get_width();
        let y = 0;
        for (const [actor, h] of [[this._kbStrip, view?.m.strip], [this._kbKeys, view?.m.height],
            [this._kbNav, view?.m.navBar]]) {
            if (!actor)
                continue;
            const height = view ? h / view.scale : 0;
            actor.allocate(new Clutter.ActorBox({x1: 0, y1: y, x2: width, y2: y + height}));
            y += height;
        }
    }

    _animateShow() {
        global.compositor.disable_unredirect();
        Main.layoutManager.keyboardBox.show();
        this._keyboardVisible = true;
        if (this._focusWindow && !Main.overview.visible)
            this._animateWindow(this._focusWindow, true);
        if (this._focusPopovers.size > 0 && !Main.overview.visible)
            this._animatePopovers(true);
        this.opacity = 255;
        this.ease({
            translation_y: 0,
            duration: SHOW_MS,
            mode: Clutter.AnimationMode.EASE_OUT_CUBIC,
            onComplete: () => this._animateShowComplete(),
        });
        this.emit('visibility-changed');
    }

    _animateHide() {
        if (this._focusWindow)
            this._animateWindow(this._focusWindow, false);
        if (this._focusPopovers.size > 0)
            this._animatePopovers(false);
        this._kbReleaseAll();
        this.ease({
            translation_y: this.height,
            duration: HIDE_MS,
            mode: Clutter.AnimationMode.EASE_IN_QUAD,
            onComplete: () => {
                this.opacity = 0;
                this._animateHideComplete();
            },
        });
        this._keyboardVisible = false;
        this.emit('visibility-changed');
    }

    // ---- the keyboard's own ----

    _kbHomePage() {
        const P = Clutter.InputContentPurpose;
        return [P.DIGITS, P.NUMBER, P.PHONE].includes(this._kb.purpose) ? 'symbols' : 'alphabet';
    }

    _kbPageName() {
        const P = Clutter.InputContentPurpose;
        let name = this._kb.page;
        if (this._kb.purpose === P.EMAIL)
            name += '/email';
        else if (this._kb.purpose === P.URL)
            name += '/url';
        if (inputSources().length > 1)
            name += '+lang';
        return name;
    }

    // the space bar's language: the short name, the long one when another source has the same language
    _kbLanguage(layout) {
        const sources = inputSources();
        if (sources.length < 2)
            return null;
        const lang = this._kb.locale.split('_')[0];
        const same = sources.filter(s => localeOf(s.xkbId).split('_')[0] === lang).length;
        return same > 1 ? layout.fullName : layout.name;
    }

    _kbRebuild() {
        if (!this._kb || !this._kbKeysCanvas)
            return;
        const monitor = Main.layoutManager.keyboardMonitor;
        if (!monitor)
            return;
        const scale = monitor.geometry_scale ?? global.display.get_monitor_scale(monitor.index);
        const work = Main.layoutManager.getWorkAreaForMonitor(monitor.index);
        const form = formFactor(monitor.width, monitor.height);
        const geometry = layoutData('geometry')[form];
        const m = measure(geometry, form, {
            monitorWidth: Math.round(monitor.width * scale),
            monitorHeight: Math.round(monitor.height * scale),
            density: scale,
            insetLeft: Math.max(0, Math.round((work.x - monitor.x) * scale)),
            insetRight: Math.max(0, Math.round((monitor.x + monitor.width - work.x - work.width) * scale)),
        });
        const layout = layoutData(this._kb.locale);
        const pages = layout.forms[form];
        const ids = pages[this._kbPageName()] ?? pages[this._kb.page] ?? pages.alphabet;
        const rows = placeKeys(m, ids.map(i => layout.rows[i]));
        const view = {
            m, rows, scale, colors: this._kbPalette.colors,
            enterIcon: this._kb.purpose === Clutter.InputContentPurpose.URL ? 'go_key' : null,
            language: this._kbLanguage(layout),
            rtl: RTL.has(this._kb.locale),
            switcher: inputSources().length > 1,
        };
        this._kb.view = view;
        this._kbStrip.set_height(m.strip / scale);
        this._kbKeys.set_height(m.height / scale);
        // a canvas's surface is its size rounded to whole logical pixels, then scaled: the keys' canvas is made a
        // whole number of them high (its last row reaching under the navigation bar or off the screen), so that
        // its surface is drawn one to one
        this._kbKeysCanvas.set_size(monitor.width, Math.ceil(m.height / scale));
        this._kbNav.set_height(m.navBar / scale);
        this._kbNav.visible = m.navBar > 0;
        for (const canvas of [this._kbStrip, this._kbKeysCanvas, this._kbNav])
            canvas.setView(view);
        this._kbReleaseAll();
        this.queue_relayout();
    }

    // (any page change but the shift key's own lets the field's capitals apply again)
    _kbSetPage(page) {
        if (!this._kb)
            return;
        this._kb.manualShift = false;
        if (this._kb.page === page)
            return;
        this._kb.page = page;
        this._latched = page === 'alphabet-locked';
        this._kbRebuild();
    }

    // a touch point in the keys' screen pixels (from the keyboard's left edge, insets excluded)
    _kbPoint(event) {
        const [x, y] = event.get_coords();
        const [ok, ax, ay] = this._kbKeys.transform_stage_point(x, y);
        if (!ok || !this._kb.view)
            return null;
        const {m, scale} = this._kb.view;
        return [ax * scale - m.insetLeft, ay * scale];
    }

    _kbPress(id, event) {
        const p = this._kbPoint(event);
        const k = p ? keyAt(this._kb.view.rows, ...p) : null;
        this._kb.touches.set(id, k);
        if (k)
            this._kbPressed.place(this._kb.view, k);
    }

    _kbRelease(id, cancelled) {
        if (!this._kb.touches.has(id))
            return;
        const k = this._kb.touches.get(id);
        this._kb.touches.delete(id);
        if (this._kb.touches.size === 0)
            this._kbPressed.hide();
        if (k && !cancelled)
            this._kbActivate(k.key);
    }

    _kbReleaseAll() {
        this._kb?.touches.clear();
        this._kbPressed?.hide();
    }

    _onTouch(event) {
        const type = event.type();
        const id = event.get_event_sequence()?.get_slot?.() ?? event.get_event_sequence();
        if (type === Clutter.EventType.TOUCH_BEGIN)
            this._kbPress(id, event);
        else if (type === Clutter.EventType.TOUCH_END)
            this._kbRelease(id, false);
        else if (type === Clutter.EventType.TOUCH_CANCEL)
            this._kbRelease(id, true);
        return Clutter.EVENT_STOP;
    }

    _onButton(event, pressed) {
        if (event.get_button() !== Clutter.BUTTON_PRIMARY)
            return Clutter.EVENT_PROPAGATE;
        if (pressed)
            this._kbPress('pointer', event);
        else
            this._kbRelease('pointer', false);
        return Clutter.EVENT_STOP;
    }

    _onNavEvent(event) {
        const type = event.type();
        if (type !== Clutter.EventType.TOUCH_END && type !== Clutter.EventType.BUTTON_RELEASE)
            return Clutter.EVENT_STOP;
        const [x] = event.get_coords();
        const [ok, ax] = this._kbNav.transform_stage_point(x, 0);
        if (!ok || !this._kb.view)
            return Clutter.EVENT_STOP;
        switch (this._kbNav.hitTest(this._kb.view, ax * this._kb.view.scale)) {
        case 'hide':
            this.close(true);
            break;
        case 'switch':
            this._kbNextSource();
            break;
        }
        return Clutter.EVENT_STOP;
    }

    _kbNextSource() {
        const sources = inputSources();
        if (sources.length < 2)
            return;
        const current = InputSources.getInputSourceManager().currentSource;
        const i = sources.indexOf(current);
        sources[(i + 1) % sources.length].activate(true);
    }

    // text typed: a capital takes the letters back to lower case, and once the text is in, the field's capitals
    // apply again (a new sentence starts in capitals)
    _kbCommit(str) {
        this._kb.manualShift = false;
        if (this._kb.page === 'alphabet-shifted')
            this._kbSetPage('alphabet');
        this._keyboardController.commit(str, this._modifiers)
            .then(() => this._updateLevelFromHints(true))
            .catch(logError);
    }

    _kbKeyval(keyval) {
        this._kb.manualShift = false;
        this._keyboardController.keyvalPress(keyval);
        this._keyboardController.keyvalRelease(keyval);
        this._updateLevelFromHints(true);
    }

    _kbShift() {
        const page = this._kb.page;
        if (page.startsWith('symbols')) {
            this._kbSetPage(page === 'symbols' ? 'symbols-shifted' : 'symbols');
            return;
        }
        const now = GLib.get_monotonic_time() / 1000;
        const doubleTap = now - this._kb.lastShift < DOUBLE_TAP_MS;
        this._kb.lastShift = now;
        if (page === 'alphabet')
            this._kbSetPage('alphabet-shifted');
        else if (page === 'alphabet-shifted' && doubleTap)
            this._kbSetPage('alphabet-locked');
        else
            this._kbSetPage('alphabet');
        this._kb.manualShift = true;
    }

    _kbActivate(key) {
        switch (key.c) {
        case 'shift':
            this._kbShift();
            break;
        case 'capslock':
            this._kbSetPage('alphabet-locked');
            break;
        case 'symbols':
            this._kbSetPage(this._kb.page.startsWith('symbols') ? 'alphabet' : 'symbols');
            if (this._kb.page === 'alphabet')
                this._updateLevelFromHints(false);
            break;
        case 'delete': {
            // as the shell's own backspace: key events, or the text input's delete for an Android window
            const alien = this._focusWindow?.is_alien?.();
            this._kb.manualShift = false;
            this._keyboardController.toggleDelete(true, alien);
            this._keyboardController.toggleDelete(false, alien);
            this._updateLevelFromHints(true);
            break;
        }
        case 'enter':
        case 'shift_enter':
            this._kbKeyval(Clutter.KEY_Return);
            break;
        case 'tab':
            this._kbKeyval(Clutter.KEY_Tab);
            break;
        case 'space':
            this._kbCommit(' ');
            break;
        case 'language':
            this._kbNextSource();
            break;
        case 'emoji':
        case 'settings':
        case 'clipboard_paste':
        case 'shortcut':
        case 'next':
        case 'previous':
        case 'none':
            break;
        default:
            if (key.t ?? key.l)
                this._kbCommit(key.t ?? key.l);
        }
    }
});

function vibeosSettings() {
    const schema = Gio.SettingsSchemaSource.get_default()?.lookup('vibeos.Shell', true);
    return schema?.has_key('keyboard') ? new Gio.Settings({settings_schema: schema}) : null;
}

// A keyboard taken out of use: the window it moved put back, the shell's composited state and its swipe gesture
// (the shell's own teardown leaves that on the stage) undone, then destroyed
function retire(keyboard) {
    keyboard.remove_all_transitions();
    // the shell still calls close() on it when the overview shows: let that find it closed
    keyboard._keyboardRequested = false;
    const window = keyboard._focusWindow;
    if (window?.__keyboardMoved)
        keyboard._animateWindow(window, false);
    if (keyboard._keyboardVisible)
        global.compositor.enable_unredirect();
    keyboard._keyboardVisible = false;
    if (keyboard._surroundingTextId) {
        Main.inputMethod.disconnect(keyboard._surroundingTextId);
        keyboard._surroundingTextId = 0;
    }
    if (keyboard._panGesture) {
        keyboard._panGesture.enabled = false;
        Main.uiGroup.remove_action(keyboard._panGesture);
    }
    keyboard.destroy();
}

// KeyboardManager._syncEnabled as the shell has it, making @Class
function syncEnabledWith(Class) {
    return function () {
        const enableKeyboard = this._a11yApplicationsSettings.get_boolean('screen-keyboard-enabled');
        const autoEnabled = this._seat.get_touch_mode() && this._lastDeviceIsTouchscreen();
        const enabled = enableKeyboard || autoEnabled;
        if (enabled && !this._keyboard) {
            this._keyboard = new Class();
            this._keyboard.setSuggestionsVisible(this._suggestionsVisible);
            this._keyboard.connect('visibility-changed', () => this.emit('visibility-changed'));
        } else if (!enabled && this._keyboard) {
            const keyboard = this._keyboard;
            this._keyboard = null;
            retire(keyboard);
        }
    };
}

// The shell's keyboard manager makes its keyboard in _syncEnabled from the module's own Keyboard, which cannot be
// replaced from outside: the method is replaced on the prototype instead. Two of its signal handlers were bound to
// the shell's method when the manager was made, so a keyboard those still make is replaced by ours as it is stored.
export class KeyboardSwap extends Signals.EventEmitter {
    constructor() {
        super();
        this._settings = vibeosSettings();
        this._settings?.connectObject('changed::keyboard', () => this._sync(), this);
        this._active = false;
        this._sync();
    }

    get active() {
        return this._active;
    }

    _sync() {
        const want = this._settings?.get_string('keyboard') === 'vibeos';
        if (want === this._active)
            return;
        try {
            if (want)
                this._swapIn();
            else
                this._swapOut();
            this._active = want;
        } catch (e) {
            logError(e, 'vibeos: the keyboard swap');
            // what went in comes out again: GNOME's keyboard as it was
            try {
                this._swapOut();
            } catch (e2) {
                logError(e2, 'vibeos: the keyboard swap undone');
            }
            this._active = false;
        }
        this.emit('changed');
    }

    // the manager's keyboard replaced by a new one, open if the old one was
    _replace(manager) {
        const old = manager._keyboard;
        const wasOpen = old?._keyboardVisible ?? false;
        if (old) {
            manager._keyboard = null;
            retire(old);
        }
        manager._syncEnabled();
        const keyboard = manager._keyboard;
        if (keyboard) {
            keyboard._setFocusWindow(keyboard._focusTracker?.currentWindow ?? null);
            if (wasOpen)
                keyboard.open();
        }
    }

    _swapIn() {
        const manager = Main.keyboard;
        const proto = KeyboardUI.KeyboardManager.prototype;
        this._shellSyncEnabled = proto._syncEnabled;
        proto._syncEnabled = syncEnabledWith(VibeosKeyboard);

        let current = manager._keyboard;
        Object.defineProperty(manager, '_keyboard', {
            configurable: true,
            enumerable: true,
            get: () => current,
            set: keyboard => {
                if (keyboard && !(keyboard instanceof VibeosKeyboard)) {
                    retire(keyboard);
                    keyboard = new VibeosKeyboard();
                }
                current = keyboard;
            },
        });
        this._replace(manager);
    }

    _swapOut() {
        const manager = Main.keyboard;
        if (Object.getOwnPropertyDescriptor(manager, '_keyboard')?.get) {
            const current = manager._keyboard;
            delete manager._keyboard;
            manager._keyboard = current;
        }
        if (this._shellSyncEnabled)
            KeyboardUI.KeyboardManager.prototype._syncEnabled = this._shellSyncEnabled;
        this._shellSyncEnabled = null;
        this._replace(manager);
    }

    destroy() {
        this._settings?.disconnectObject(this);
        if (this._active)
            this._swapOut();
        this._active = false;
        this._settings = null;
    }
}
