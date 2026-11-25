#!/usr/bin/python3 -I
"""q6voiced-test.py: data/q6voice/vibeos-q6voiced without a phone: the PCM it finds in /proc/asound/pcm lines, and
when it opens and closes the voice PCM as calls change state (a fake PCM in place of ALSA)."""
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

    def open(self):
        self.log.append('open')
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

sys.exit(1 if fails else 0)
