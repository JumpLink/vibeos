// The shell's two small sounds, as Android plays them: a click under each key of the on-screen keyboard, and the sound
// theme's volume sound at each step of the volume.
//
// On or off. Both are on where the device has no vibration motor (a tablet: its keys cannot tick, so they click) and
// off where it has one (a phone keeps its haptics: keyhaptics.js, haptics.js). A key's default in a schema cannot
// depend on the device, so vibeos.Shell's keyboard-sounds and volume-change-sound are read by their user value: until
// the user sets one (get_user_value is null) it is the device's default, and once set it is the user's. Settings reads
// them the same way and shows the effective value (apps/settings/src/ns-sound.c).
//
// Silent, always: in feedbackd's Vibrate and Mute modes, during a call (the Phone's service or ModemManager says one
// is ringing, dialling or up), and while the stream they would play on is muted or at zero. Nothing here sets a volume
// or moves a stream to be heard.
//
// Played with libcanberra through GSound, the one binding of it GJS has: its play_full does not wait for the sound, and
// a sample cached with canberra.cache-control permanent sits in PipeWire's pulse sample cache, so a key's click starts
// a stream from the server's memory instead of decoding the file. GNOME's own media keys change the canberra device to
// the sink whose volume changes before they play (ca_context_change_device); GSound has no call for that, so the volume
// sound names its sink as the stream's target.object, which libcanberra hands on to PipeWire with the stream's other
// properties.
//
// A flag file, $XDG_RUNTIME_DIR/vibeos-sound-log (or VIBEOS_SOUND_LOG=1 in the shell's environment), logs each tap and
// volume sound with the monotonic clock (see README in the sounds directory).
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import {FlagFile} from './flagfile.js';

export const SCHEMA = 'vibeos.Shell';
export const KEYBOARD_KEY = 'keyboard-sounds';
export const VOLUME_KEY = 'volume-change-sound';
export const THROTTLE_MS = 100;

const FEEDBACKD_SCHEMA = 'org.sigxcpu.feedbackd';
const DIR = GLib.path_get_dirname(GLib.filename_from_uri(import.meta.url)[0]);
export const KEY_SAMPLE = GLib.build_filenamev([DIR, 'sounds', 'key-tap.oga']);
export const VOLUME_EVENT = 'audio-volume-change';

// ---- is there a vibration motor -------------------------------------------------------------------------------------

function readText(path) {
    try {
        const [ok, bytes] = GLib.file_get_contents(path);
        return ok ? new TextDecoder().decode(bytes) : null;
    } catch (e) {
        return null;
    }
}

function listDir(path) {
    const names = [];
    try {
        const enumerator = Gio.File.new_for_path(path).enumerate_children('standard::name', Gio.FileQueryInfoFlags.NONE,
            null);
        for (let info = enumerator.next_file(null); info; info = enumerator.next_file(null))
            names.push(info.get_name());
        enumerator.close(null);
    } catch (e) {
        // no such directory
    }
    return names;
}

// hexadecimal words: anything but zeros and spaces is a bit set
const anyBit = text => text !== null && /[1-9a-fA-F]/.test(text);

// The motor, by any of three signs (apps/settings/src/ns-motor.c is the same rule):
//  - an input event node that plays force feedback (its "ff" capability is not zero), which feedbackd's rules drive as a
//    vibra, and is not a game controller (rumble with absolute axes, which comes and goes);
//  - an LED named for a vibrator (some phones drive the motor as the LED "vibrator");
//  - an input node feedbackd's rules tagged FEEDBACKD_TYPE=vibra in udev's database.
export function hasMotor({input = '/sys/class/input', leds = '/sys/class/leds', udev = '/run/udev/data'} = {}) {
    for (const name of listDir(input)) {
        if (!name.startsWith('event'))
            continue;
        const caps = `${input}/${name}/device/capabilities`;
        if (anyBit(readText(`${caps}/ff`)) && !anyBit(readText(`${caps}/abs`)))
            return true;
    }
    if (listDir(leds).some(name => name.toLowerCase().includes('vibrator')))
        return true;
    for (const name of listDir(udev)) {
        if (!name.startsWith('c13:'))
            continue;
        const text = readText(`${udev}/${name}`);
        if (text !== null && `\n${text}`.includes('\nE:FEEDBACKD_TYPE=vibra\n'))
            return true;
    }
    return false;
}

