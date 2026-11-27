#!/usr/bin/python3 -I
"""q6voiced-test.py: data/q6voice/vibeos-q6voiced without a phone: the PCM it finds in /proc/asound/pcm lines,
when it opens and closes the voice PCM as calls change state (a fake PCM in place of ALSA), and which mixer changes
reopen it during a call."""
import importlib.machinery, importlib.util, os, sys, tempfile

HERE = os.path.dirname(os.path.realpath(__file__))
S = os.path.join(HERE, '..', 'data', 'q6voice', 'vibeos-q6voiced')
loader = importlib.machinery.SourceFileLoader('q6voiced', S)
spec = importlib.util.spec_from_loader('q6voiced', loader)
q = importlib.util.module_from_spec(spec)
loader.exec_module(q)
q.log = lambda *m: None
fails = 0


def check(what, got, want):
    global fails
    ok = got == want
    fails += not ok
    print('%s %s: %r%s' % ('ok  ' if ok else 'FAIL', what, got, '' if ok else ' (want %r)' % (want,)))


def pcm_file(text):
    f = tempfile.NamedTemporaryFile('w', delete=False)
    f.write(text)
    f.close()
    return f.name


# the OnePlus 6T's /proc/asound/pcm (sdm845, mainline with q6voice)
p = pcm_file('00-00: MultiMedia1 (*) :  : playback 1\n00-01: MultiMedia2 (*) :  : capture 1\n'
             '00-06: VoiceMMode1 (*) :  : playback 1 : capture 1\n')
check('6T voice PCM', q.find_device(p), 'hw:0,6')
check('no voice PCM', q.find_device(pcm_file('00-00: MultiMedia1 (*) :  : playback 1\n')), None)
check('a PCM named VoiceMMode10 is not it', q.find_device(pcm_file('01-03: VoiceMMode10 :  : capture 1\n')), None)
check('no /proc/asound', q.find_device('/nonexistent'), None)


class FakePcm:
    def __init__(self, ok=True):
        self.ok, self.log = ok, []

    def open(self, seen=0):
        self.log.append('reopen' if seen else 'open')
        return self.ok

    def close(self):
        self.log.append('close')


DIALING, RINGING_OUT, RINGING_IN, ACTIVE, HELD, WAITING, TERMINATED = 1, 2, 3, 4, 5, 6, 7

f = FakePcm(); v = q.Voice(f)
for st in (RINGING_IN, ACTIVE, TERMINATED):
    v.state('/Call/1', st)
check('incoming: opens on answer, closes on hangup', f.log, ['open', 'close'])

f = FakePcm(); v = q.Voice(f)
for st in (DIALING, RINGING_OUT, ACTIVE, TERMINATED):
    v.state('/Call/2', st)
check('outgoing: opens on dialing, once', f.log, ['open', 'close'])

f = FakePcm(); v = q.Voice(f)
v.state('/Call/1', ACTIVE); v.state('/Call/2', WAITING); v.state('/Call/1', HELD); v.state('/Call/2', ACTIVE)
v.state('/Call/1', TERMINATED)
check('second call: stays open while one is live', f.log, ['open'])
v.state('/Call/2', TERMINATED)
check('second call: closes after the last', f.log, ['open', 'close'])

f = FakePcm(); v = q.Voice(f)
v.state('/Call/1', RINGING_IN); v.state('/Call/1', TERMINATED)
check('missed call: never opens', f.log, [])

f = FakePcm(ok=False); v = q.Voice(f)
v.state('/Call/1', ACTIVE); v.state('/Call/1', TERMINATED)
check('open failed: no close', f.log, ['open'])

# the sound card registers after the service started: looked up again at the next call, not given up
class FakeLib:
    def __init__(self):
        self.opened = []

    def snd_pcm_open(self, h, dev, stream, mode):
        self.opened.append(dev)
        return 0

    def __getattr__(self, name):
        return lambda *a: 0


card = {'dev': None}
a = q.Alsa.__new__(q.Alsa)
a.lib, a.dev, a.find, a.handles = FakeLib(), None, lambda: card['dev'], []
a.settle = type('NoSettle', (), {'wait': lambda self, card, seen=0: 0})()
check('late card: no PCM at the first call', a.open(), False)
card['dev'] = 'hw:0,6'
check('late card: opens at the next call', a.open(), True)
check('late card: both directions on the PCM found', a.lib.opened, [b'hw:0,6', b'hw:0,6'])

# both ways started: the codec turns the earpiece's SLIMbus channel on only when playback starts, and playback,
# with no data, starts only with its stop threshold at the boundary
BOUNDARY = 0x4000000000000000


