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
import GioUnix from 'gi://GioUnix';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';
import St from 'gi://St';

import * as Signals from 'resource:///org/gnome/shell/misc/signals.js';
import * as KeyboardUI from 'resource:///org/gnome/shell/ui/keyboard.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as InputSources from 'resource:///org/gnome/shell/ui/status/keyboard.js';

import {click} from '../haptics.js';
import {sounds} from '../sounds.js';
import {gestureTookTouches} from '../oskfocus.js';
import {openSettings} from '../settings.js';
import {inHomeStrip} from '../task.js';
import {SCALE, dp} from '../units.js';
import {formFactor, keyAt, measure, moreKeyAt, placeKeys, placeMoreKeys, stripSlots} from './geometry.js';
import {KeyboardPalette} from './palette.js';
import {KeysCanvas, labelWidth, MoreKeysPanel, NavCanvas, PressedKey, StripCanvas} from './renderer.js';
import {atSentenceStart, autocorrection, Learnt, Lexicon, neighboursOf, placeInStrip, suggest, wordsBefore} from './suggest.js';

// shown, the keyboard slides up decelerating; hidden, it slides down accelerating
const SHOW_MS = 200;
const HIDE_MS = 190;
// a window slid up over the keys stops with the text cursor's line this far above their top edge
const SLIDE_MARGIN_DP = 8;
// a second tap on shift this soon locks the capitals, and so does holding it this long
const DOUBLE_TAP_MS = 300;
const SHIFT_LOCK_MS = 1200;
// a held key that repeats (delete) does so after this long, then at this interval; every other repeat ticks
const REPEAT_START_MS = 400;
const REPEAT_MS = 50;
// a finger further than this from every key of a long press's panel chooses none of them (lifted, it types nothing)
const MORE_KEYS_REACH_DP = 48;
// a finger that moves this far after its panel opens has slid in the panel; one that did not leaves the panel open
const MORE_KEYS_SLIDE_DP = 8;
// an open panel's key a tap chooses: the one under the finger, or one this close to it (the gaps between them)
const MORE_KEYS_TAP_REACH_DP = 4;
// a short press's key preview: it grows into place as the finger lands, stays a moment after the finger lifts, then
// shrinks away
const PREVIEW_SHOW_MS = 17;
const PREVIEW_SHOW_SCALE = 0.98;
const PREVIEW_LINGER_MS = 70;
const PREVIEW_DISMISS_MS = 53;
const PREVIEW_DISMISS_SCALE = 0.94;

// an input source's XKB layout -> the keyboard's language
const LOCALES = {us: 'en_US', gb: 'en_GB', de: 'de', fr: 'fr', it: 'it', es: 'es', ru: 'ru', ua: 'uk', il: 'iw'};
const RTL = new Set(['iw']);

const DIR = GLib.path_get_dirname(GLib.filename_from_uri(import.meta.url)[0]);
const data = new Map();

// Turned on a tablet, the letters' second row starts half a key in (where a laptop's Caps Lock is): a Tab key goes
// in that space (owner, 11-21), as wide as it is, on the letter pages of every layout that leaves it.
const TAB_KEY_MIN_SPACE = 3;                // % of the keyboard's width
function withTabKey(form, page, rows) {
    const row = rows[1];
    if (form !== 'tablet-land' || !page.startsWith('alphabet') || !row?.length || row[0].x < TAB_KEY_MIN_SPACE)
        return rows;
    const tab = {a: ['noPreview'], b: 'functional', c: 'tab', i: 'tab_key', w: row[0].x, x: 0};
    return rows.map((r, i) => i === 1 ? [tab, ...r] : r);
}

// ---- the suggestion strip's words ----

// A language's word list is read this many lines at a time, between frames
const WORDS_SLICE = 6000;
// what this user types is kept here, a file per language, written this long after the last word learnt
const LEARNT_DIR = GLib.build_filenamev([GLib.get_user_data_dir(), 'vibeos', 'keyboard']);
const LEARNT_SAVE_MS = 10000;
// typed after a suggestion was picked (and its space added), these go before that space: "word, " not "word ,"
const SPACE_SWALLOWERS = /^[.,!?;:)\]}%]$/;
// typed after a word, these end it, and make its auto-correction (Enter too)
const SEPARATORS = /^[ .,!?;:)\]}"\u2026\n]$/u;

// the language the strip suggests in: one at a time, loaded when the keyboard first needs it
const words = {locale: null, lexicon: null, learnt: null, waiters: [], loadId: 0, saveId: 0, generation: 0};

function learntPath(locale) {
    return GLib.build_filenamev([LEARNT_DIR, `learnt-${locale}.json`]);
}

function saveLearnt() {
    if (words.saveId) {
        GLib.source_remove(words.saveId);
        words.saveId = 0;
    }
    if (!words.learnt || !words.learnt.dirty)
        return;
    words.learnt.dirty = false;
    try {
        GLib.mkdir_with_parents(LEARNT_DIR, 0o700);
        const bytes = new TextEncoder().encode(JSON.stringify(words.learnt));
        GLib.file_set_contents(learntPath(words.locale), bytes);
    } catch (e) {
        logError(e, 'keyboard: the learnt words were not saved');
    }
}

function learnSoon() {
    words.learnt.dirty = true;
    if (!words.saveId) {
        words.saveId = GLib.timeout_add(GLib.PRIORITY_LOW, LEARNT_SAVE_MS, () => {
            words.saveId = 0;
            saveLearnt();
            return GLib.SOURCE_REMOVE;
        });
    }
}

function readLearnt(locale) {
    try {
        const [ok, bytes] = GLib.file_get_contents(learntPath(locale));
        if (ok)
            return new Learnt(JSON.parse(new TextDecoder().decode(bytes)));
    } catch {
        // none yet
    }
    return new Learnt(null);
}

// The words of @locale's language: {lexicon (null while it loads, or where the language has no list), learnt}, the
// list read in slices off the main loop's idle time; @onReady() once it is in
function wordsFor(locale, onReady) {
    if (words.locale !== locale) {
        saveLearnt();
        if (words.loadId)
            GLib.source_remove(words.loadId);
        words.loadId = 0;
        words.generation++;
        Object.assign(words, {locale, lexicon: null, learnt: readLearnt(locale), waiters: []});
        loadLexicon(locale, words.generation);
    }
    if (!words.lexicon && onReady && !words.waiters.includes(onReady))
        words.waiters.push(onReady);
    return words;
}