// whether @key is on: the user's value when there is one, else the device's default (on without a motor)
export function enabled(settings, key, motor) {
    const user = settings?.get_user_value(key) ?? null;
    return user ? user.get_boolean() : !motor;
}

// ---- when they are silent ---------------------------------------------------------------------------------------------

// ModemManager's MMCallState: dialling, ringing out, ringing in, active, held and waiting are a call; unknown and
// terminated are not
const MM_CALL_STATES = [1, 2, 3, 4, 5, 6];

// the Phone's CallState (apps/dialer/src/call-types.h): connecting up to on hold are a call, disconnecting and
// disconnected are not
const DIALER_LAST_LIVE_STATE = 5;

export const callUp = {
    modemManager: states => states.some(state => MM_CALL_STATES.includes(state)),
    dialer: states => states.some(state => state <= DIALER_LAST_LIVE_STATE),
};

// why a sound is not played, or null. @profile feedbackd's, @inCall, @streams the streams it would play on (a sink, the
// event stream when there is one), each with is_muted and volume
export function silence({profile, inCall, streams}) {
    if (profile === 'quiet' || profile === 'silent')
        return `profile ${profile}`;
    if (inCall)
        return 'call';
    for (const stream of streams) {
        if (!stream)
            return 'no stream';
        if (stream.is_muted)
            return 'muted';
        if (stream.volume === 0)
            return 'volume 0';
    }
    return null;
}

// at most one in @ms: a call counts when none was let through in the last @ms (a leading edge, so the first step of a
// drag is heard at once and the rest at the interval)
export class Throttle {
    constructor(ms = THROTTLE_MS, clock = () => GLib.get_monotonic_time() / 1000) {
        this._ms = ms;
        this._clock = clock;
        this._last = -Infinity;
    }

    allow() {
        const now = this._clock();
        if (now - this._last < this._ms)
            return false;
        this._last = now;
        return true;
    }
}

// ---- calls ------------------------------------------------------------------------------------------------------------

const MM_NAME = 'org.freedesktop.ModemManager1';
const MM_CALL = 'org.freedesktop.ModemManager1.Call';
const DIALER_NAME = 'vibeos.Dialer.Calls';
const DIALER_PATH = '/vibeos/Dialer/Calls';

// Whether a call is on, from the Phone's service (vibeos.Dialer.Calls, on the session bus: its Calls property) and from
// ModemManager itself (every Call object's State, on the system bus: for a call the Phone's service does not handle,
// or while it is not running). Neither is started by this.
export class CallWatch {
    constructor({session = Gio.DBus.session, system = Gio.DBus.system} = {}) {
        this._cancellable = new Gio.Cancellable();
        this._dialer = false;
        this._modem = false;
        this._proxy = null;
        this._manager = null;
        this._watchDialer(session);
        this._watchModem(system);
    }

    get busy() {
        return this._dialer || this._modem;
    }

    _watchDialer(bus) {
        if (!bus)
            return;
        Gio.DBusProxy.new(bus, Gio.DBusProxyFlags.DO_NOT_AUTO_START, null, DIALER_NAME, DIALER_PATH, DIALER_NAME,
            this._cancellable, (_o, res) => {
                try {
                    this._proxy = Gio.DBusProxy.new_finish(res);
                } catch (e) {
                    return;
                }
                this._proxy.connect('g-properties-changed', () => this._readDialer());
                this._readDialer();
            });
    }

    _readDialer() {
        const calls = this._proxy?.get_cached_property('Calls');
        const states = [];
        for (let i = 0; calls && i < calls.n_children(); i++) {
            const state = calls.get_child_value(i).lookup_value('state', null);
            if (state)
                states.push(state.get_uint32());
        }
        this._dialer = callUp.dialer(states);
    }