class RecordingLib:
    def __init__(self):
        self.calls, self.next = [], 1

    def snd_pcm_open(self, h, dev, stream, mode):
        h._obj.value = self.next
        self.next += 1
        return 0

    def snd_pcm_sw_params_get_boundary(self, sw, b):
        b._obj.value = BOUNDARY
        return 0

    def snd_pcm_sw_params_set_stop_threshold(self, h, sw, v):
        self.calls.append(('stop_threshold', h.value, v.value))
        return 0

    def __getattr__(self, name):
        def call(*a):
            if name in ('snd_pcm_prepare', 'snd_pcm_start'):
                self.calls.append((name[8:], a[0].value))
            return 0
        return call


a = q.Alsa.__new__(q.Alsa)
a.lib, a.dev, a.find, a.handles = RecordingLib(), 'hw:0,6', None, []
a.settle = type('Settled', (), {'wait': lambda self, card, seen=0: a.lib.calls.append(('settle', card)) or 0})()
check('started: opens', a.open(), True)
check('started: capture (1) and playback (2) both started, playback never stopping',
      a.lib.calls, [('settle', '0'), ('prepare', 1), ('start', 1), ('stop_threshold', 2, BOUNDARY), ('prepare', 2), ('start', 2)])

# the earpiece route comes from the audio manager on the same call signal: the stream starts only after the mixer
# went quiet. A fake control device: each wait either sees a change (and takes 0.1 s) or times out


class CtlLib:
    def __init__(self, changes):
        self.changes, self.now, self.waits, self.closed = list(changes), 0.0, [], False

    def snd_ctl_wait(self, ctl, ms):
        self.waits.append(ms)
        if self.changes and self.changes[0] <= self.now + ms / 1000:
            self.now = max(self.now, self.changes.pop(0))
            return 1
        self.now += ms / 1000
        return 0

    def snd_ctl_close(self, ctl):
        self.closed = True

    def __getattr__(self, name):
        return lambda *a: 0


def settle(changes, seen=0):
    lib = CtlLib(changes)
    n = q.Settle(lib, clock=lambda: lib.now).wait('0', seen)
    return n, round(lib.now, 2), lib.closed


check('settle: a profile switch 0.8 s in, 3 controls', settle([0.8, 0.85, 0.9]), (3, 1.4, True))
check('settle: nothing changes, waits FIRST only', settle([]), (0, 2.0, True))
check('settle: a mixer that never quiets is left after MOST', settle([i / 10 for i in range(100)])[:2], (50, 5.0))
check('settle: after a reroute seen, only QUIET', settle([], seen=1), (1, 0.5, True))

# the speaker button: the audio manager switches the Voice Call profile, the earpiece's voice mixer goes off and the
# speaker's on, and the DSP plays on the new port only in a new voice session


class EventLib:
    """a control device with events queued: (name, mask) each"""
    def __init__(self, events):
        self.events, self.cur = list(events), None

    def snd_ctl_read(self, ctl, ev):
        if not self.events:
            return -11  # -EAGAIN: the control is non-blocking
        self.cur = self.events.pop(0)
        return 1

    def snd_ctl_event_get_type(self, ev):
        return 0

    def snd_ctl_event_elem_get_name(self, ev):
        return self.cur[0].encode()

    def snd_ctl_event_elem_get_mask(self, ev):
        return self.cur[1]

    def __getattr__(self, name):
        return lambda *a: 0


def rerouted(*events):
    m = q.Mixer(EventLib(events))
    m.ctl = 1
    return m.rerouted(), m.lib.events


VALUE, REMOVE = 1, 0xffffffff
check('mixer: the speaker taking the call', rerouted(("SLIMBUS_0_RX Voice Mixer VoiceMMode1", VALUE),
                                                     ("QUAT_MI2S_RX Voice Mixer VoiceMMode1", VALUE)), (True, []))
check('mixer: a headset mic recording the call', rerouted(("VoiceMMode1 Capture Mixer TERT_MI2S_TX", VALUE)), (True, []))
check('mixer: a volume key is no reroute', rerouted(("RX0 Digital Volume", VALUE), ("SLIM RX0 MUX", VALUE)), (False, []))
check('mixer: a music route is no reroute', rerouted(("QUAT_MI2S_RX Audio Mixer MultiMedia1", VALUE)), (False, []))
check('mixer: a control removed is no reroute', rerouted(("QUAT_MI2S_RX Voice Mixer VoiceMMode1", REMOVE)), (False, []))
check('mixer: not watching, nothing read', q.Mixer(EventLib([("x Voice Mixer y", VALUE)])).rerouted(), False)

f = FakePcm(); v = q.Voice(f)
v.state('/Call/1', ACTIVE); v.reroute(); v.state('/Call/1', TERMINATED)
check('reroute: closed and opened again, without the first wait', f.log, ['open', 'close', 'reopen', 'close'])

f = FakePcm(); v = q.Voice(f)
v.reroute()
check('reroute: no call, nothing opened', f.log, [])

f = FakePcm(ok=False); v = q.Voice(f)
v.state('/Call/1', ACTIVE); f.ok = True; v.reroute(); v.state('/Call/1', TERMINATED)
check('reroute: the open that failed is tried again', f.log, ['open', 'reopen', 'close'])

sys.exit(1 if fails else 0)
