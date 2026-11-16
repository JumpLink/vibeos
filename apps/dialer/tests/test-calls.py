#!/usr/bin/python3
# vibeos-dialer-calls against a mock phone (mock-phone.py): the call flows, end to end over
# D-Bus, with no modem and no real call. Two buses of its own, so nothing reaches the phone's real ModemManager,
# shell or notifications.
#
# usage: test-calls.py BUILD_DIR SOURCE_DIR
import json
import os
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time

from gi.repository import Gio, GLib

BUS_CONFIG = '''<busconfig><type>session</type><listen>unix:tmpdir=%s</listen><auth>EXTERNAL</auth>
<policy context="default"><allow send_destination="*" eavesdrop="true"/><allow eavesdrop="true"/>
<allow own="*"/></policy></busconfig>'''

NAME, PATH, IFACE = 'vibeos.Dialer.Calls', '/vibeos/Dialer/Calls', 'vibeos.Dialer.Calls'
# call-types.h
CONNECTING, DIALING, INCOMING, CALL_WAITING, ACTIVE, ONHOLD, DISCONNECTING, DISCONNECTED = range(8)
END_NONE, END_LOCAL, END_REMOTE, END_MISSED, END_REJECTED, END_CANCELED, END_BUSY, END_ERROR = range(8)
T_INCOMING, T_OUTGOING, T_MISSED, T_REJECTED, T_BLOCKED = 1, 2, 3, 5, 6
R_TERMINATED = 4                        # MMCallStateReason

failures = []


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


def block(data_home, number):
    # what the Phone's or the Messaging's Block does: a row in the shared list (blocked-numbers.c), which the
    # service hears of through its file monitor
    path = os.path.join(data_home, 'vibeos-phone', 'blocked_numbers.db')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    db = sqlite3.connect(path)
    db.execute('CREATE TABLE IF NOT EXISTS blocked (_id INTEGER PRIMARY KEY AUTOINCREMENT,'
               ' original_number TEXT NOT NULL UNIQUE, e164_number TEXT)')
    db.execute('INSERT OR IGNORE INTO blocked (original_number) VALUES (?)', (number,))
    db.commit()
    db.close()


class Test:
    def __init__(self, session_address, data_home):
        self.data_home = data_home
        self.bus = Gio.DBusConnection.new_for_address_sync(
            session_address, Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT |
            Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)

    def wait(self, what, predicate, timeout=5.0):
        ctx = GLib.MainContext.default()
        end = time.time() + timeout
        last = None
        while time.time() < end:
            while ctx.pending():
                ctx.iteration(False)
            try:
                value = predicate()
                if value:
                    check(what, True)
                    return value
            except (GLib.Error, LookupError, TypeError, StopIteration) as e:
                last = e
            time.sleep(0.02)
        check(what, False, 'timed out (%r); the phone has %s' % (last, json.dumps(self.report())[:1500]))
        return None

    def settle(self, seconds=0.3):
        ctx = GLib.MainContext.default()
        end = time.time() + seconds
        while time.time() < end:
            while ctx.pending():
                ctx.iteration(False)
            time.sleep(0.02)

    def service(self, method, args=None, signature=None):
        reply = self.bus.call_sync(NAME, PATH, IFACE, method, GLib.Variant(signature, args) if signature else None,
                                   None, Gio.DBusCallFlags.NONE, 5000, None)
        return reply.unpack() if reply else None

    def prop(self, name):
        reply = self.bus.call_sync(NAME, PATH, 'org.freedesktop.DBus.Properties', 'Get',
                                   GLib.Variant('(ss)', (IFACE, name)), None, Gio.DBusCallFlags.NONE, 5000, None)
        return reply.unpack()[0]

    def calls(self):
        return self.prop('Calls')

    def call(self, call_id):
        return next(c for c in self.calls() if c['id'] == call_id)

    def phone(self, method, args=None, signature=None):
        reply = self.bus.call_sync('vibeos.Test.Phone', '/vibeos/Test/Phone', 'vibeos.Test.Phone', method,
                                   GLib.Variant(signature, args) if signature else None, None,
                                   Gio.DBusCallFlags.NONE, 5000, None)
        return reply.unpack() if reply else None

    def report(self):
        return json.loads(self.phone('Report')[0])

    def logged(self, method):
        return [entry for entry in self.report()['log'] if entry[0] == method]

    def notification(self, title=None, body=None):
        for nid, n in self.report()['notifications'].items():
            if (title is None or n['title'] == title) and (body is None or n['body'] == body):
                return int(nid), n
        return None

    def call_log(self):
        return self.service('GetCallLog')[0]


