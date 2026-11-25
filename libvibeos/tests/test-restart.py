#!/usr/bin/python3
# vibeos-restart.c: a program starting over in the session's new language, on a bus of the test's own with a mock
# systemd user manager on it (test-restart-child.c is the program).
#
#   fresh        started from a shell in another language than the manager's: it stays in its own until told
#   idle         a change: the same process again (execve) in the new language a moment later; the other variables
#                kept, the change's others (PATH) not taken; the arguments its prepare gives; its bus name its own
#                again; a descriptor left open without close-on-exec not reaching it; its name not "exe"
#   busy         a change while it is busy: it stays until it is idle, then starts over
#   told idle    the same, the program saying it became idle (vibeos_restart_check): at once, not at the next retry
#   undone       a change undone before it was asked: nothing
#   gap          the language changed again while it was starting over: it follows that one too, at its start
#   systemd      the main process of a Type=dbus service: systemd is asked to restart the service, and the program
#                does not start over by itself
#   not dbus     a service of another type: started over in place
#   prepare      told before it goes; told again when it stays after all: systemd took the restart and never did it
#                (it goes on, and asks again), or its executable is gone (it goes on, until the next change)
#
# usage: test-restart.py BUILD_DIR
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

from gi.repository import Gio, GLib

BUS_CONFIG = '''<busconfig><type>session</type><listen>unix:tmpdir=%s</listen><auth>EXTERNAL</auth>
<policy context="default"><allow send_destination="*" eavesdrop="true"/><allow eavesdrop="true"/>
<allow own="*"/></policy></busconfig>'''

SYSTEMD = '''<node>
<interface name="org.freedesktop.systemd1.Manager">
  <method name="GetUnitByPID"><arg type="u" direction="in"/><arg type="o" direction="out"/></method>
  <method name="RestartUnit"><arg type="s" direction="in"/><arg type="s" direction="in"/>
    <arg type="o" direction="out"/></method>
  <property name="Environment" type="as" access="read"/>
</interface>
<interface name="org.freedesktop.systemd1.Unit"><property name="Id" type="s" access="read"/></interface>
<interface name="org.freedesktop.systemd1.Service">
  <property name="Type" type="s" access="read"/><property name="MainPID" type="u" access="read"/>
</interface>
</node>'''
UNIT_PATH = '/org/freedesktop/systemd1/unit/test_2drestart_2eservice'
UNIT = 'test-restart.service'

VARIABLES = ('LANG', 'LANGUAGE', 'LC_ALL', 'LC_MESSAGES', 'LC_TIME', 'LC_NUMERIC', 'LC_MONETARY', 'LC_MEASUREMENT',
             'LC_PAPER')


def session(lang, **region):
    # what Settings sends (vibeos_locale_session_environment): every variable, the ones unset empty
    return ['%s=%s' % (v, lang if v == 'LANG' else region.get(v, '')) for v in VARIABLES]


RU, EN, DE = session('ru_RU.UTF-8'), session('en_US.UTF-8'), session('de_DE.UTF-8')
failures = []


def check(what, ok, detail=''):
    print('%s %s%s' % ('ok  ' if ok else 'FAIL', what, (': %s' % (detail,)) if detail and not ok else ''), flush=True)
    if not ok:
        failures.append(what)


class Systemd:
    """the user manager, as far as vibeos-restart.c asks it: its environment, the unit of a pid, and RestartUnit,
    which stops the service's process and starts its command again in the manager's environment"""

    def __init__(self, bus, start):
        self.bus = bus
        self.start = start
        self.environment = []
        self.service = None             # (Popen, type) of the one service
        self.restarts = []
        self.silent = False             # RestartUnit taken, and nothing done
        node = Gio.DBusNodeInfo.new_for_xml(SYSTEMD)
        self.ids = [bus.register_object('/org/freedesktop/systemd1', node.interfaces[0], self.call, self.get, None)]
        for iface in node.interfaces[1:]:
            self.ids.append(bus.register_object(UNIT_PATH, iface, self.call, self.get, None))
        bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus', 'RequestName',
                      GLib.Variant('(su)', ('org.freedesktop.systemd1', 4)), None, Gio.DBusCallFlags.NONE, -1, None)

    def call(self, conn, sender, path, iface, method, params, inv):
        if method == 'GetUnitByPID':
            if self.service and self.service[0].pid == params.unpack()[0]:
                return inv.return_value(GLib.Variant('(o)', (UNIT_PATH,)))
            return inv.return_dbus_error('org.freedesktop.systemd1.NoUnitForPID', 'no unit')
        if method == 'RestartUnit':
            self.restarts.append(list(params.unpack()))
            inv.return_value(GLib.Variant('(o)', ('/org/freedesktop/systemd1/job/1',)))
            if not self.silent:
                GLib.idle_add(self.restart)

    def restart(self):
        proc, kind = self.service
        proc.send_signal(signal.SIGTERM)
        proc.wait(5)
        env = dict((e.split('=', 1) for e in self.environment), **{
            k: v for k, v in proc.env.items() if k not in VARIABLES})
        self.service = (self.start(env), kind)

    def get(self, conn, sender, path, iface, prop):
        if prop == 'Environment':
            return GLib.Variant('as', self.environment)
        if prop == 'Id':
            return GLib.Variant('s', UNIT)
        if prop == 'Type':
            return GLib.Variant('s', self.service[1])
        if prop == 'MainPID':
            return GLib.Variant('u', self.service[0].pid)


