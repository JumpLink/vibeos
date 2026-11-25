#!/usr/bin/python3
# vibeos-clock-alarms with a GStreamer whose start never finishes (the plugin scanner, which opens every device, sleeps
# for good: a camera driver that never answers does that): the service keeps answering, and an alarm that rings still
# asks the shell for the ringing screen at once. A mock shell on a bus of its own takes that call.
#
# usage: test-service-gst-hang.py BUILD_DIR SOURCE_DIR [healthy | stuck-sound]
#
# With "healthy" GStreamer starts as it should, from a registry made first (the same steps without the stuck scanner, for
# the times to compare): the klaxon then plays, which the service's log says.
# With "stuck-sound" GStreamer is up and the sound server takes the connection and never answers: opening the sink
# waits for good, and the service must go on answering.
import datetime
import os
import shutil
import stat
import socket
import subprocess
import sys
import threading
import tempfile
import time

from gi.repository import Gio, GLib

BUS_CONFIG = '''<busconfig><type>session</type><listen>unix:tmpdir=%s</listen><auth>EXTERNAL</auth>
<policy context="default"><allow send_destination="*" eavesdrop="true"/><allow eavesdrop="true"/>
<allow own="*"/></policy></busconfig>'''

NAME, PATH, IFACE = 'vibeos.Clock.Alarms', '/vibeos/Clock/Alarms', 'vibeos.Clock.Alarms'
# how soon the service must answer: GStreamer's own wait for a sound is 10 s, so anything under it is not that wait
PROMPT = 5.0

failures = []
launched = []
held = []                        # a sound server's connections, never answered


def check(what, ok, detail=''):
    print('%s %s%s' % ('ok  ' if ok else 'FAIL', what, (': %s' % detail) if detail and not ok else ''), flush=True)
    if not ok:
        failures.append(what)


def start_bus(tmp, name):
    config = os.path.join(tmp, name + '.conf')
    with open(config, 'w') as f:
        f.write(BUS_CONFIG % tmp)
    bus = subprocess.Popen(['dbus-daemon', '--config-file=' + config, '--print-address', '--nofork'],
                           stdout=subprocess.PIPE, text=True)
    return bus, bus.stdout.readline().strip()


def pump(seconds):
    ctx = GLib.MainContext.default()
    end = time.time() + seconds
    while time.time() < end:
        while ctx.pending():
            ctx.iteration(False)
        time.sleep(0.02)


def wait(what, predicate, timeout):
    end = time.time() + timeout
    while time.time() < end:
        pump(0.05)
        try:
            if predicate():
                check(what, True)
                return True
        except GLib.Error:
            pass
    check(what, False, 'timed out')
    return False


SHELL = '''<node><interface name='vibeos.Shell'><method name='LaunchFullScreen'>
<arg name='app' type='s' direction='in'/><arg type='b' direction='out'/></method></interface></node>'''


def on_shell_call(bus, sender, path, iface, method, parameters, invocation):
    launched.append((parameters.unpack()[0], time.time()))
    invocation.return_value(GLib.Variant('(b)', (True,)))


def call(bus, method, args=None, signature=None):
    return bus.call_sync(NAME, PATH, IFACE, method, GLib.Variant(signature, args) if signature else None, None,
                         Gio.DBusCallFlags.NONE, 5000, None)


def run(session_address, started, log, healthy, stuck_sound):
    bus = Gio.DBusConnection.new_for_address_sync(
        session_address, Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
        None, None)
    node = Gio.DBusNodeInfo.new_for_xml(SHELL)
    bus.register_object('/vibeos/Shell', node.interfaces[0], on_shell_call, None, None)
    Gio.bus_own_name_on_connection(bus, 'vibeos.Shell', Gio.BusNameOwnerFlags.NONE, None, None)

    wait('the service answers', lambda: call(bus, 'GetAlarms'), 120)
    print('info the service answered %.1f s after its start' % (time.time() - started), flush=True)

    # an alarm for the next minute but one that has not begun
    now = datetime.datetime.now()
    at = now + datetime.timedelta(minutes=1 if now.second < 40 else 2)
    call(bus, 'AddAlarm', ({'hour': GLib.Variant('i', at.hour), 'minute': GLib.Variant('i', at.minute),
                            'days': GLib.Variant('u', 0), 'label': GLib.Variant('s', 'test')},), '(a{sv})')
    due = at.replace(second=0, microsecond=0).timestamp()
    wait('the alarm rings: the ringing screen is asked for', lambda: launched, due - time.time() + 30)
    if launched:
        late = launched[0][1] - due
        print('info the ringing screen was asked for %.1f s after the alarm was due' % late, flush=True)
        check('the ringing screen is vibeos.Clock.Ringing', launched[0][0] == 'vibeos.Clock.Ringing', launched[0][0])
        check('at once', late < PROMPT, '%.1f s' % late)
    began = time.time()
    check('the service answers while it rings', call(bus, 'GetAlarms') is not None and time.time() - began < PROMPT)
    ringing = bus.call_sync(NAME, PATH, 'org.freedesktop.DBus.Properties', 'Get', GLib.Variant('(ss)', (IFACE, 'Ringing')),
                            None, Gio.DBusCallFlags.NONE, 5000, None).unpack()[0]
    check('the alarm is the one ringing', bool(ringing), ringing)
    # past GStreamer's wait: the sound is given up for this ring, and the service goes on
    pump(11)
    check('the service still answers', call(bus, 'GetAlarms') is not None)
    with open(log) as f:
        text = f.read()
    if stuck_sound:
        check('GStreamer came up', 'gstreamer: up' in text, text[-600:])
    elif healthy:
        check('GStreamer came up, and the klaxon went on to play (no sound server is there to take it)',
              'gstreamer: up' in text and 'klaxon: cannot play' in text, text[-600:])
    else:
        check('the ring gave up the sound after its wait', 'not up after' in text and 'gstreamer: up' not in text,
              text[-600:])
    if ringing:
        call(bus, 'Dismiss', (ringing['instance'],), '(u)')
    pump(0.5)
    check('dismissed', not bus.call_sync(NAME, PATH, 'org.freedesktop.DBus.Properties', 'Get',
                                         GLib.Variant('(ss)', (IFACE, 'Ringing')), None, Gio.DBusCallFlags.NONE, 5000,
                                         None).unpack()[0])