function loadLexicon(locale, generation) {
    const file = Gio.File.new_for_path(GLib.build_filenamev([DIR, 'words', `${locale}.txt.gz`]));
    file.load_contents_async(null, (_f, result) => {
        if (generation !== words.generation)
            return;
        let lines;
        try {
            const [, bytes] = file.load_contents_finish(result);
            const gz = Gio.ConverterInputStream.new(Gio.MemoryInputStream.new_from_bytes(new GLib.Bytes(bytes)),
                Gio.ZlibDecompressor.new(Gio.ZlibCompressorFormat.GZIP));
            const out = Gio.MemoryOutputStream.new_resizable();
            out.splice(gz, Gio.OutputStreamSpliceFlags.CLOSE_SOURCE | Gio.OutputStreamSpliceFlags.CLOSE_TARGET, null);
            lines = new TextDecoder().decode(out.steal_as_bytes().toArray()).split('\n');
        } catch (e) {
            // a language with no list: the strip has only the words this user types
            if (!e.matches?.(Gio.IOErrorEnum, Gio.IOErrorEnum.NOT_FOUND))
                logError(e, `keyboard: no words for ${locale}`);
            return;
        }
        const lexicon = new Lexicon();
        let at = 0;
        words.loadId = GLib.idle_add(GLib.PRIORITY_LOW, () => {
            if (generation !== words.generation)
                return GLib.SOURCE_REMOVE;
            const to = Math.min(lines.length, at + WORDS_SLICE);
            lexicon.add(lines, at, to);
            at = to;
            if (at < lines.length)
                return GLib.SOURCE_CONTINUE;
            words.loadId = 0;
            words.lexicon = lexicon.done();
            for (const ready of words.waiters.splice(0))
                ready();
            return GLib.SOURCE_REMOVE;
        });
    });
}

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

// a touch's finger: its slot, the same for each of its events
function touchId(event) {
    return event.get_event_sequence()?.get_slot?.() ?? event.get_event_sequence();
}

// The keyboard goes over the app's window: the window keeps its size and nothing in it is laid out again. Only where
// the text cursor's line would be under the keys does the whole window slide up, just far enough to keep that line
// SLIDE_MARGIN_DP above them, and back down when they close (VibeosKeyboard._kbSlideStart); a field that is in view
// leaves the window still. A terminal fitted above the keys had its text reflowed for the new size, and again when it
// grew back (owner, 11-24: "keyboard squeezes terminal window instead of opening on top of it").
//
// An app whose desktop file says X-Vibeos-Keyboard=overlay has the keys over its window with no slide: it learns how
// much of it the keys cover from vibeos.Shell's KeyboardOverlap (service.js) and makes room itself, as the Navi map
// does. One that says X-Vibeos-Keyboard=fit, or whose id is in vibeos.Shell's keyboard-fit-apps, is fitted above the
// keys instead, the shell's own way: its window is made smaller and grows back when they close.
const OVERLAY_KEY = 'X-Vibeos-Keyboard';
const FIT_APPS_KEY = 'keyboard-fit-apps';

function windowApp(window) {
    return window ? Shell.WindowTracker.get_default().get_window_app(window) : null;
}

function keyboardKey(app) {
    const info = app?.get_app_info();
    return info instanceof GioUnix.DesktopAppInfo && info.has_key(OVERLAY_KEY) ? info.get_string(OVERLAY_KEY) : null;
}

export function keyboardOverlays(window) {
    return keyboardKey(windowApp(window)) === 'overlay';
}

export function keyboardFits(window) {
    const app = windowApp(window);
    if (!app)
        return false;
    if (keyboardKey(app) === 'fit')
        return true;
    shellSettings ??= vibeosSettings();
    const apps = shellSettings?.settings_schema.has_key(FIT_APPS_KEY)
        ? shellSettings.get_strv(FIT_APPS_KEY) : [];
    return apps.includes(app.get_id()?.replace(/\.desktop$/, ''));
}

// A window's menus (xdg popups, each its own window and actor) are placed by mutter against the window where it is,
// not where it is drawn slid: while it is slid, or slides back, each is drawn as far up as the window, or higher where
// the keyboard moved it clear of the keys (_animatePopovers, which measures them unslid).
const MENU_TYPES = [Meta.WindowType.DROPDOWN_MENU, Meta.WindowType.POPUP_MENU, Meta.WindowType.MENU,
    Meta.WindowType.COMBO, Meta.WindowType.TOOLTIP];

function liftMenus(window) {
    const parent = window.get_compositor_private();
    if (!parent)
        return;
    for (const actor of global.get_window_actors()) {
        const menu = actor.meta_window;
        if (!menu || menu === window || !MENU_TYPES.includes(menu.get_window_type()) ||
            menu.find_root_ancestor() !== window)
            continue;
        actor.translation_y = Math.min(menu.__vibeosLift ?? 0, parent.translation_y);
    }
}

// the window drawn y logical pixels up (y <= 0) after an ease of ms, its menus following it, until it is back down
function slideWindow(actor, y, ms, mode) {
    const moving = !!actor.get_transition('translation-y');
    if (moving ? actor.__vibeosSlideTo === y : actor.translation_y === y)
        return;
    actor.__vibeosSlideTo = y;
    actor.__vibeosMenusId ??= actor.connect('notify::translation-y', () => liftMenus(actor.meta_window));
    actor.ease({
        translation_y: y,
        duration: ms,
        mode,
        onStopped: finished => {
            if (!finished || y !== 0 || !actor.__vibeosMenusId)
                return;
            actor.disconnect(actor.__vibeosMenusId);
            delete actor.__vibeosMenusId;
        },
    });
}