class Test:
    def __init__(self, build, tmp, address):
        self.build = build
        self.tmp = tmp
        self.address = address
        self.log = os.path.join(tmp, 'log')
        self.busy = os.path.join(tmp, 'busy')
        self.bus = Gio.DBusConnection.new_for_address_sync(
            address, Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT |
            Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
        self.systemd = Systemd(self.bus, self.start)
        self.procs = []

    def start(self, env):
        env = dict(env)
        proc = subprocess.Popen([os.path.join(self.build, 'test-restart-child')], env=env)
        proc.env = env
        self.procs.append(proc)
        return proc

    def child(self, **more):
        # started from a shell: German, with British dates and a language list of its own. It hears a change once
        # it has its bus name (it subscribes before it asks for the name), which each case waits for
        env = {k: v for k, v in os.environ.items()
               if k not in VARIABLES and k not in ('INVOCATION_ID', 'VIBEOS_RESTARTED')}
        env.update(DBUS_SESSION_BUS_ADDRESS=self.address, RESTART_LOG=self.log, RESTART_BUSY=self.busy, KEEP='yes',
                   LANG='de_DE.UTF-8', LANGUAGE='de', LC_TIME='en_GB.UTF-8', VIBEOS_RESTART_SETTLE_MS='300',
                   VIBEOS_RESTART_RETRY_MS='400', RESTART_ARGS='--again')
        env.update(more)
        return self.start(env)

    def lines(self, event='start'):
        if not os.path.exists(self.log):
            return []
        with open(self.log) as f:
            return [line for line in map(json.loads, f) if line['event'] == event]

    def settle(self, seconds):
        ctx = GLib.MainContext.default()
        end = time.time() + seconds
        while time.time() < end:
            while ctx.pending():
                ctx.iteration(False)
            time.sleep(0.02)

    def wait(self, what, predicate, timeout=5.0):
        ctx = GLib.MainContext.default()
        end = time.time() + timeout
        while time.time() < end:
            while ctx.pending():
                ctx.iteration(False)
            value = predicate()
            if value:
                check(what, True)
                return value
            time.sleep(0.02)
        check(what, False, 'timed out; the log has %s' % self.lines('start'))
        return None

    def change(self, env, extra=()):
        self.bus.emit_signal(None, '/vibeos/Locale', 'vibeos.Locale', 'Changed', GLib.Variant('(as)', (env + list(extra),)))
        self.bus.flush_sync(None)

    def owner_pid(self):
        try:
            owner = self.bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
                                       'GetNameOwner', GLib.Variant('(s)', ('vibeos.Test.Restart',)), None,
                                       Gio.DBusCallFlags.NONE, 2000, None).unpack()[0]
            return self.bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
                                      'GetConnectionUnixProcessID', GLib.Variant('(s)', (owner,)), None,
                                      Gio.DBusCallFlags.NONE, 2000, None).unpack()[0]
        except GLib.Error:
            return None

    def stop(self):
        for p in self.procs:
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(3)
                except subprocess.TimeoutExpired:
                    p.kill()
        self.procs = []
        if os.path.exists(self.log):
            os.unlink(self.log)
        self.wait('its name is gone', lambda: self.owner_pid() is None)


