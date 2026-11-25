#!/usr/bin/python3
# vibeos-dialer-calls with a GStreamer whose start never finishes (the plugin scanner, which opens every device, sleeps
# for good: a camera driver that never answers does that): the service keeps answering, a ringing call is announced at
# once, and the call can be answered. Buses and mock phone as test-calls.py has them.
#
# usage: test-calls-gst-hang.py BUILD_DIR SOURCE_DIR [healthy]
#
# With "healthy" GStreamer starts as it should, from a registry made first (the same steps without the stuck scanner, for
# the times to compare): the ringtone then plays, which the service's log says.
import importlib.util
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import time

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('calls_test', os.path.join(here, 'test-calls.py'))
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)
check, Test = base.check, base.Test

# how soon the service must answer: GStreamer's own wait for a sound is 10 s, so anything under it is not that wait
PROMPT = 5.0


def run(t, started, log, healthy):
    t.wait('the service is ready', lambda: t.prop('Status') == 'ready', timeout=120)
    print('info service ready %.1f s after its start' % (time.time() - started), flush=True)
    # the dialpad opens: the tones' stream waits for GStreamer, and the service goes on
    began = time.time()
    t.service('PrepareTones')
    check('the dialpad opening is answered at once', time.time() - began < PROMPT)
    t.phone('Clear')
    began = time.time()
    path = t.phone('Incoming', ('+491701234567',), '(s)')[0]
    t.wait('the answer screen is asked for at once',
           lambda: ['LaunchFullScreen', 'vibeos.Dialer.InCall'] in t.logged('LaunchFullScreen'), timeout=PROMPT)
    print('info the answer screen was asked for %.1f s after the call came' % (time.time() - began), flush=True)
    call_id = t.wait('INCOMING, and the service answers', lambda: next((c['id'] for c in t.calls()
                                                                       if c['state'] == base.INCOMING), 0),
                     timeout=PROMPT)
    # past GStreamer's wait: the ring gives up its sound, and nothing else changes
    t.settle(11)
    check('the service still answers', t.prop('Status') == 'ready')
    t.service('Answer', (call_id,), '(u)')
    t.wait('Accept on the call', lambda: t.logged('Accept'))
    t.wait('ACTIVE', lambda: t.call(call_id)['state'] == base.ACTIVE)
    t.phone('RemoteHangup', (path, base.R_TERMINATED), '(oi)')
    t.wait('they hung up: REMOTE', lambda: t.call(call_id)['end'] == base.END_REMOTE)
    t.wait('the last call has gone', lambda: not t.calls())

    # a second call rings: still announced at once
    t.phone('Clear')
    began = time.time()
    t.phone('Incoming', ('+491701234568',), '(s)')
    t.wait('a second call is announced at once',
           lambda: ['LaunchFullScreen', 'vibeos.Dialer.InCall'] in t.logged('LaunchFullScreen'), timeout=PROMPT)
    print('info the second call was announced %.1f s after it came' % (time.time() - began), flush=True)
    with open(log) as f:
        text = f.read()
    if healthy:
        check('GStreamer came up, and the ringtone went on to play (no sound server is there to take it)',
              'gstreamer: up' in text and 'ringer: cannot play' in text, text[-600:])
    else:
        check('the ring gave up the sound after its wait', 'not up after' in text and 'gstreamer: up' not in text,
              text[-600:])


def main():
    build, source = sys.argv[1], sys.argv[2]
    healthy = sys.argv[3:] == ['healthy']
    tmp = tempfile.mkdtemp(prefix='test-calls-hang-')
    procs = []
    try:
        subprocess.run(['glib-compile-schemas', '--targetdir', tmp, os.path.join(source, 'data')], check=True)
        sounds = os.path.join(tmp, 'share', 'vibeos-dialer', 'sounds')
        os.makedirs(sounds)
        shutil.copy(os.path.join(source, 'data', 'sounds', 'call-recording-en.opus'), sounds)
        scanner = os.path.join(tmp, 'stuck-scanner')
        with open(scanner, 'w') as f:
            f.write('#!/bin/sh\nexec sleep 100000\n')
        os.chmod(scanner, os.stat(scanner).st_mode | stat.S_IXUSR)
        system, system_address = base.start_bus(tmp, 'system')
        session, session_address = base.start_bus(tmp, 'session')
        procs += [system, session]
        env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS=session_address, VIBEOS_DIALER_SYSTEM_BUS=system_address,
                   XDG_DATA_HOME=os.path.join(tmp, 'data'), XDG_STATE_HOME=os.path.join(tmp, 'state'),
                   XDG_CONFIG_HOME=os.path.join(tmp, 'config'), GSETTINGS_BACKEND='keyfile',
                   GSETTINGS_SCHEMA_DIR=tmp, PULSE_SERVER='unix:' + os.path.join(tmp, 'no-sound'),
                   HOME=os.path.join(tmp, 'home'), XDG_CACHE_HOME=os.path.join(tmp, 'cache'),
                   XDG_DATA_DIRS=os.path.join(tmp, 'share') + ':' + os.environ.get('XDG_DATA_DIRS',
                                                                                  '/usr/local/share:/usr/share'),
                   ALSA_CONFIG_UCM2=os.path.join(source, 'tests', 'ucm2'), VIBEOS_DIALER_UCM_CARD='RecordingPhone',
                   GST_REGISTRY_1_0=os.path.join(tmp, 'no-registry.bin'), GST_REGISTRY=os.path.join(tmp, 'no-registry.bin'))
        if healthy:
            del env['GST_REGISTRY']
            env.update(base.gst_env(tmp))
            base.gst_warm(env)
        else:
            env.update(GST_PLUGIN_SCANNER_1_0=scanner, GST_PLUGIN_SCANNER=scanner)
        log = os.path.join(tmp, 'service.log')
        mock = subprocess.Popen([sys.executable, os.path.join(source, 'tests', 'mock-phone.py'), system_address],
                                env=env, stdout=subprocess.PIPE, text=True)
        procs.append(mock)
        if mock.stdout.readline().strip() != 'ready':
            print('FAIL the mock phone did not start')
            return 1
        started = time.time()
        procs.append(subprocess.Popen([os.path.join(build, 'vibeos-dialer-calls')], env=env, stderr=open(log, 'w')))
        run(Test(session_address, env['XDG_DATA_HOME'], env['HOME'], env['XDG_CONFIG_HOME']), started, log, healthy)
    finally:
        for p in reversed(procs):
            p.terminate()
            try:
                p.wait(3)
            except subprocess.TimeoutExpired:
                p.kill()
        shutil.rmtree(tmp, ignore_errors=True)
    print('%d failed' % len(base.failures) if base.failures else 'all passed')
    return 1 if base.failures else 0


if __name__ == '__main__':
    sys.exit(main())