export const VibeosKeyboard = GObject.registerClass(
class VibeosKeyboard extends KeyboardUI.Keyboard {
    // The shell's keyboard is set up, then its looks are taken away and ours put in
    _setupKeyboard() {
        this._kb = {locale: 'en_US', purpose: Clutter.InputContentPurpose.NORMAL, page: 'alphabet', view: null,
            touches: new Map(), passOn: new Set(), more: null, preview: null, lastShift: 0, manualShift: false, capitals: null,
            hiding: false, unredirect: false};
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
        // a long press's panel stands above the keyboard's top edge: it is in the stage's top layer, and takes no
        // input (the finger that opened it is still the keys' touch, which chooses in it)
        this._kbMore = new MoreKeysPanel({reactive: false});
        this._kbMore.hide();
        Main.layoutManager.uiGroup.add_child(this._kbMore);
        // left open after its finger lifted, the panel takes taps of its own (reactive only then)
        this._kbMore.connect('touch-event', (_actor, event) => this._onMoreEvent(event));
        this._kbMore.connect('button-press-event', (_actor, event) => this._onMoreEvent(event));
        this._kbMore.connect('button-release-event', (_actor, event) => this._onMoreEvent(event));
        this._kbMore.connect('motion-event', (_actor, event) => this._onMoreEvent(event));
        // a short press's key preview, the same card as a long press's panel of one key and where that one opens
        this._kbPreview = new MoreKeysPanel({reactive: false});
        this._kbPreview.set_pivot_point(0.5, 1);
        this._kbPreview.hide();
        Main.layoutManager.uiGroup.add_child(this._kbPreview);
        this.add_child(this._kbStrip);
        this.add_child(this._kbKeys);
        this.add_child(this._kbNav);

        // a keyboard left on the screen closed (_kbStale) goes at the first touch on it, and the keys see nothing of
        // that touch (or click), to its end: it types nothing
        const swallowed = new Set();
        this.connect('captured-event', (_actor, event) => {
            const T = Clutter.EventType;
            const type = event.type();
            const pointer = type === T.BUTTON_PRESS || type === T.BUTTON_RELEASE || type === T.MOTION;
            const id = pointer ? 'pointer' : touchId(event);
            if (type === T.TOUCH_BEGIN || type === T.BUTTON_PRESS) {
                if (!this._kbSettle())
                    return Clutter.EVENT_PROPAGATE;
                swallowed.add(id);
                return Clutter.EVENT_STOP;
            }
            if (!swallowed.has(id))
                return Clutter.EVENT_PROPAGATE;
            if (type === T.TOUCH_END || type === T.TOUCH_CANCEL || type === T.BUTTON_RELEASE)
                swallowed.delete(id);
            return Clutter.EVENT_STOP;
        });
        this._kbKeys.connect('touch-event', (_actor, event) => this._onTouch(event));
        this._kbKeys.connect('button-press-event', (_actor, event) => this._onButton(event, true));
        this._kbKeys.connect('button-release-event', (_actor, event) => this._onButton(event, false));
        this._kbKeys.connect('motion-event', (_actor, event) => this._onMotion(event));
        this._kbNav.connect('touch-event', (_actor, event) => this._onNavEvent(event));
        this._kbNav.connect('button-release-event', (_actor, event) => this._onNavEvent(event));
        // a word in the strip is tapped: it takes the place of the word being typed (any tap there stays on the
        // keyboard)
        this._kbStrip.connect('button-press-event', () => Clutter.EVENT_STOP);
        this._kbStrip.connect('button-release-event', (_actor, event) => {
            this._kbStripTap(event);
            return Clutter.EVENT_STOP;
        });
        this._kbStrip.connect('touch-event', (_actor, event) => {
            if (event.type() === Clutter.EventType.TOUCH_END)
                this._kbStripTap(event);
            return Clutter.EVENT_STOP;
        });

        this._kbPalette = new KeyboardPalette(() => this._kbRebuild());
        // turned, a camera hole's band takes a side of the work area, and the keys stay clear of it
        global.display.connectObject('workareas-changed', () => this._kbRebuild(), this);
        // the field reports its text after every key and every move of the cursor: the capitals follow
        Main.inputMethod.connectObject('surrounding-text-set', () => {
            this._updateLevelFromHints(false);
            this._kbSuggestSoon();
        }, 'notify::content-purpose', () => this._kbSuggestSoon(),
        'notify::content-hints', () => this._kbSuggestSoon(),
        // the text cursor, for a window slid over the keys: mutter's rect of the client's, in the stage, where the
        // window is when not slid (the window actor's translation left out). The client sends it when the field takes
        // the focus and whenever it moves. (A rect of NaNs came as Console's menu took the focus: no cursor.)
        'cursor-location-changed', (_im, rect) => {
            const y = rect.get_y(), height = rect.get_height();
            if (!Number.isFinite(y) || !Number.isFinite(height))
                return;
            this._kbCursor = {window: global.display.focus_window, focus: Main.inputMethod.currentFocus, y, height};
            this._kbSlideSoon();
        }, this);
        // a menu opened on a window slid over the keys: drawn slid with it from its first frame
        global.window_manager.connectObject('map', (_wm, actor) => {
            const window = actor.meta_window?.find_root_ancestor();
            if (window && window !== actor.meta_window && window.get_compositor_private()?.__vibeosMenusId)
                liftMenus(window);
        }, this);
        this._kbRebuild();
    }

    _onDestroy() {
        this._kbGone = true;
        this._kbSlideEnd(true);
        if (this._kbSuggestId)
            GLib.source_remove(this._kbSuggestId);
        this._kbSuggestId = 0;
        saveLearnt();
        this._kbReleaseAll();
        this._kbMore?.destroy();
        this._kbMore = null;
        this._kbPreview?.destroy();
        this._kbPreview = null;
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
        this._kbSuggestSoon();
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

    // The keys go over the window (_kbSlideStart): it is slid, never resized. A window of an app taken out of that
    // (keyboardFits) is fitted above the keys at once, and grows back at once: the shell's size-change animation of the
    // unmaximize and the maximize after it stretched a terminal's text down the screen and back. Other windows keep
    // the animation.
    _animateWindow(window, show) {
        // an app that has the keys over its window with no slide (keyboardOverlays): left as it is
        if (show && keyboardOverlays(window))
            return;
        if (show && window && !window.__keyboardMoved && !keyboardFits(window)) {
            this._kbSlideStart(window);
            return;
        }
        if (!show && window && this._kbSlide?.window === window)
            this._kbSlideEnd();
        const actor = window?.get_compositor_private();
        if (show && actor && window.maximized_vertically && !window.__keyboardMoved && !window.is_alien() &&
            Main.inputMethod.content_purpose === Clutter.InputContentPurpose.TERMINAL) {
            Main.wm.skipNextEffect(actor);
            window.__keyboardTerminal = true;
        } else if (!show && actor && window.__keyboardMoved && window.__keyboardTerminal) {
            Main.wm.skipNextEffect(actor);
        }
        if (!show)
            delete window?.__keyboardTerminal;
        super._animateWindow(window, show);
    }

    // The window in front slid up over the keys, as the keyboard comes up with them, until the text cursor's line is
    // SLIDE_MARGIN_DP above the keys' top edge, and no further than the window's bottom over it; with the line in
    // view, or no cursor known (the field's client has not sent its rectangle, or never does), it stays where it is,
    // and moves when a rectangle comes. It follows the cursor while the keyboard is up (a terminal's output takes it
    // down the screen, `clear` back up), and the keyboard's own changes of height. The window keeps its size: nothing
    // in it is laid out again.
    _kbSlideStart(window) {
        if (this._kbSlide?.window !== window) {
            this._kbSlideEnd();
            const actor = window.get_compositor_private();
            if (!actor)
                return;
            const soon = () => this._kbSlideSoon();
            this._kbSlide = {
                window, actor, later: 0,
                ids: [
                    window.connect('position-changed', soon),
                    window.connect('size-changed', soon),
                    // closing: left where it is, for its close animation
                    window.connect('unmanaging', () => this._kbSlideEnd(false)),
                ],
                heightId: this.connect('notify::height', soon),
            };
        }
        this._kbSlideMove();
    }

    _kbSlideSoon() {
        const slide = this._kbSlide;
        if (!slide || slide.later)
            return;
        slide.later = global.compositor.get_laters().add(Meta.LaterType.BEFORE_REDRAW, () => {
            slide.later = 0;
            if (this._kbSlide === slide)
                this._kbSlideMove();
            return GLib.SOURCE_REMOVE;
        });
    }

    _kbSlideMove() {
        const {window, actor} = this._kbSlide;
        const monitor = Main.layoutManager.keyboardMonitor;
        if (!monitor)
            return;
        const top = monitor.y + monitor.height - this.height;
        const frame = window.get_frame_rect();
        const covered = Math.max(0, frame.y + frame.height - top);
        // the rectangle of the field that has the focus now, in this window: not one an earlier field sent
        const cursor = this._kbCursor?.window === window && this._kbCursor.focus === Main.inputMethod.currentFocus &&
            this._kbCursor.height > 0 ? this._kbCursor : null;
        const shift = cursor
            ? Math.min(covered, Math.max(0, cursor.y + cursor.height + dp(SLIDE_MARGIN_DP) - top)) : 0;
        slideWindow(actor, -Math.round(shift), SHOW_MS, Clutter.AnimationMode.EASE_OUT_CUBIC);
    }

    // back down with the keys (animate), or left as it is
    _kbSlideEnd(animate = true) {
        const slide = this._kbSlide;
        if (!slide)
            return;
        this._kbSlide = null;
        if (slide.later)
            global.compositor.get_laters().remove(slide.later);
        slide.ids.forEach(id => slide.window.disconnect(id));
        this.disconnect(slide.heightId);
        if (animate)
            slideWindow(slide.actor, 0, HIDE_MS, Clutter.AnimationMode.EASE_IN_QUAD);
    }

    // the shell's lift of each popover clear of the keys, kept for the menus of a window slid over them (liftMenus)
    _animatePopovers(show) {
        super._animatePopovers(show);
        const slid = new Set();
        for (const popover of this._focusPopovers ?? []) {
            const actor = popover.get_compositor_private();
            if (!actor)
                continue;
            popover.__vibeosLift = show ? actor.translation_y : 0;
            const window = popover.find_root_ancestor();
            if (window !== popover && window.get_compositor_private()?.__vibeosMenusId)
                slid.add(window);
        }
        slid.forEach(liftMenus);
    }

    // opened on the letters, in capitals where the field asks for them (at its start, after a full stop)
    _open() {
        super._open();
        if (this._keyboardRequested)
            this._updateLevelFromHints(false);
        this._kbSuggestSoon();
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
        // the compositor's unredirection stopped once while the keyboard is up, allowed again once it is gone
        // (_animateHideComplete)
        if (!this._kb.unredirect)
            global.compositor.disable_unredirect();
        this._kb.unredirect = true;
        this._kb.hiding = false;
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
        this._keyboardVisible = false;
        this._kbSlideAway();
        this.emit('visibility-changed');
    }

    // The slide down, and the keyboard gone from the screen at its end. Also when another slide cuts it short: with
    // the end of a finished slide alone, the shell's swipe down, cancelled by the close, slid the closed keyboard back
    // up, and it stayed on the screen, closed, over the app and then the launcher (owner, 11-24).
    _kbSlideAway() {
        this._kb.hiding = true;
        this.ease({
            translation_y: this.height,
            duration: HIDE_MS,
            mode: Clutter.AnimationMode.EASE_IN_QUAD,
            onStopped: () => {
                if (this._kbGone)
                    return;
                this._kb.hiding = false;
                if (this._keyboardVisible)
                    return;
                this.opacity = 0;
                this._animateHideComplete();
            },
        });
    }

    _animateHideComplete() {
        Main.layoutManager.keyboardBox.hide();
        if (this._kb.unredirect)
            global.compositor.enable_unredirect();
        this._kb.unredirect = false;
    }

    // Closed, not on its way down, and still on the screen: what a hide cut short left. Back and a touch on it take it
    // away (keyboardShown, dismissKeyboard, the captured touch in _setupKeyboard).
    get _kbStale() {
        return !this._kbGone && !this._keyboardVisible && !this._kb.hiding && !this._gestureInProgress &&
            Main.layoutManager.keyboardBox.visible && this.mapped && this.opacity > 0 &&
            this.translation_y < this.height;
    }

    _kbSettle() {
        if (!this._kbStale)
            return false;
        this._kbReleaseAll();
        this._kbSlideAway();
        return true;
    }

    // The swipe down takes the finger: counted gone, as by the other gestures that take one (gestureTookTouches), so a
    // close is not held for a lift the stage may never see (oskfocus.js), and one held already comes now
    _panBegin(gesture) {
        gestureTookTouches();
        super._panBegin(gesture);
    }

    // The swipe down let go, or cancelled. A keyboard closed under the finger (a field losing the focus closes it, and
    // the close turns the swipe off, which cancels it) is not brought back: the shell's own end slid it back up and
    // fitted the window above it again, closed.
    _panEnd(gesture) {
        if (this._keyboardVisible)
            super._panEnd(gesture);
        else
            this._kbSettle();
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
        const rows = placeKeys(m, withTabKey(form, this._kb.page, ids.map(i => layout.rows[i])));
        const view = {
            m, rows, scale, colors: this._kbPalette.colors,
            enterIcon: this._kb.purpose === Clutter.InputContentPurpose.URL ? 'go_key' : null,
            language: this._kbLanguage(layout),
            rtl: RTL.has(this._kb.locale),
            switcher: inputSources().length > 1,
        };
        this._kb.view = view;
        this._kb.letterRows = (pages.alphabet ?? []).map(i => layout.rows[i]);
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
        this._kbCarryTouches();
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

    // A finger down: the key under it pressed, and timed if holding it does something. Each touch is
    // {k: the key, p: the finger's last point, timer, done: the hold has done the key's work (lifted, it types
    // nothing more)}. Another finger while a panel is open closes the panel.
    _kbPress(id, event) {
        if (this._kb.more) {
            const open = this._kb.more.open;
            this._kbCloseMore();
            // (a panel left open: the tap that closes it types nothing)
            if (open) {
                this._kb.touches.set(id, {k: null, p: null, timer: 0, done: true});
                return;
            }
        }
        const p = this._kbPoint(event);
        const k = p ? keyAt(this._kb.view.rows, ...p) : null;
        const touch = {k, p, timer: 0, done: false};
        this._kb.touches.set(id, touch);
        if (!k)
            return;
        // a tick under the finger as it lands, as the shell's own keyboard gives (feedbackd's click, so its profile
        // decides: none when silent), and the key's sound where the user wants it (sounds.js)
        sounds().keyTap();
        click();
        this._kbPressed.place(this._kb.view, k);
        this._kbPreviewShow(id, k);
        const key = k.key;
        if (key.a?.includes('repeat'))
            this._kbTimer(touch, REPEAT_START_MS, () => this._kbRepeat(id, 1));
        else if (key.a?.includes('longPress') && key.m?.length > 0)
            this._kbTimer(touch, key.c === 'shift' ? SHIFT_LOCK_MS : this._kb.view.m.moreKeys.longPress,
                () => this._kbLongPress(id));
    }

    _kbTimer(touch, ms, callback) {
        if (touch.timer)
            GLib.source_remove(touch.timer);
        touch.timer = GLib.timeout_add(GLib.PRIORITY_DEFAULT, ms, () => {
            touch.timer = 0;
            callback();
            return GLib.SOURCE_REMOVE;
        });
    }

    // a held delete: once now, again at the interval, a tick every other time
    _kbRepeat(id, count) {
        const touch = this._kb.touches.get(id);
        if (!touch?.k)
            return;
        touch.done = true;
        if (count % 2 === 1)
            click();
        sounds().keyTap();
        this._kbActivate(touch.k.key);
        this._kbTimer(touch, REPEAT_MS, () => this._kbRepeat(id, count + 1));
    }

    // a key held: its panel of more keys opens with the one nearest the finger chosen, or a key whose layout says
    // so (shift: the capitals locked) does its first more key at once
    _kbLongPress(id) {
        const touch = this._kb.touches.get(id);
        const view = this._kb.view;
        if (!touch?.k || !view)
            return;
        const key = touch.k.key;
        touch.done = true;
        this._kbPressed.hide();
        this._kbPreviewHide();
        if (key.mf?.includes('noPanel')) {
            click();
            this._kbActivate(key.m[0]);
            return;
        }
        const at = view.m.tablet ? touch.p[0] : touch.k.x + touch.k.w / 2;
        const panel = placeMoreKeys(view.m, touch.k, at, labelWidth);
        const [keysX, keysY] = this._kbKeys.get_transformed_position();
        this._kb.more = {id, panel, selected: -1, start: touch.p, slid: false, open: false, down: false};
        this._kbMore.place(view, panel, keysX, keysY);
        Main.layoutManager.uiGroup.set_child_above_sibling(this._kbMore, null);
        this._kbChoose(touch.p);
    }

    // the panel's key nearest the finger chosen
    _kbChoose(p) {
        const more = this._kb.more;
        const reach = MORE_KEYS_REACH_DP * this._kb.view.m.density;
        more.selected = p ? moreKeyAt(more.panel, ...p, reach) : -1;
        this._kbMore.select(more.selected);
    }

    _kbCloseMore() {
        this._kb.more = null;
        this._kbWatchOutside(false);
        if (this._kbMore) {
            this._kbMore.reactive = false;
            this._kbMore.hide();
        }
    }

    // A panel of several keys whose finger lifted where it landed stays open, nothing chosen: a tap on one of its
    // keys types that key, a tap anywhere else closes it
    _kbLeaveMoreOpen(more) {
        more.id = null;
        more.open = true;
        more.selected = -1;
        this._kbMore.select(-1);
        this._kbMore.reactive = true;
        this._kbWatchOutside(true);
    }

    // a finger on the open panel chooses the key under it; lifted, that key typed and the panel closed
    _onMoreEvent(event) {
        const more = this._kb?.more;
        if (!more?.open)
            return Clutter.EVENT_PROPAGATE;
        const type = event.type();
        const T = Clutter.EventType;
        if (type === T.TOUCH_CANCEL) {
            this._kbCloseMore();
            return Clutter.EVENT_STOP;
        }
        if (type === T.TOUCH_END || type === T.BUTTON_RELEASE) {
            const chosen = more.down && more.selected >= 0 ? more.panel.keys[more.selected].key : null;
            this._kbCloseMore();
            if (chosen)
                this._kbActivate(chosen);
            return Clutter.EVENT_STOP;
        }
        if (type === T.TOUCH_BEGIN || type === T.BUTTON_PRESS) {
            more.down = true;
            click();
        } else if (!more.down) {
            return Clutter.EVENT_STOP;
        }
        const p = this._kbPoint(event);
        more.selected = p ? moreKeyAt(more.panel, ...p, MORE_KEYS_TAP_REACH_DP * this._kb.view.m.density) : -1;
        this._kbMore.select(more.selected);
        return Clutter.EVENT_STOP;
    }

    // While a panel is open, a finger or click anywhere but on it or on the keys (which close it themselves) closes
    // it; the touch goes on to wherever it was going
    _kbWatchOutside(on) {
        if (!on) {
            if (this._kbOutsideId)
                global.stage.disconnect(this._kbOutsideId);
            this._kbOutsideId = 0;
            return;
        }
        if (this._kbOutsideId)
            return;
        this._kbOutsideId = global.stage.connect('captured-event', (_stage, event) => {
            const type = event.type();
            if (type !== Clutter.EventType.TOUCH_BEGIN && type !== Clutter.EventType.BUTTON_PRESS)
                return Clutter.EVENT_PROPAGATE;
            const target = global.stage.get_event_actor(event);
            if (this._kb?.more?.open && !this._kbMore.contains(target) && !this._kbKeys.contains(target))
                this._kbCloseMore();
            return Clutter.EVENT_PROPAGATE;
        });
    }

    // A short press's key preview: the key's label, large, on a card above the key, where a long press's panel of
    // one key opens (the long press turns the one into the other). Keys the layout gives no preview (shift, delete,
    // space, enter, the page keys) show none, nor does the tablet.
    _kbPreviewShow(id, k) {
        this._kbPreviewHide();
        const view = this._kb.view;
        const key = k.key;
        if (!view?.m.moreKeys.single || !key.l || key.a?.includes('noPreview'))
            return;
        const panel = placeMoreKeys(view.m, {...k, key: {l: key.l, m: [{l: key.l}]}}, k.x + k.w / 2, labelWidth);
        const [keysX, keysY] = this._kbKeys.get_transformed_position();
        this._kb.preview = {id, timer: 0};
        const actor = this._kbPreview;
        actor.place(view, panel, keysX, keysY);
        Main.layoutManager.uiGroup.set_child_above_sibling(actor, null);
        actor.set_scale(PREVIEW_SHOW_SCALE, PREVIEW_SHOW_SCALE);
        actor.ease({scale_x: 1, scale_y: 1, duration: PREVIEW_SHOW_MS, mode: Clutter.AnimationMode.LINEAR});
    }

    // the finger that showed the preview lifted: it stays a moment, then shrinks away
    _kbPreviewLift(id) {
        const preview = this._kb.preview;
        if (preview?.id !== id || preview.timer)
            return;
        preview.timer = GLib.timeout_add(GLib.PRIORITY_DEFAULT, PREVIEW_LINGER_MS, () => {
            preview.timer = 0;
            if (this._kb?.preview === preview) {
                this._kb.preview = null;
                this._kbPreview.ease({
                    scale_x: PREVIEW_DISMISS_SCALE, scale_y: PREVIEW_DISMISS_SCALE,
                    duration: PREVIEW_DISMISS_MS, mode: Clutter.AnimationMode.LINEAR,
                    onComplete: () => {
                        if (!this._kb?.preview)
                            this._kbPreview?.hide();
                    },
                });
            }
            return GLib.SOURCE_REMOVE;
        });
    }

    // the preview gone at once
    _kbPreviewHide() {
        const preview = this._kb?.preview;
        if (preview?.timer)
            GLib.source_remove(preview.timer);
        if (this._kb)
            this._kb.preview = null;
        this._kbPreview?.remove_all_transitions();
        this._kbPreview?.hide();
    }

    _kbMove(id, event) {
        const touch = this._kb.touches.get(id);
        if (!touch)
            return;
        touch.p = this._kbPoint(event) ?? touch.p;
        const more = this._kb.more;
        if (more?.id === id) {
            const slide = MORE_KEYS_SLIDE_DP * this._kb.view.m.density;
            if (!more.slid && more.start && touch.p &&
                Math.hypot(touch.p[0] - more.start[0], touch.p[1] - more.start[1]) > slide)
                more.slid = true;
            this._kbChoose(touch.p);
        }
    }

    // A finger up: the key typed, unless holding it did something already; with a panel open, the panel's chosen
    // key typed (none chosen: nothing)
    _kbRelease(id, cancelled) {
        const touch = this._kb.touches.get(id);
        if (!touch)
            return;
        if (touch.timer)
            GLib.source_remove(touch.timer);
        this._kb.touches.delete(id);
        if (this._kb.touches.size === 0)
            this._kbPressed.hide();
        if (cancelled && this._kb.preview?.id === id)
            this._kbPreviewHide();
        else
            this._kbPreviewLift(id);
        const more = this._kb.more?.id === id ? this._kb.more : null;
        if (more) {
            // held and lifted without sliding, on a key with several more keys: the panel stays for a tap
            if (!cancelled && !more.slid && more.panel.keys.length > 1) {
                this._kbLeaveMoreOpen(more);
                return;
            }
            this._kbCloseMore();
            if (!cancelled && more.selected >= 0)
                this._kbActivate(more.panel.keys[more.selected].key);
            return;
        }
        if (touch.k && !cancelled && !touch.done)
            this._kbActivate(touch.k.key);
    }

    // The fingers down kept over a new page (the capitals changed under a held delete): each on the key that is
    // now where its key was. An open panel closes, its finger typing nothing.
    _kbCarryTouches() {
        const more = this._kb.more;
        if (more) {
            const touch = this._kb.touches.get(more.id);
            if (touch)
                touch.k = null;
            this._kbCloseMore();
        }
        for (const touch of this._kb.touches.values()) {
            if (touch.k)
                touch.k = keyAt(this._kb.view.rows, touch.k.x + touch.k.w / 2, touch.k.y + touch.k.h / 2);
        }
        this._kbPressed.hide();
        // (a preview whose finger lifted stays its moment: typing a capital turns the page back as the finger lifts)
        if (this._kb.preview && this._kb.touches.has(this._kb.preview.id))
            this._kbPreviewHide();
    }

    _kbReleaseAll() {
        for (const touch of this._kb?.touches.values() ?? []) {
            if (touch.timer)
                GLib.source_remove(touch.timer);
        }
        this._kb?.touches.clear();
        this._kb?.passOn.clear();
        if (this._kb?.more)
            this._kbCloseMore();
        this._kbPressed?.hide();
        this._kbPreviewHide();
    }

    _onTouch(event) {
        const type = event.type();
        const id = touchId(event);
        if (type === Clutter.EventType.TOUCH_BEGIN)
            this._kbPress(id, event);
        else if (type === Clutter.EventType.TOUCH_UPDATE)
            this._kbMove(id, event);
        else if (type === Clutter.EventType.TOUCH_END)
            this._kbRelease(id, false);
        else if (type === Clutter.EventType.TOUCH_CANCEL)
            this._kbRelease(id, true);
        return this._kbPassOn(event) ? Clutter.EVENT_PROPAGATE : Clutter.EVENT_STOP;
    }

    _onMotion(event) {
        if (!this._kb.touches.has('pointer'))
            return Clutter.EVENT_PROPAGATE;
        this._kbMove('pointer', event);
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
        const passed = this._kbPassOn(event) ? Clutter.EVENT_PROPAGATE : Clutter.EVENT_STOP;
        if (type !== Clutter.EventType.TOUCH_END && type !== Clutter.EventType.BUTTON_RELEASE)
            return passed;
        const [x] = event.get_coords();
        const [ok, ax] = this._kbNav.transform_stage_point(x, 0);
        if (!ok || !this._kb.view)
            return passed;
        switch (this._kbNav.hitTest(this._kb.view, ax * this._kb.view.scale)) {
        case 'hide':
            this.close(true);
            break;
        case 'switch':
            this._kbNextSource();
            break;
        }
        return passed;
    }

    // A touch that lands in the bottom strip the swipe home starts in (task.js) is passed on as well as taken. Clutter
    // cancels every gesture on a touch an actor claims, so over the keyboard the swipe home never began. When it
    // does begin, the keys hear nothing more of the touch: dismissKeyboard () lets it go, typing nothing.
    // Elsewhere on the keys a touch stays theirs alone.
    _kbPassOn(event) {
        const type = event.type();
        const id = touchId(event);
        if (type === Clutter.EventType.TOUCH_BEGIN) {
            const [, y] = event.get_coords();
            this._kb.passOn.delete(id);
            if (inHomeStrip(y, SCALE))
                this._kb.passOn.add(id);
        }
        const passOn = this._kb.passOn.has(id);
        if (type === Clutter.EventType.TOUCH_END || type === Clutter.EventType.TOUCH_CANCEL)
            this._kb.passOn.delete(id);
        return passOn;
    }

    // ---- the suggestion strip ----

    // Fields that get suggestions: not passwords or PINs, numbers, addresses (web or mail) or a terminal's prompt, as
    // the AOSP keyboard offers none there either
    _kbSuggestOn() {
        const P = Clutter.InputContentPurpose, H = Clutter.InputContentHintFlags;
        const none = [P.DIGITS, P.NUMBER, P.PHONE, P.URL, P.EMAIL, P.PASSWORD, P.PIN, P.TERMINAL, P.DATE, P.TIME,
            P.DATETIME].filter(v => v !== undefined);
        const hints = this._contentHints ?? 0;
        return !none.includes(Main.inputMethod.content_purpose ?? this._kb.purpose) &&
            !(hints & (H.HIDDEN_TEXT | H.SENSITIVE_DATA)) && !!Main.inputMethod.currentFocus;
    }

    // once the events now pending are handled: the field reports its text after each key
    _kbSuggestSoon() {
        if (this._kbSuggestId || !this._kb)
            return;
        this._kbSuggestId = GLib.idle_add(GLib.PRIORITY_DEFAULT_IDLE, () => {
            this._kbSuggestId = 0;
            this._kbSuggest();
            return GLib.SOURCE_REMOVE;
        });
    }

    // The words for the strip, from the field's text before the cursor: the word being typed completed or corrected,
    // or between words the next one. None in the middle of a word or with text selected, nor where the field does not
    // tell its text. Where a space will correct the word, its correction is in the middle, in bold, and the word as
    // typed on its left (as LatinIME has them). Settings > Keyboard turns each part off.
    _kbSuggest() {
        if (!this._kb || !this._kbStrip)
            return;
        let shown = [null, null, null];
        let bold = -1;
        this._kb.suggestion = null;
        const [text, cursor, anchor] = Main.inputMethod.getSurroundingText();
        if (this._kbSuggestOn() && typeof text === 'string' && cursor !== null && (anchor ?? cursor) === cursor) {
            const before = GLib.utf8_substring(text, 0, cursor);
            const after = GLib.utf8_substring(text, cursor, -1);
            if (!/^[\p{L}\p{M}]/u.test(after)) {
                const w = wordsFor(this._kb.locale, () => this._kbSuggestSoon());
                if (w.lexicon && !w.lexicon.near && this._kb.letterRows)
                    w.lexicon.near = neighboursOf(this._kb.letterRows);
                const [typed, previous] = wordsBefore(before);
                const capitalise = !typed && this._kb.page === 'alphabet-shifted';
                const start = !previous && atSentenceStart(before.slice(0, before.length - typed.length));
                const opts = {next: textCorrection('keyboard-next-word'), start};
                let words = suggest(w.lexicon, w.learnt, typed, previous, 3, capitalise, opts);
                const fix = typed && textCorrection('keyboard-autocorrect')
                    ? autocorrection(w.lexicon, w.learnt, typed, previous, start, opts) : null;
                if (fix && fix !== typed)
                    words = [fix, typed, ...words.filter(x => x !== fix && x !== typed)].slice(0, 3);
                bold = fix && fix !== typed ? 1 : -1;
                shown = textCorrection('keyboard-suggestions') ? placeInStrip(words) : [null, null, null];
                this._kb.suggestion = {typed, previous, words: shown};
            }
        }
        this._kbStrip.setWords(shown, shown[1] ? bold : -1);
    }

    // The auto-correction a space, a mark or Enter (@str) makes of the word just typed: {typed, fixed, previous}, or
    // null. Worked out again from the field's text, as the strip may not have caught up with fast typing.
    _kbFixFor(str) {
        if (this._kb.picking || !SEPARATORS.test(str) || !this._kbSuggestOn() || !textCorrection('keyboard-autocorrect'))
            return null;
        const [text, cursor, anchor] = Main.inputMethod.getSurroundingText();
        if (typeof text !== 'string' || cursor === null || (anchor ?? cursor) !== cursor ||
            /^[\p{L}\p{M}]/u.test(GLib.utf8_substring(text, cursor, -1)))
            return null;
        const before = GLib.utf8_substring(text, 0, cursor);
        const [typed, previous] = wordsBefore(before);
        const w = wordsFor(this._kb.locale);
        if (!typed || !w.lexicon)
            return null;
        const start = !previous && atSentenceStart(before.slice(0, before.length - typed.length));
        const fixed = autocorrection(w.lexicon, w.learnt, typed, previous, start,
            {next: textCorrection('keyboard-next-word'), start});
        return fixed && fixed !== typed ? {typed, fixed, previous} : null;
    }

    // Backspace right after an auto-correction takes it back: the word as typed, with the space or mark after it, and
    // the word is learnt, so that it is not corrected again (as on LatinIME). False where the field has moved on.
    _kbRevert(fix) {
        const [text, cursor] = Main.inputMethod.getSurroundingText();
        const done = fix.fixed + fix.sep;
        const n = [...done].length;
        if (typeof text !== 'string' || cursor === null || cursor < n ||
            GLib.utf8_substring(text, cursor - n, cursor) !== done)
            return false;
        click();
        Main.inputMethod.delete_surrounding(-n, n);
        this._kbLearn(fix.previous, fix.typed);
        this._kb.picking = true;
        this._kbCommit(fix.typed + fix.sep);
        this._kb.picking = false;
        return true;
    }

    // a tap on the strip: the word in the place it landed in
    _kbStripTap(event) {
        const view = this._kb?.view;
        const words = this._kb?.suggestion?.words;
        if (!view || !words)
            return;
        const [sx, sy] = event.get_coords();
        const [ok, ax] = this._kbStrip.transform_stage_point(sx, sy);
        if (!ok)
            return;
        const {m} = view;
        let x = ax * view.scale - m.insetLeft;
        if (view.rtl)
            x = m.width - x;
        const slot = stripSlots(m).findIndex(([left, w]) => x >= left && x < left + w);
        if (slot >= 0 && words[slot])
            this._kbPick(words[slot]);
    }

    // A suggestion picked: it takes the place of the word being typed, and a space follows (taken back if the next key
    // is a full stop or a comma, which then goes before it)
    _kbPick(word) {
        const s = this._kb.suggestion;
        click();
        const n = [...s.typed].length;
        if (n > 0)
            Main.inputMethod.delete_surrounding(-n, n);
        this._kbLearn(s.previous, word);
        this._kb.picking = true;
        this._kbCommit(`${word} `);
        this._kb.picking = false;
        this._kb.pickedSpace = true;
    }

    // text typed: a space or a mark after a word makes it a word this user types (its correction, where @fix made
    // one); a full stop or a comma right after a picked suggestion goes before its space
    _kbWordEnds(str, fix = null) {
        const picked = this._kb.pickedSpace;
        this._kb.pickedSpace = false;
        if (this._kb.picking)
            return;
        const [text, cursor] = Main.inputMethod.getSurroundingText();
        const spaceBefore = typeof text === 'string' && cursor > 0 && GLib.utf8_substring(text, cursor - 1, cursor) === ' ';
        if (picked && spaceBefore && SPACE_SWALLOWERS.test(str) && Main.inputMethod.currentFocus) {
            Main.inputMethod.delete_surrounding(-1, 1);
            this._kbAfterCommit = ' ';
        }
        const s = this._kb.suggestion;
        if (fix)
            this._kbLearn(fix.previous, fix.fixed);
        else if (s?.typed && !/[\p{L}\p{M}'\u2019]/u.test(str) && this._kbSuggestOn())
            this._kbLearn(s.previous, s.typed);
    }

    _kbLearn(previous, word) {
        const w = wordsFor(this._kb.locale);
        if (!w.learnt || !this._kbSuggestOn())
            return;
        // a word capitalised only for starting a sentence is learnt as the list has it
        const lower = word.toLowerCase();
        if (word === lower.charAt(0).toUpperCase() + lower.slice(1) && w.lexicon?.freqOf.has(lower))
            word = lower;
        w.learnt.add(previous, word);
        learnSoon();
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
        const fix = this._kbFixFor(str);
        this._kbWordEnds(str, fix);
        this._kb.manualShift = false;
        if (this._kb.page === 'alphabet-shifted')
            this._kbSetPage('alphabet');
        const after = this._kbAfterCommit ?? '';
        this._kbAfterCommit = null;
        // the auto-correction: the word as typed goes, its correction comes with the space or mark
        let out = str + after;
        if (fix) {
            const n = [...fix.typed].length;
            Main.inputMethod.delete_surrounding(-n, n);
            out = fix.fixed + out;
        }
        this._kb.lastFix = fix ? {...fix, sep: str + after} : null;
        this._keyboardController.commit(out, this._modifiers)
            .then(() => this._updateLevelFromHints(true))
            .catch(logError);
    }

    _kbKeyval(keyval) {
        this._kb.pickedSpace = false;
        this._kb.lastFix = null;
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
            const fix = this._kb.lastFix;
            this._kb.lastFix = null;
            if (fix && this._kbRevert(fix))
                break;
            // as the shell's own backspace: key events, or the text input's delete for an Android window
            const alien = this._focusWindow?.is_alien?.();
            this._kb.pickedSpace = false;
            this._kb.manualShift = false;
            this._keyboardController.toggleDelete(true, alien);
            this._keyboardController.toggleDelete(false, alien);
            this._updateLevelFromHints(true);
            break;
        }
        case 'enter':
        case 'shift_enter': {
            // the word before is corrected first, as a space would
            const fix = this._kbFixFor('\n');
            if (!fix) {
                this._kbKeyval(Clutter.KEY_Return);
                break;
            }
            this._kbWordEnds('\n', fix);
            const n = [...fix.typed].length;
            Main.inputMethod.delete_surrounding(-n, n);
            this._keyboardController.commit(fix.fixed, this._modifiers)
                .then(() => this._kbKeyval(Clutter.KEY_Return))
                .catch(logError);
            break;
        }
        case 'tab':
            this._kbKeyval(Clutter.KEY_Tab);
            break;
        case 'space':
            this._kbCommit(' ');
            break;
        case 'language':
            this._kbNextSource();
            break;
        // the gear (a long press on the comma): Settings, on its keyboard page
        case 'settings':
            openSettings('keyboard');
            break;
        // the clipboard's text typed into the field
        case 'clipboard_paste':
            St.Clipboard.get_default().get_text(St.ClipboardType.CLIPBOARD, (_clipboard, text) => {
                if (text && this._kb)
                    this._kbCommit(text);
            });
            break;
        case 'emoji':
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

// vibeos.Shell, for the keys below and keyboard-fit-apps (keyboardFits)
let shellSettings = null;

// Settings > Keyboard > Text correction: the strip's words, auto-correction and the next word's suggestions (on where
// the schema has no such key yet)
function textCorrection(key) {
    shellSettings ??= vibeosSettings();
    return shellSettings?.settings_schema.has_key(key) ? shellSettings.get_boolean(key) : true;
}

// The swipe home begun: the keyboard goes at once, with the app. Its touches let go first: a gesture that takes a
// touch tells the actors under it nothing, and the key it began on would stay pressed, its long press still to come.
// The finger is the gesture's now, so the close is not held for its lift (oskfocus.js), which would never come: the
// keyboard stayed up for two seconds after the app had gone.
export function dismissKeyboard() {
    const keyboard = Main.keyboard._keyboard;
    keyboard?._kbReleaseAll?.();
    gestureTookTouches();
    keyboard?.close();
    // and one left on the screen closed (Keyboard.close does nothing to a closed one)
    keyboard?._kbSettle?.();
}

// Up, for Back: open, or left on the screen closed (which dismissKeyboard takes away)
export function keyboardShown() {
    return !!Main.keyboard.visible || !!Main.keyboard._keyboard?._kbStale;
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
    keyboard._kbSlideEnd?.();
    // (ours keeps its own count: a hide that remove_all_transitions cut short has allowed it already)
    if (keyboard._kb ? keyboard._kb.unredirect : keyboard._keyboardVisible)
        global.compositor.enable_unredirect();
    if (keyboard._kb)
        keyboard._kb.unredirect = false;
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