def run(t):
    # --- fresh, then idle: Settings gives the manager the new language, then says so
    t.systemd.environment = RU
    proc = t.child()
    t.wait('started', lambda: t.lines())
    t.wait('it has its name', lambda: t.owner_pid() == proc.pid)
    t.settle(1.0)
    check('started from a shell, it stays in its language until told', len(t.lines()) == 1, t.lines())
    t.change(RU, ['PATH=/nowhere', 'KEEP=no'])
    t.settle(0.15)
    check('not at once: a moment after the change', len(t.lines()) == 1)
    after = t.wait('started over', lambda: t.lines()[1:] and t.lines()[-1])
    if after:
        check('the same process', after['pid'] == proc.pid and proc.poll() is None, after)
        check('in the new language, the language list and the dates\' region unset',
              after['LANG'] == 'ru_RU.UTF-8' and after['LANGUAGE'] is None and after['LC_TIME'] is None, after)
        check('the other variables kept, none taken from the change', after['KEEP'] == 'yes' and
              after['PATH'] == os.environ.get('PATH') and after['VIBEOS_RESTARTED'] == '1', after)
        check('with the arguments it gave', after['argv'][1:3] == ['--leak', after['argv'][2]] and
              after['argv'][3:] == ['--again'], after['argv'])
        check('the pipe it left open did not reach it', after['leaked'] is False, after)
        check('its name its own, not "exe"', after['comm'] == 'test-restart-ch', after['comm'])
    t.wait('its bus name its own again', lambda: len(t.lines('name')) == 2 and t.owner_pid() == proc.pid)
    t.settle(1.0)
    check('once: the manager\'s language is the one it is in now', len(t.lines()) == 2, t.lines())
    t.stop()

    # --- busy
    proc = t.child(LANG='ru_RU.UTF-8', LANGUAGE='', LC_TIME='')
    t.wait('started', lambda: t.owner_pid() == proc.pid)
    open(t.busy, 'w').close()
    t.systemd.environment = EN
    t.change(EN)
    t.settle(1.5)
    check('busy, it stays', len(t.lines()) == 1, t.lines())
    os.unlink(t.busy)
    after = t.wait('idle, it starts over', lambda: t.lines()[1:] and t.lines()[-1], timeout=3)
    check('in English', after and after['LANG'] == 'en_US.UTF-8' and after['pid'] == proc.pid, after)
    t.stop()

    # --- told idle: the next retry a minute away, the program says it is idle
    proc = t.child(LANG='ru_RU.UTF-8', LANGUAGE='', LC_TIME='', VIBEOS_RESTART_RETRY_MS='60000')
    t.wait('started', lambda: t.owner_pid() == proc.pid)
    open(t.busy, 'w').close()
    t.change(EN)
    t.settle(1.0)
    check('busy, it stays', len(t.lines()) == 1, t.lines())
    os.unlink(t.busy)
    proc.send_signal(signal.SIGUSR1)
    after = t.wait('told it is idle, it starts over at once', lambda: t.lines()[1:] and t.lines()[-1], timeout=1)
    check('in English', after and after['LANG'] == 'en_US.UTF-8' and after['pid'] == proc.pid, after)
    t.stop()

    # --- undone
    t.systemd.environment = DE
    proc = t.child(LANGUAGE='', LC_TIME='')
    t.wait('started', lambda: t.owner_pid() == proc.pid)
    t.change(EN)
    t.change(DE)
    t.settle(1.5)
    check('a change undone before it was asked: nothing', len(t.lines()) == 1, t.lines())
    t.stop()

    # --- gap: Russian asked for, and the manager already in English (Settings changed again meanwhile)
    t.systemd.environment = EN
    proc = t.child()
    t.wait('started', lambda: t.owner_pid() == proc.pid)
    t.change(RU)
    t.wait('Russian first', lambda: len(t.lines()) >= 2 and t.lines()[1]['LANG'] == 'ru_RU.UTF-8')
    last = t.wait('then the manager\'s English, looked at as it started', lambda: len(t.lines()) == 3 and
                  t.lines()[2])
    check('English, the same process', last and last['LANG'] == 'en_US.UTF-8' and last['pid'] == proc.pid, last)
    t.settle(1.0)
    check('and no more', len(t.lines()) == 3, t.lines())
    t.stop()

    # --- systemd, Type=dbus
    t.systemd.environment = DE
    proc = t.child(INVOCATION_ID='0123', LANGUAGE='', LC_TIME='')
    t.systemd.service = (proc, 'dbus')
    t.wait('started by "systemd"', lambda: t.owner_pid() == proc.pid)
    t.settle(0.5)
    check('started by systemd in the manager\'s language: it stays', len(t.lines()) == 1, t.lines())
    t.systemd.environment = RU
    t.change(RU)
    t.wait('systemd asked to restart the service', lambda: t.systemd.restarts == [[UNIT, 'replace']])
    after = t.wait('a new process, in Russian', lambda: t.lines()[1:] and t.lines()[-1])
    check('not started over by itself: systemd\'s', after and after['pid'] != proc.pid and
          after['VIBEOS_RESTARTED'] is None and after['LANG'] == 'ru_RU.UTF-8', after)
    t.settle(1.0)
    check('once', len(t.lines()) == 2 and len(t.systemd.restarts) == 1, (t.lines(), t.systemd.restarts))
    t.systemd.service = None
    t.stop()

    # --- systemd, another type: in place
    t.systemd.environment = DE
    proc = t.child(INVOCATION_ID='4567', LANGUAGE='', LC_TIME='')
    t.systemd.service = (proc, 'exec')
    t.systemd.restarts = []
    t.wait('started', lambda: t.owner_pid() == proc.pid)
    t.systemd.environment = RU
    t.change(RU)
    after = t.wait('started over in place', lambda: t.lines()[1:] and t.lines()[-1])
    check('the same process, systemd not asked', after and after['pid'] == proc.pid and not t.systemd.restarts and
          after['LANG'] == 'ru_RU.UTF-8', (after, t.systemd.restarts))
    t.systemd.service = None
    t.stop()

    # --- prepare: told before it goes (every case above went through it), and when it stays after all
    t.systemd.environment = DE
    proc = t.child(INVOCATION_ID='89ab', LANGUAGE='', LC_TIME='', VIBEOS_RESTART_UNIT_WAIT_MS='800')
    t.systemd.service = (proc, 'dbus')
    t.systemd.restarts = []
    t.systemd.silent = True
    t.wait('started', lambda: t.owner_pid() == proc.pid)
    t.systemd.environment = RU
    t.change(RU)
    t.wait('systemd asked', lambda: t.systemd.restarts)
    t.wait('systemd does nothing: it is told it stays', lambda: [p['going'] for p in t.lines('prepare')][:2] ==
           [True, False], timeout=3)
    t.wait('and asks again', lambda: len(t.systemd.restarts) >= 2, timeout=3)
    check('the same process all along, in its language', proc.poll() is None and len(t.lines()) == 1, t.lines())
    t.systemd.silent = False
    t.systemd.service = None
    t.stop()

    copy = os.path.join(t.tmp, 'gone-child')
    shutil.copy(os.path.join(t.build, 'test-restart-child'), copy)
    env = {k: v for k, v in os.environ.items() if k not in VARIABLES and k not in ('INVOCATION_ID', 'VIBEOS_RESTARTED')}
    env.update(DBUS_SESSION_BUS_ADDRESS=t.address, RESTART_LOG=t.log, RESTART_BUSY=t.busy, LANG='de_DE.UTF-8',
               VIBEOS_RESTART_SETTLE_MS='300')
    proc = subprocess.Popen([copy], env=env)
    t.procs.append(proc)
    t.wait('started', lambda: t.owner_pid() == proc.pid)
    os.unlink(copy)
    t.change(RU)
    t.wait('its executable gone: told before, and told it stays', lambda: [p['going'] for p in t.lines('prepare')] ==
           [True, False], timeout=3)
    t.settle(1.0)
    check('it goes on as it was', proc.poll() is None and len(t.lines()) == 1, t.lines())
    t.stop()


def main():
    build = sys.argv[1]
    tmp = tempfile.mkdtemp(prefix='test-restart-')
    daemon = None
    t = None
    try:
        config = os.path.join(tmp, 'bus.conf')
        with open(config, 'w') as f:
            f.write(BUS_CONFIG % tmp)
        daemon = subprocess.Popen(['dbus-daemon', '--config-file=' + config, '--print-address', '--nofork'],
                                  stdout=subprocess.PIPE, text=True)
        t = Test(build, tmp, daemon.stdout.readline().strip())
        run(t)
    finally:
        if t:
            t.stop()
        if daemon:
            daemon.terminate()
            daemon.wait(3)
        shutil.rmtree(tmp, ignore_errors=True)
    print('%d failed' % len(failures) if failures else 'all passed')
    return 1 if failures else 0


if __name__ == '__main__':
    if not shutil.which('dbus-daemon'):
        print('skipped: no dbus-daemon')
        sys.exit(77)
    sys.exit(main())
