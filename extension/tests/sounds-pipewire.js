// sounds-pipewire.js (run by sounds-pipewire.sh): sounds.js with real libcanberra (through GSound) against a private
// PipeWire with two null sinks, "sink-a" (the default) and "sink-b", each recorded at its monitor, and the shell's own
// Gvc mixer (when it is installed) for the streams.
//
//   click      the key's click is heard on the default sink only, and still when its file is gone after the start:
//              it came from the server's sample cache.
//   volume     the volume sound is heard on the sink whose volume changes: the default one and another one, and on the
//              new default one after the default moves.
//   throttle   twenty steps of a drag in 400 ms play a few times, not twenty.
//   silent     a muted sink and a sink at zero play nothing (counted at the player, since a muted sink is silent anyway).
//   untouched  no volume, mute or default sink changed through all of it.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import System from 'system';

import {KEY_SAMPLE, Player, Sounds} from './sounds.js';

let failures = 0;
function check(name, ok, detail = '') {
    print(ok ? 'PASS' : 'FAIL', name, detail);
    if (!ok)
        failures++;
}
const sleep = ms => new Promise(resolve => GLib.timeout_add(GLib.PRIORITY_DEFAULT, ms, () => {
    resolve();
    return GLib.SOURCE_REMOVE;
}));

async function waitFor(cond, ms = 5000) {
    const end = GLib.get_monotonic_time() + ms * 1000;
    while (!cond() && GLib.get_monotonic_time() < end)
        await sleep(20); // eslint-disable-line no-await-in-loop
    return cond();
}

function run(argv) {
    const p = Gio.Subprocess.new(argv, Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_MERGE);
    const [, out] = p.communicate_utf8(null, null);
    return out;
}

// ---- what the sinks hold: pw-dump's volume, mute and the default sink ------------------------------------------------

function snapshot() {
    const objects = JSON.parse(run(['pw-dump']));
    const out = {};
    for (const o of objects) {
        const props = o.info?.props;
        if (props?.['node.name']?.startsWith('sink-')) {
            const p = (o.info.params?.Props ?? []).find(x => 'channelVolumes' in x || 'mute' in x) ?? {};
            out[props['node.name']] = JSON.stringify({volume: p.volume, mute: p.mute, channelVolumes: p.channelVolumes});
        }
        if (o.type === 'PipeWire:Interface:Metadata' && o.props?.['metadata.name'] === 'default')
            out.default = JSON.stringify((o.metadata ?? []).filter(m => m.key === 'default.audio.sink' ||
                m.key === 'default.configured.audio.sink'));
    }
    return JSON.stringify(out);
}

// ---- recording a monitor ---------------------------------------------------------------------------------------------

class Recorder {
    constructor(sink, path) {
        this.path = path;
        this.process = Gio.Subprocess.new(['pw-record', '-P', 'stream.capture.sink=true', '--target', sink,
            '--rate', '48000', '--channels', '2', '--format', 's16', path], Gio.SubprocessFlags.STDERR_SILENCE);
    }

    // the peak of what was recorded, as a share of full scale, and the bursts of sound in it (runs above a hundredth of
    // full scale, apart by at least 50 ms of quiet)
    async stop() {
        this.process.send_signal(2);
        await new Promise(resolve => this.process.wait_async(null, () => resolve()));
        const [, bytes] = GLib.file_get_contents(this.path);
        let at = 0;
        for (let i = 12; i + 8 <= bytes.length;) {
            const id = String.fromCharCode(...bytes.slice(i, i + 4));
            const size = new DataView(bytes.buffer, bytes.byteOffset + i + 4, 4).getUint32(0, true);
            if (id === 'data') {
                at = i + 8;
                break;
            }
            i += 8 + size + (size & 1);
        }
        const samples = new Int16Array(bytes.buffer.slice(bytes.byteOffset + at,
            bytes.byteOffset + at + ((bytes.length - at) & ~1)));
        let peak = 0;
        let bursts = 0;
        let quiet = 1e9;
        const gap = 48000 * 2 * 0.05;
        for (let i = 0; i < samples.length; i++) {
            const v = Math.abs(samples[i]) / 32768;
            peak = Math.max(peak, v);
            if (v > 0.01) {
                if (quiet >= gap)
                    bursts++;
                quiet = 0;
            } else {
                quiet++;
            }
        }
        return {peak, bursts};
    }
}