    _watchModem(bus) {
        if (!bus)
            return;
        Gio.DBusObjectManagerClient.new(bus, Gio.DBusObjectManagerClientFlags.DO_NOT_AUTO_START, MM_NAME,
            '/org/freedesktop/ModemManager1', null, this._cancellable, (_o, res) => {
                try {
                    this._manager = Gio.DBusObjectManagerClient.new_finish(res);
                } catch (e) {
                    return;
                }
                this._manager.connect('object-added', () => this._readModem());
                this._manager.connect('object-removed', () => this._readModem());
                this._manager.connect('interface-added', () => this._readModem());
                this._manager.connect('interface-removed', () => this._readModem());
                this._manager.connect('interface-proxy-properties-changed', () => this._readModem());
                // the manager's owner coming or going: every cached value changes
                this._manager.connect('notify::name-owner', () => this._readModem());
                this._readModem();
            });
    }

    _readModem() {
        const states = [];
        for (const object of this._manager?.get_objects() ?? []) {
            const state = object.get_interface(MM_CALL)?.get_cached_property('State');
            if (state)
                states.push(state.get_int32());
        }
        this._modem = callUp.modemManager(states);
    }

    destroy() {
        this._cancellable.cancel();
        this._proxy = null;
        this._manager = null;
    }
}

// ---- playing ----------------------------------------------------------------------------------------------------------

// libcanberra through GSound, which may not be there (the extension must load without it)
export class Player {
    // @GSound the module, or null: this one imports it when it is first needed
    constructor(GSound = null) {
        this._GSound = GSound;
        this._tried = GSound !== null;
        this._context = null;
    }

    async load() {
        if (!this._tried) {
            this._tried = true;
            try {
                this._GSound = (await import('gi://GSound')).default;
            } catch (e) {
                console.warn(`vibeos: no GSound, no shell sounds: ${e.message}`);
            }
        }
        return this._GSound !== null;
    }

    // the context, connected to the sound server when it is first asked for: a short wait, once. A server that went away
    // takes it with it, and the next sound opens a new one.
    _open() {
        if (!this._context) {
            const context = new this._GSound.Context();
            context.init(null);
            context.open?.();
            this._context = context;
        }
        return this._context;
    }

    // put @attrs's sample in the server's cache, so that the first sound of it is as fast as the rest
    preload(attrs) {
        if (!this._GSound)
            return false;
        try {
            this._open().cache(attrs);
            return true;
        } catch (e) {
            this._context = null;
            console.warn(`vibeos: sound cache failed: ${e.message}`);
            return false;
        }
    }

    // start @attrs; @done(error) when it has played. Does not wait for the sound.
    play(attrs, done) {
        if (!this._GSound)
            return false;
        try {
            this._open().play_full(attrs, null, (context, res) => {
                try {
                    context.play_full_finish(res);
                    done?.(null);
                } catch (e) {
                    this._context = null;
                    done?.(e);
                }
            });
            return true;
        } catch (e) {
            this._context = null;
            console.warn(`vibeos: sound failed: ${e.message}`);
            return false;
        }
    }

    destroy() {
        this._context = null;
    }
}

// ---- the two sounds ---------------------------------------------------------------------------------------------------

export class Sounds {
    // everything but the default one can be replaced, for the tests: @settings vibeos.Shell's, @feedbackd feedbackd's,
    // @motor whether there is a vibration motor, @calls {busy}, @player a Player, @mixer {defaultSink(), eventStream()},
    // @clock the throttle's milliseconds, @log a function that takes a line
    constructor({settings, feedbackd, motor, calls, player, mixer, clock, log} = {}) {
        const source = Gio.SettingsSchemaSource.get_default();
        const lookup = id => source?.lookup(id, true) ? new Gio.Settings({schema_id: id}) : null;
        this._settings = settings === undefined ? lookup(SCHEMA) : settings;
        this._feedbackd = feedbackd === undefined ? lookup(FEEDBACKD_SCHEMA) : feedbackd;
        this._motor = motor ?? hasMotor();
        this._calls = calls ?? new CallWatch();
        this._player = player ?? new Player();
        this._mixer = mixer ?? null;
        this._throttle = new Throttle(THROTTLE_MS, clock);
        this._flag = log ? null : new FlagFile('vibeos-sound-log');
        this._log = log ?? (line => {
            if (this._flag.on || GLib.getenv('VIBEOS_SOUND_LOG'))
                console.log(`vibeos-sound: ${line}`);
        });
        this._now = clock ? () => Math.round(clock() * 1000) : () => GLib.get_monotonic_time();
        this._preloadTries = 0;
        this._preloadId = 0;
        this._destroyed = false;
        // the key's sample goes into the server's cache once the session is up and the shell is idle
        this._preloadId = GLib.idle_add(GLib.PRIORITY_LOW, () => {
            this._preloadId = 0;
            this._preload();
            return GLib.SOURCE_REMOVE;
        });
    }