# what the klaxon plays through: playbin and the pulse sink
GST_PLUGINS = ('coreelements', 'playback', 'typefindfunctions', 'audioconvert', 'audioresample', 'volume', 'ogg',
               'vorbis', 'autodetect', 'pulseaudio')


def gst_env(tmp):
    """GStreamer with only GST_PLUGINS, in a registry of its own and made first: a scan of every plugin the build
    host has takes minutes under emulation"""
    system = subprocess.run(['pkg-config', '--variable=pluginsdir', 'gstreamer-1.0'], capture_output=True, text=True,
                            check=True).stdout.strip()
    plugins = os.path.join(tmp, 'gst-plugins')
    os.makedirs(plugins)
    for name in GST_PLUGINS:
        path = os.path.join(system, 'libgst%s.so' % name)
        if os.path.exists(path):
            os.symlink(path, os.path.join(plugins, os.path.basename(path)))
    env = dict(os.environ, GST_PLUGIN_SYSTEM_PATH_1_0=plugins, GST_PLUGIN_PATH_1_0='',
               GST_REGISTRY_1_0=os.path.join(tmp, 'gst-registry.bin'))
    code = "import gi; gi.require_version('Gst', '1.0'); from gi.repository import Gst; Gst.init(None)"
    subprocess.run([sys.executable, '-c', code], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return {k: env[k] for k in ('GST_PLUGIN_SYSTEM_PATH_1_0', 'GST_PLUGIN_PATH_1_0', 'GST_REGISTRY_1_0')}


def main():
    build, source = sys.argv[1], sys.argv[2]
    healthy = sys.argv[3:] in (['healthy'], ['stuck-sound'])
    stuck_sound = sys.argv[3:] == ['stuck-sound']
    tmp = tempfile.mkdtemp(prefix='test-clock-hang-')
    procs = []
    try:
        subprocess.run(['glib-compile-schemas', '--targetdir', tmp, os.path.join(source, 'data')], check=True)
        scanner = os.path.join(tmp, 'stuck-scanner')
        with open(scanner, 'w') as f:
            f.write('#!/bin/sh\nexec sleep 100000\n')
        os.chmod(scanner, os.stat(scanner).st_mode | stat.S_IXUSR)
        # the default alarm sound where the service looks for it, in its data directories
        for sub, name in (('sounds/android/alarms', 'Hassium.ogg'), ('vibeos-clock/sounds', 'alarm_expire.ogg')):
            os.makedirs(os.path.join(tmp, 'share', sub))
            shutil.copy(os.path.join(source, 'data', 'sounds', 'alarm_expire.ogg'), os.path.join(tmp, 'share', sub, name))
        system, system_address = start_bus(tmp, 'system')
        session, session_address = start_bus(tmp, 'session')
        procs += [system, session]
        env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS=session_address, DBUS_SYSTEM_BUS_ADDRESS=system_address,
                   XDG_DATA_HOME=os.path.join(tmp, 'data'), XDG_STATE_HOME=os.path.join(tmp, 'state'),
                   XDG_CONFIG_HOME=os.path.join(tmp, 'config'), XDG_CACHE_HOME=os.path.join(tmp, 'cache'),
                   HOME=os.path.join(tmp, 'home'), GSETTINGS_BACKEND='memory', GSETTINGS_SCHEMA_DIR=tmp,
                   XDG_DATA_DIRS=os.path.join(tmp, 'share') + ':' + os.environ.get('XDG_DATA_DIRS',
                                                                                  '/usr/local/share:/usr/share'),
                   PULSE_SERVER='unix:' + os.path.join(tmp, 'no-sound'),
                   GST_REGISTRY_1_0=os.path.join(tmp, 'no-registry.bin'), GST_REGISTRY=os.path.join(tmp, 'no-registry.bin'))
        if healthy:
            del env['GST_REGISTRY']
            env.update(gst_env(tmp))
        if stuck_sound:
            server = socket.socket(socket.AF_UNIX)
            server.bind(os.path.join(tmp, 'no-sound'))
            server.listen(16)
            held.append(server)
            threading.Thread(target=lambda: [held.append(server.accept()[0]) for _ in iter(int, 1)],
                             daemon=True).start()
        else:
            env.update(GST_PLUGIN_SCANNER_1_0=scanner, GST_PLUGIN_SCANNER=scanner)
        log = os.path.join(tmp, 'service.log')
        started = time.time()
        procs.append(subprocess.Popen([os.path.join(build, 'vibeos-clock-alarms')], env=env, stderr=open(log, 'w')))
        run(session_address, started, log, healthy, stuck_sound)
    finally:
        for p in reversed(procs):
            p.terminate()
            try:
                p.wait(3)
            except subprocess.TimeoutExpired:
                p.kill()
        shutil.rmtree(tmp, ignore_errors=True)
    print('%d failed' % len(failures) if failures else 'all passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