async function record(action) {
    const a = new Recorder('sink-a', `${GLib.get_user_cache_dir()}/a-${GLib.get_monotonic_time()}.wav`);
    const b = new Recorder('sink-b', `${GLib.get_user_cache_dir()}/b-${GLib.get_monotonic_time()}.wav`);
    await sleep(700);
    await action();
    await sleep(900);
    return {a: await a.stop(), b: await b.stop()};
}

// ---- the shell's mixer ---------------------------------------------------------------------------------------------------

let control = null;
try {
    const Gvc = (await import('gi://Gvc')).default;
    control = new Gvc.MixerControl({name: 'sounds-pipewire'});
    control.open();
    await waitFor(() => control.get_state() === Gvc.MixerControlState.READY);
    await waitFor(() => control.get_sinks().length >= 2 && control.get_default_sink());
} catch (e) {
    print(`FAIL the shell's Gvc mixer: ${e.message}`);
    failures++;
}
const sinkOf = name => control.get_sinks().find(s => s.name === name);
const sinkA = control ? sinkOf('sink-a') : null;
const sinkB = control ? sinkOf('sink-b') : null;
check('mixer: the default sink is sink-a and the sinks are named as PipeWire names them',
    control?.get_default_sink()?.name === 'sink-a' && sinkA && sinkB, `${control?.get_default_sink()?.name}`);

class CountingPlayer extends Player {
    constructor() {
        super();
        this.count = 0;
    }

    play(attrs, done) {
        this.count++;
        return super.play(attrs, done);
    }
}

const player = new CountingPlayer();
const logs = [];
const sounds = new Sounds({settings: null, feedbackd: null, motor: false, calls: {busy: false}, player, mixer: control,
    log: line => logs.push(line)});
// the click is put in the server's cache when the shell is idle; it is the first thing played here
await sleep(1500);

const before = snapshot();

// ---- the click ----------------------------------------------------------------------------------------------------------

{
    // the file goes: only the server's cache can play it now. A file that was never cached is an error, which shows
    // that the file is what plays when the cache does not
    const copy = `${GLib.get_user_cache_dir()}/never-cached.oga`;
    GLib.file_set_contents(copy, GLib.file_get_contents(KEY_SAMPLE)[1]);
    GLib.unlink(copy);
    let error = null;
    player.play({'media.filename': copy, 'media.role': 'event', 'canberra.cache-control': 'permanent'},
        e => {
            error = e ?? 'none';
        });
    await waitFor(() => error !== null, 3000);
    check('click: a file that is not there and was never cached does not play', error !== 'none' && error !== null,
        `${error}`);

    GLib.unlink(KEY_SAMPLE);
    check('click: the file is gone', !GLib.file_test(KEY_SAMPLE, GLib.FileTest.EXISTS));
    const mark = logs.length;
    const heard = await record(async () => {
        sounds.keyTap();
    });
    check('click: heard on the default sink, from the cache', heard.a.peak > 0.005 && heard.a.bursts >= 1,
        `a: peak ${heard.a.peak.toFixed(3)} bursts ${heard.a.bursts}`);
    check('click: and not on the other sink', heard.b.peak < 0.001, `b: peak ${heard.b.peak.toFixed(4)}`);
    const sent = logs.slice(mark).find(l => l.includes('play sent'));
    check('click: the play was sent', !!sent, logs.slice(mark).join(' | '));
    print(`INFO ${sent ?? ''}`);
    for (let i = 0; i < 5; i++) {
        const n = logs.length;
        sounds.keyTap();
        await sleep(300); // eslint-disable-line no-await-in-loop
        print(`INFO ${logs.slice(n).join(' | ')}`);
    }
}

// ---- the volume sound ----------------------------------------------------------------------------------------------------