def run(t):
    # --- at start: the modem is found, and hold is probed with no call up
    t.wait('the service is ready', lambda: t.prop('Status') == 'ready')
    t.wait('the hold probe ran', lambda: t.logged('HoldAndAccept'))
    t.wait('hold is not claimed (the modem said not supported)', lambda: t.prop('Features') == ['dtmf'])
    check('the country is the network\'s (262 = DE)', t.prop('Country') == 'DE', t.prop('Country'))

    # --- an outgoing call, answered by the other side, hung up by us
    t.phone('Clear')
    call_id = t.service('Dial', ('030 1234-5',), '(s)')[0]
    t.wait('dialling: CreateCall with the digits only', lambda: ['CreateCall', '03012345'] in t.logged('CreateCall'))
    t.wait('the call is started', lambda: t.logged('Start'))
    t.wait('"Calling…" (DIALING)', lambda: t.call(call_id)['state'] == DIALING)
    t.wait('the call audio goes into call mode', lambda: ['SelectMode', 1] in t.logged('SelectMode'))
    t.wait('a "Dialing" notification that does not peek',
           lambda: (t.notification(body='Dialing') or (0, {}))[1].get('no-peek'))
    path = next(p for p, c in t.report()['calls'].items() if c[2] == '03012345')
    t.phone('RemoteAnswer', (path,), '(o)')
    t.wait('answered: ACTIVE, with its connect time', lambda: t.call(call_id)['state'] == ACTIVE and
           t.call(call_id)['connected'] > 0)
    t.wait('"Ongoing call" with a chronometer', lambda: (t.notification(body='Ongoing call') or (0, {}))[1]
           .get('chronometer') == t.call(call_id)['connected'])
    t.service('SetSpeaker', (True,), '(b)')
    t.wait('the speaker goes on', lambda: ['EnableSpeaker', True] in t.logged('EnableSpeaker') and t.prop('Speaker'))
    t.wait('the notification\'s action turns it off now',
           lambda: 'speaker-off' in t.notification(body='Ongoing call')[1]['actions'])
    t.service('SendDtmf', (call_id, '1'), '(us)')
    t.wait('a modem that cannot send tones loses the keypad', lambda: t.prop('Features') == [])
    t.service('Hangup', (call_id,), '(u)')
    t.wait('hung up: Hangup on the call', lambda: t.logged('Hangup'))
    t.wait('ended here: "Call ended", LOCAL', lambda: t.call(call_id)['end'] == END_LOCAL)
    t.wait('it goes after 200 ms', lambda: not t.calls())
    t.wait('the call audio leaves call mode', lambda: ['SelectMode', 0] in t.logged('SelectMode'))
    t.wait('ModemManager is told to delete it', lambda: t.logged('DeleteCall'))
    t.wait('no notification is left', lambda: not t.report()['notifications'])
    t.wait('outgoing, in the call log', lambda: t.call_log()[0]['type'] == T_OUTGOING and
           t.call_log()[0]['number'] == '03012345')
    check('speaker off again for the next call', not t.prop('Speaker'))

    # --- an incoming call on a locked phone: the answer screen, then answered, then hung up by them
    t.phone('Clear')
    path = t.phone('Incoming', ('+491701234567',), '(s)')[0]
    t.wait('the answer screen is asked for over the lock screen',
           lambda: ['LaunchFullScreen', 'vibeos.Dialer.InCall'] in t.logged('LaunchFullScreen'))
    # the first ring starts GStreamer, which in a package build's empty HOME scans every plugin first: seconds under
    # emulation, with the service answering nothing meanwhile
    call_id = t.wait('INCOMING', lambda: next((c['id'] for c in t.calls() if c['state'] == INCOMING), 0), timeout=30)
    t.wait('its notification does not peek over the answer screen',
           lambda: (t.notification(body='Incoming call') or (0, {}))[1].get('no-peek'))
    check('the call audio stays out while it rings', not t.logged('SelectMode'))
    t.service('Answer', (call_id,), '(u)')
    t.wait('Accept on the call', lambda: t.logged('Accept'))
    t.wait('ACTIVE', lambda: t.call(call_id)['state'] == ACTIVE)
    t.phone('RemoteHangup', (path, R_TERMINATED), '(oi)')
    t.wait('they hung up: REMOTE', lambda: t.call(call_id)['end'] == END_REMOTE)
    t.wait('incoming, in the call log', lambda: t.call_log()[0]['type'] == T_INCOMING)

    # --- in use: a heads-up with Decline and Answer; declined from it
    t.wait('the last call has gone', lambda: not t.calls())
    t.phone('SetInUse', (True,), '(b)')
    t.phone('Incoming', ('+491701234567',), '(s)')
    found = t.wait('a heads-up that peeks and stays (critical), Decline and Answer',
                   lambda: (lambda n: n if n and n[1]['urgency'] == 2 and n[1]['actions'][1:] == ['decline', 'answer']
                            else None)(t.notification(body='Incoming call')))
    if found:
        t.phone('InvokeAction', (found[0], 'decline'), '(us)')
    t.wait('declined: Hangup on the call', lambda: t.logged('Hangup'))
    t.wait('declined, in the call log (not missed)', lambda: t.call_log()[0]['type'] == T_REJECTED)
    check('no missed-call notification for a declined call', not t.notification(title='Missed call'))

    # --- missed: it rang out; the notification, the count; swiped away it is seen
    t.wait('the last call has gone', lambda: not t.calls())
    path = t.phone('Incoming', ('+491701234567',), '(s)')[0]
    t.wait('INCOMING', lambda: any(c['state'] == INCOMING for c in t.calls()))
    t.phone('RemoteHangup', (path, R_TERMINATED), '(oi)')
    t.wait('missed, in the call log, not seen', lambda: t.call_log()[0]['type'] == T_MISSED and
           not t.call_log()[0]['is_read'])
    t.wait('the missed count is 1', lambda: t.prop('MissedCount') == 1)
    missed = t.wait('a "Missed call" notification with Call back', lambda: (lambda n: n if n and 'call-back' in
                                                                             n[1]['actions'] else None)(
        t.notification(title='Missed call')))
    if missed:
        t.phone('Dismiss', (missed[0],), '(u)')
    t.wait('swiped away, it is seen', lambda: t.prop('MissedCount') == 0 and t.call_log()[0]['is_read'])

    # --- a second call on a modem that cannot hold: answering it ends the call that is up, then accepts it once it
    # rings on its own (a call that cannot be held ends)
    t.wait('the last call has gone', lambda: not t.calls())
    t.phone('SetInUse', (False,), '(b)')
    t.phone('Incoming', ('+491701234567',), '(s)')
    first = t.wait('INCOMING', lambda: next((c['id'] for c in t.calls() if c['state'] == INCOMING), 0))
    t.service('Answer', (first,), '(u)')
    t.wait('the first call is up', lambda: t.call(first)['state'] == ACTIVE)
    t.phone('Clear')
    t.phone('Incoming', ('+493023575124',), '(s)')
    second = t.wait('the second one waits: CALL_WAITING',
                    lambda: next((c['id'] for c in t.calls() if c['state'] == CALL_WAITING), 0))
    t.service('Answer', (second,), '(u)')
    t.wait('the call that was up is hung up', lambda: t.logged('Hangup'))
    t.wait('the second is accepted once it rings alone, and is up',
           lambda: t.logged('Accept') and t.call(second)['state'] == ACTIVE)
    check('no +CHLD was tried', not t.logged('HoldAndAccept') and not t.logged('HangupAndAccept'))
    t.service('Hangup', (second,), '(u)')

    # --- post-dial: "0301234,5;67" dials 0301234 alone; once the call is up, a 3 s pause, the 5, then a wait with
    # the rest, which goes out on Yes (the dialpad's "Add 2-sec pause" and "Add wait")
    t.wait('the last call has gone', lambda: not t.calls())
    t.phone('SetDtmf', (True,), '(b)')
    t.phone('Clear')
    call_id = t.service('Dial', ('0301234,5;67',), '(s)')[0]
    t.wait('the number alone goes to the modem', lambda: ['CreateCall', '0301234'] in t.logged('CreateCall'))
    path = t.wait('the call is there', lambda: next((p for p, c in t.report()['calls'].items()
                                                     if c[2] == '0301234'), None))
    t.wait('"Calling…" (DIALING)', lambda: t.call(call_id)['state'] == DIALING)
    check('no tones before the call is up', not t.logged('SendDtmf'))
    t.phone('RemoteAnswer', (path,), '(o)')
    t.wait('answered: ACTIVE', lambda: t.call(call_id)['state'] == ACTIVE)
    t.settle(1.5)
    check('no tones during the pause', not t.logged('SendDtmf'), t.logged('SendDtmf'))
    t.wait('after the pause, the 5', lambda: ['SendDtmf', path, '5'] in t.logged('SendDtmf'))
    t.wait('then it waits, with the rest to ask about', lambda: t.call(call_id)['post-dial-wait'] == '67')
    t.service('ContinuePostDial', (call_id, True), '(ub)')
    t.wait('Yes: the rest goes out', lambda: ['SendDtmf', path, '67'] in t.logged('SendDtmf'))
    check('and it no longer waits', t.call(call_id)['post-dial-wait'] == '')
    t.service('Hangup', (call_id,), '(u)')
    t.wait('logged as it was dialled', lambda: t.call_log()[0]['number'] == '0301234,5;67')

    # --- a wait answered No: nothing more is sent
    t.wait('the last call has gone', lambda: not t.calls())
    t.phone('Clear')
    call_id = t.service('Dial', ('0301234;89',), '(s)')[0]
    path = t.wait('the call is there', lambda: next((p for p, c in t.report()['calls'].items()
                                                     if c[2] == '0301234'), None))
    t.wait('dialling', lambda: t.call(call_id)['state'] == DIALING)
    t.phone('RemoteAnswer', (path,), '(o)')
    t.wait('it waits at once', lambda: t.call(call_id)['post-dial-wait'] == '89')
    t.service('ContinuePostDial', (call_id, False), '(ub)')
    t.wait('No: it no longer waits', lambda: t.call(call_id)['post-dial-wait'] == '')
    t.settle(0.5)
    check('and nothing was sent', not t.logged('SendDtmf'), t.logged('SendDtmf'))
    t.service('Hangup', (call_id,), '(u)')

    # --- the call log's own changes, once the last call is in it (a call is logged before it goes from Calls)
    t.wait('the last call has gone', lambda: not t.calls())
    ids = [e['_id'] for e in t.call_log()]
    t.service('DeleteCallLog', ([ids[0]],), '(ax)')
    t.wait('one row deleted', lambda: len(t.call_log()) == len(ids) - 1)
    t.service('ClearCallLog')
    t.wait('the log cleared', lambda: not t.call_log())

    # --- the modem going away entirely and coming back (a ModemManager restart, a modem reset): the service lives on
    # (a handler with the wrong arguments for object-added crashed it here once)
    t.phone('SetModem', (False,), '(b)')
    t.wait('the modem gone: no-modem', lambda: t.prop('Status') == 'no-modem')
    t.phone('SetModem', (True,), '(b)')
    t.wait('the modem back: ready again', lambda: t.prop('Status') == 'ready')
    t.phone('Incoming', ('+493023575124',), '(s)')
    t.wait('and a call comes in again', lambda: any(c['state'] == INCOMING for c in t.calls()))
    t.service('Decline', (next(c['id'] for c in t.calls() if c['state'] == INCOMING),), '(u)')
    t.wait('the last call has gone', lambda: not t.calls())

    # --- a call from a blocked number: declined before it rings, never shown, logged as "Blocked call"
    block(t.data_home, '030 99887766')
    t.settle(0.5)
    t.phone('Clear')
    t.phone('Incoming', ('+493099887766',), '(s)')
    t.wait('a blocked caller is hung up on', lambda: t.logged('Hangup'))
    check('without ringing or its answer screen', not t.logged('LaunchFullScreen'), t.report()['log'])
    check('not among the calls', not t.calls(), t.calls())
    t.wait('and deleted off the modem', lambda: t.logged('DeleteCall'))
    t.wait('logged as a blocked call, seen', lambda: t.call_log()[0]['type'] == T_BLOCKED
           and t.call_log()[0]['is_read'])
    check('no notification', not t.report()['notifications'], t.report()['notifications'])