    // @mixer: the shell's Gvc.MixerControl (the default sink, the event stream)
    setMixer(mixer) {
        this._mixer = mixer;
    }

    get motor() {
        return this._motor;
    }

    isEnabled(key) {
        return enabled(this._settings, key, this._motor);
    }

    _attrs() {
        // the event id is the sample's name in the server's cache (which canberra needs to cache it); the file is what is
        // loaded into it
        return {
            'event.id': 'vibeos-key-tap',
            'media.filename': KEY_SAMPLE,
            'media.role': 'event',
            'media.name': 'Key tap',
            'canberra.cache-control': 'permanent',
        };
    }

    async _preload() {
        if (this._destroyed)
            return;
        // on a phone too: the switch can be turned on at any time, and the first click must not wait
        if (!await this._player.load())
            return;
        if (this._destroyed)
            return;
        if (this._player.preload(this._attrs()))
            return;
        // pipewire-pulse was not up yet: again in a few seconds, a few times
        if (++this._preloadTries < 6) {
            this._preloadId = GLib.timeout_add_seconds(GLib.PRIORITY_LOW, 5, () => {
                this._preloadId = 0;
                this._preload();
                return GLib.SOURCE_REMOVE;
            });
        }
    }

    _streams(sink) {
        const streams = [sink];
        const event = this._mixer?.get_event_sink_input?.() ?? null;
        if (event)
            streams.push(event);
        return streams;
    }

    _silence(sink) {
        return silence({
            profile: this._feedbackd?.get_string('profile') ?? 'full',
            inCall: this._calls.busy,
            streams: this._streams(sink),
        });
    }

    // a key of the keyboard went down
    keyTap() {
        if (this._destroyed || !this.isEnabled(KEYBOARD_KEY))
            return;
        const t0 = this._now();
        const sink = this._mixer?.get_default_sink?.() ?? null;
        const reason = this._silence(sink);
        if (reason) {
            this._log(`tap t=${t0} silent (${reason})`);
            return;
        }
        const sent = this._player.play(this._attrs(), error => {
            this._log(`tap t=${t0} done t=${this._now()} (${this._now() - t0} us)${error ? ` error ${error.message}` : ''}`);
        });
        this._log(`tap t=${t0} play ${sent ? 'sent' : 'not sent'} t=${this._now()} (${this._now() - t0} us)`);
    }

    // the volume of @stream (a sink) was just changed by a step of a key or the slider
    volumeChanged(stream) {
        if (this._destroyed || !this.isEnabled(VOLUME_KEY))
            return;
        const t0 = this._now();
        if (!stream) {
            this._log(`volume t=${t0} no stream`);
            return;
        }
        const reason = this._silence(stream);
        if (reason) {
            this._log(`volume t=${t0} silent (${reason})`);
            return;
        }
        if (!this._throttle.allow()) {
            this._log(`volume t=${t0} throttled`);
            return;
        }
        const sent = this._player.play({
            'event.id': VOLUME_EVENT,
            'media.role': 'event',
            'media.name': 'Volume changed',
            'canberra.xdg-theme.name': 'freedesktop',
            // the sink whose volume changed, wherever the default is
            'target.object': stream.name,
        }, error => {
            this._log(`volume t=${t0} done t=${this._now()}${error ? ` error ${error.message}` : ''}`);
        });
        this._log(`volume t=${t0} play ${sent ? 'sent' : 'not sent'} sink=${stream.name} ` +
            `t=${this._now()} (${this._now() - t0} us)`);
    }

    destroy() {
        this._destroyed = true;
        if (this._preloadId)
            GLib.source_remove(this._preloadId);
        this._preloadId = 0;
        this._calls.destroy?.();
        this._flag?.destroy();
        this._player.destroy();
    }
}

let shared = null;

// the shell's one, made at the first use
export function sounds() {
    shared ??= new Sounds();
    return shared;
}

export function destroySounds() {
    shared?.destroy();
    shared = null;
}