{
    const onA = await record(async () => {
        sounds.volumeChanged(sinkA);
    });
    check('volume: heard on the sink whose volume changes, the default one', onA.a.peak > 0.005 && onA.b.peak < 0.001,
        `a ${onA.a.peak.toFixed(3)} b ${onA.b.peak.toFixed(4)}`);
    await sleep(300);
    const onB = await record(async () => {
        sounds.volumeChanged(sinkB);
    });
    check('volume: and on another sink when that one\'s volume changes, not on the default',
        onB.b.peak > 0.005 && onB.a.peak < 0.001, `a ${onB.a.peak.toFixed(4)} b ${onB.b.peak.toFixed(3)}`);
    }

// ---- the throttle ---------------------------------------------------------------------------------------------------------

{
    await sleep(500);
    const mark = player.count;
    const marked = logs.length;
    const start = GLib.get_monotonic_time();
    await record(async () => {
        for (let i = 0; i < 20; i++) {
            sounds.volumeChanged(sinkA);
            await sleep(20); // eslint-disable-line no-await-in-loop
        }
    });
    const played = player.count - mark;
    const throttled = logs.slice(marked).filter(l => l.includes('throttled')).length;
    // the steps took 20 ms each and whatever the playing cost: one sound per 100 ms of them at most, the first at once
    const elapsedMs = (GLib.get_monotonic_time() - start) / 1000 - 1600;
    check('throttle: twenty steps of a drag play at most once per 100 ms, and not once per step',
        played >= 3 && played < 20 && played <= Math.floor(elapsedMs / 100) + 1,
        `played ${played}, throttled ${throttled}, in about ${Math.round(elapsedMs)} ms`);
}

// ---- untouched ---------------------------------------------------------------------------------------------------------

check('untouched: no volume, mute or default sink was changed by playing', before === snapshot(), `${before}`);

// ---- silent -------------------------------------------------------------------------------------------------------------

{
    const mark = player.count;
    const oldVolume = sinkA.volume;
    sinkA.change_is_muted(true);
    await waitFor(() => sinkA.is_muted);
    sounds.volumeChanged(sinkA);
    sounds.keyTap();
    check('silent: a muted sink plays nothing', player.count === mark && logs.some(l => l.includes('silent (muted)')),
        logs.slice(-2).join(' | '));
    sinkA.change_is_muted(false);
    await waitFor(() => !sinkA.is_muted);
    sinkA.volume = 0;
    sinkA.push_volume();
    await waitFor(() => sinkA.volume === 0);
    await sleep(300);
    sounds.volumeChanged(sinkA);
    sounds.keyTap();
    check('silent: a sink at zero plays nothing', player.count === mark && logs.some(l => l.includes('silent (volume 0)')),
        logs.slice(-2).join(' | '));
    sinkA.volume = oldVolume;
    sinkA.push_volume();
    await sleep(500);
}

// ---- another default -----------------------------------------------------------------------------------------------------

{
    run(['pw-metadata', '-n', 'default', '0', 'default.configured.audio.sink', '{ "name": "sink-b" }']);
    const moved = await waitFor(() => control.get_default_sink()?.name === 'sink-b');
    check('default: the mixer follows the default sink to sink-b', moved);
    await sleep(800);
    const heard = await record(async () => {
        sounds.volumeChanged(sinkB);
    });
    check('default: now it is sink-b whose volume change is heard, and sink-a\'s is not', heard.b.peak > 0.005 &&
        heard.a.peak < 0.001, `a ${heard.a.peak.toFixed(4)} b ${heard.b.peak.toFixed(3)}`);
    const keys = await record(async () => {
        sounds.keyTap();
    });
    check('default: and the click follows it, from the cache', keys.b.peak > 0.005 && keys.a.peak < 0.001,
        `a ${keys.a.peak.toFixed(4)} b ${keys.b.peak.toFixed(3)}`);
}

sounds.destroy();
print(failures ? `${failures} FAILED` : 'all passed');
System.exit(failures ? 1 : 0);