def main():
    build, source = sys.argv[1], sys.argv[2]
    tmp = tempfile.mkdtemp(prefix='test-calls-')
    procs = []
    try:
        subprocess.run(['glib-compile-schemas', '--targetdir', tmp, os.path.join(source, 'data')], check=True)
        system, system_address = start_bus(tmp, 'system')
        session, session_address = start_bus(tmp, 'session')
        procs += [system, session]
        env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS=session_address, VIBEOS_DIALER_SYSTEM_BUS=system_address,
                   XDG_DATA_HOME=os.path.join(tmp, 'data'), XDG_STATE_HOME=os.path.join(tmp, 'state'),
                   XDG_CONFIG_HOME=os.path.join(tmp, 'config'), GSETTINGS_BACKEND='memory',
                   GSETTINGS_SCHEMA_DIR=tmp, PULSE_SERVER='unix:' + os.path.join(tmp, 'no-sound'))
        mock = subprocess.Popen([sys.executable, os.path.join(source, 'tests', 'mock-phone.py'), system_address],
                                env=env, stdout=subprocess.PIPE, text=True)
        procs.append(mock)
        if mock.stdout.readline().strip() != 'ready':
            print('FAIL the mock phone did not start')
            return 1
        procs.append(subprocess.Popen([os.path.join(build, 'vibeos-dialer-calls')], env=env))
        run(Test(session_address, env['XDG_DATA_HOME']))
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
