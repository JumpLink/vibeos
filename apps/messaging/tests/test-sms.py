#!/usr/bin/python3
# neo-messaging-sms against a mock modem (mock-modem.py): Messaging's receive and send flows (spec §10.2) and its
# notifications (§5), end to end over D-Bus, with no modem and no real message. Two buses of its own, so nothing
# reaches the phone's real ModemManager or notifications; the store is read as the app reads it.
#
# usage: test-sms.py BUILD_DIR SOURCE_DIR
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

NAME, PATH, IFACE = 'de.yesman.Messaging.Sms', '/de/yesman/Messaging/Sms', 'de.yesman.Messaging.Sms'
# sms-store.h
COMPLETE, YET_TO_SEND, SENDING, RESENDING, AWAITING_RETRY, FAILED, INCOMING = 1, 4, 5, 6, 7, 8, 100
UNKNOWN_ERROR = 'org.freedesktop.ModemManager1.Error.MessageError.Unknown'
NO_NETWORK = 'org.freedesktop.ModemManager1.Error.MobileEquipment.NoNetwork'
ANNA, BOB = '+491701234567', '+491709999999'

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
    # what the Phone's or the Messaging's Block does: a row in the shared list (dialer/src/blocked-numbers.c), which
    # the service hears of through its file monitor
    path = os.path.join(data_home, 'neo-phone', 'blocked_numbers.db')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    db = sqlite3.connect(path)
    db.execute('CREATE TABLE IF NOT EXISTS blocked (_id INTEGER PRIMARY KEY AUTOINCREMENT,'
               ' original_number TEXT NOT NULL UNIQUE, e164_number TEXT)')
    db.execute('INSERT OR IGNORE INTO blocked (original_number) VALUES (?)', (number,))
    db.commit()
    db.close()


class Test:
    def __init__(self, session_address, store):
        self.bus = Gio.DBusConnection.new_for_address_sync(
            session_address, Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT |
            Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
        self.store = store
        self.changed = []
        self.bus.signal_subscribe(None, IFACE, 'Changed', PATH, None, Gio.DBusSignalFlags.NONE,
                                  lambda *a: self.changed.append(a[5].unpack()[0]))

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
            except (GLib.Error, LookupError, TypeError, StopIteration, sqlite3.Error) as e:
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

    def modem(self, method, args=None, signature=None):
        reply = self.bus.call_sync('de.yesman.Test.Modem', '/de/yesman/Test/Modem', 'de.yesman.Test.Modem', method,
                                   GLib.Variant(signature, args) if signature else None, None,
                                   Gio.DBusCallFlags.NONE, 5000, None)
        return reply.unpack() if reply else None

    def report(self):
        return json.loads(self.modem('Report')[0])

    def logged(self, method):
        return [entry for entry in self.report()['log'] if entry[0] == method]

    def notification(self, title=None):
        for nid, n in self.report()['notifications'].items():
            if title is None or n['title'] == title:
                return int(nid), n
        return None

    def incoming(self, number, *parts, timestamp='2026-11-08T01:30:00+01'):
        return self.modem('Incoming', (number, list(parts), timestamp), '(sass)')[0]

    # the store, read as the app reads it
    def query(self, sql, *args):
        db = sqlite3.connect('file:%s?mode=ro' % self.store, uri=True)
        try:
            return db.execute(sql, args).fetchall()
        finally:
            db.close()

    def messages(self, conversation):
        return self.query('SELECT m._id, m.message_status, m.read, m.seen, p.text, m.sent_timestamp FROM messages m'
                          ' JOIN parts p ON p.message_id = m._id WHERE m.conversation_id = ? ORDER BY m._id',
                          conversation)

    def status(self, message):
        rows = self.query('SELECT message_status FROM messages WHERE _id = ?', message)
        return rows[0][0] if rows else None

    def conversation_of(self, number):
        rows = self.query('SELECT cp.conversation_id FROM conversation_participants cp JOIN participants p'
                          ' ON p._id = cp.participant_id WHERE p.normalized_destination = ?', number)
        return rows[0][0] if rows else None


def run(t):
    # --- coming
    t.wait('the modem is there', lambda: t.prop('Available'))
    t.wait('its country is the network\'s', lambda: t.prop('Country') == 'DE')
    # what came while the service was not running is taken at start, and off the modem
    bob = t.wait('a message waiting on the modem is kept', lambda: t.conversation_of(BOB))
    t.wait('and deleted off the modem', lambda: not t.report()['messages'])
    rows = t.messages(bob)
    check('with the network\'s timestamp as when it was sent',
          rows and rows[0][5] == 1794096000000, rows)
    n = t.wait('and notified, by its sender', lambda: t.notification('0170 9999999'))
    if n:
        check('as the Messaging\'s', n[1]['app'] == 'Messaging' and n[1]['desktop-entry'] == 'de.yesman.Messaging',
              n)

    # a long message in parts: one message once all are in
    t.modem('Clear')
    t.changed.clear()
    t.incoming(ANNA, 'Are you coming ', 'tonight?')
    anna = t.wait('a message in two parts is kept once', lambda: t.conversation_of(ANNA))
    t.wait('whole', lambda: [r[4] for r in t.messages(anna)] == ['Are you coming tonight?'])
    t.wait('Changed names its conversation', lambda: anna in t.changed)
    check('it is unread and not seen', t.messages(anna)[0][2:4] == (0, 0), t.messages(anna))
    # two conversations: one notification for both, newest first
    n = t.wait('two conversations: "2 new messages"', lambda: t.notification('2 new messages'))
    if n:
        check('a line for each, newest first', n[1]['body'] == '0170 1234567  Are you coming tonight?\n'
              '0170 9999999  While you were away', n[1]['body'])
        check('with a heads-up', not n[1]['no-peek'], n)

    # swiped away: seen, and the next message is notified alone
    t.modem('Dismiss', (n[0] if n else 1,), '(u)')
    t.wait('dismissed: its messages are seen', lambda: t.messages(anna)[0][3] == 1)
    t.incoming(ANNA, 'Hello?')
    n = t.wait('the next message is notified alone, by its sender', lambda: t.notification('0170 1234567'))
    if n:
        check('its text', n[1]['body'] == 'Hello?', n)
    t.incoming(ANNA, 'Call me')
    t.wait('one conversation\'s messages, oldest first',
           lambda: t.notification('0170 1234567')[1]['body'] == 'Hello?\nCall me')

    # the conversation open in the app: read at once, nothing notified
    t.service('SetVisible', (anna,), '(x)')
    t.wait('opening the conversation reads it', lambda: all(r[2] == 1 for r in t.messages(anna)))
    t.wait('and takes its notification away', lambda: t.notification() is None)
    t.incoming(ANNA, 'I see you are here')
    t.wait('a message into the open conversation is read', lambda: len(t.messages(anna)) == 4
           and t.messages(anna)[-1][2] == 1)
    t.settle()
    check('and not notified', t.notification() is None, t.report()['notifications'])
    # the list at its top: what comes is seen, not notified
    t.service('SetVisible', (0,), '(x)')
    t.incoming(BOB, 'Seen from the list')
    t.wait('with the list at its top, a message is seen', lambda: t.messages(bob)[-1][3] == 1)
    check('but unread', t.messages(bob)[-1][2] == 0, t.messages(bob))
    t.settle()
    check('and not notified', t.notification() is None, t.report()['notifications'])
    t.service('SetVisible', (-1,), '(x)')

    # an MMS notification (a binary message, no text) is mmsd-tng's: not an empty message here, not deleted
    binary = t.modem('IncomingBinary', (BOB,), '(s)')[0]
    t.settle(0.5)
    check('a binary message is not kept as a message', not t.query('SELECT _id FROM parts WHERE text = ?', ''),
          t.query('SELECT message_id, text FROM parts'))
    check('and is left on the modem for mmsd-tng', binary in t.report()['messages'], t.report()['messages'])
    t.modem('Take', (binary,), '(o)')

    # the modem going away entirely and coming back (a ModemManager restart, a modem reset): the service lives on
    t.modem('SetModem', (False,), '(b)')
    t.wait('the modem gone, not available', lambda: not t.prop('Available'))
    t.modem('SetModem', (True,), '(b)')
    t.wait('the modem back, available again', lambda: t.prop('Available'))
    t.incoming(BOB, 'After the restart')
    t.wait('and messages come again', lambda: t.query('SELECT _id FROM parts WHERE text = ?', 'After the restart'))
    n = t.wait('and are notified', lambda: t.notification('0170 9999999'))
    t.modem('Dismiss', (n[0] if n else 0,), '(u)')

    # a blocked number's text: kept, archived, seen, and not notified
    carol = '+491705555555'
    block(os.path.dirname(os.path.dirname(t.store)), '0170 5555555')
    t.settle(0.5)
    t.incoming(carol, 'Buy now')
    conversation = t.wait('a blocked number\'s text is kept', lambda: t.conversation_of(carol))
    t.wait('in an archived conversation', lambda: t.query('SELECT archive_status FROM conversations WHERE _id = ?',
                                                            conversation) == [(1,)])
    check('seen', t.messages(conversation)[-1][3] == 1, t.messages(conversation))
    t.settle()
    check('and not notified', t.notification() is None, t.report()['notifications'])

    # a delivery report is not a message
    report = t.modem('StatusReport')[0]
    t.wait('a delivery report is deleted off the modem', lambda: report not in t.report()['messages'])
    check('and kept nowhere', len(t.messages(anna)) == 4, t.messages(anna))

    # --- sending
    t.modem('Clear')
    same = t.service('GetConversation', (['0170 1234567'],), '(as)')[0]
    check('a number as typed finds its conversation', same == anna, (same, anna))
    group = t.service('GetConversation', ([ANNA, '0170 9999999', ANNA],), '(as)')[0]
    check('the same people are one group, whatever the order',
          group == t.service('GetConversation', ([BOB, ANNA],), '(as)')[0] and group not in (anna, bob))
    message = t.service('Send', (anna, 'On my way'), '(xs)')[0]
    t.wait('a message is sent', lambda: t.status(message) == COMPLETE)
    log = t.report()['log']
    check('as Create then Send', [e[0] for e in log if e[0] in ('Create', 'Send')] == ['Create', 'Send'], log)
    check('to the number in E.164', ['Create', [ANNA, 'On my way']] in log, log)
    t.wait('and deleted off the modem after', lambda: not t.report()['messages'])

    # a group: one message each
    t.modem('Clear')
    message = t.service('Send', (group, 'Dinner at 8'), '(xs)')[0]
    t.wait('a group message is sent', lambda: t.status(message) == COMPLETE)
    sent_to = sorted(e[1][0] for e in t.logged('Create'))
    check('to each of them', sent_to == [ANNA, BOB], sent_to)

    # refused by the network: given up at once, notified
    t.modem('SetSendError', (UNKNOWN_ERROR,), '(s)')
    message = t.service('Send', (anna, 'Not going anywhere'), '(xs)')[0]
    t.wait('a message the network refuses fails', lambda: t.status(message) == FAILED)
    n = t.wait('"Message not sent"', lambda: t.notification('Message not sent'))
    if n:
        check('naming the conversation', n[1]['body'] == '0170 1234567', n)
    t.modem('SetSendError', ('',), '(s)')
    t.service('Resend', (message,), '(x)')
    t.wait('tried again, it is sent', lambda: t.status(message) == COMPLETE)
    t.wait('and its notification goes', lambda: t.notification('Message not sent') is None)

    # no network: tried again, and sent when the network is back
    t.modem('SetSendError', (NO_NETWORK,), '(s)')
    message = t.service('Send', (bob, 'Later'), '(xs)')[0]
    t.wait('with no network a message waits to be tried again', lambda: t.status(message) == AWAITING_RETRY)
    check('not notified as failed', t.notification('Message not sent') is None)
    t.modem('SetSendError', ('',), '(s)')
    t.wait('5 s on it is sent', lambda: t.status(message) == COMPLETE, timeout=8)

    # the modem going (disabled): a message waits; it goes the moment the modem is back
    t.modem('SetMessaging', (False,), '(b)')
    t.wait('without a modem, not available', lambda: not t.prop('Available'))
    message = t.service('Send', (bob, 'When you can'), '(xs)')[0]
    t.wait('a message waits for the modem', lambda: t.status(message) == AWAITING_RETRY)
    t.modem('SetMessaging', (True,), '(b)')
    t.wait('the modem back, available', lambda: t.prop('Available'))
    t.wait('and the message goes at once', lambda: t.status(message) == COMPLETE, timeout=2)

    # --- the conversation's state
    t.service('SetDraft', (bob, 'Half-written'), '(xs)')
    check('a draft is kept', t.query('SELECT show_draft, draft_snippet_text FROM conversations WHERE _id = ?', bob)
          == [(1, 'Half-written')])
    t.service('SetArchived', (bob, True), '(xb)')
    check('archived', t.query('SELECT archive_status FROM conversations WHERE _id = ?', bob) == [(1,)])
    t.service('DeleteConversation', (bob,), '(x)')
    check('deleted, with its messages', not t.messages(bob)
          and not t.query('SELECT _id FROM conversations WHERE _id = ?', bob))

    # tapping a notification opens its conversation in the app
    t.modem('Clear')
    t.incoming(ANNA, 'Tap me')
    n = t.wait('a new message is notified', lambda: t.notification('0170 1234567'))
    t.modem('InvokeAction', (n[0] if n else 0, 'default'), '(us)')
    opened = t.wait('a tap opens the conversation', lambda: t.logged('ActivateAction'))
    if opened:
        check('this one', opened[0][1:] == ['open-conversation', [anna]], opened)
    t.wait('and the notification goes', lambda: t.notification('0170 1234567') is None)

    # --- gateway mode: the messages are gsm2sipd's
    t.modem('SetGateway', (True,), '(b)')
    t.wait('in gateway mode, not available', lambda: not t.prop('Available'))
    sms = t.incoming(BOB, 'For the gateway')
    t.settle(0.5)
    check('a message is left on the modem', sms in t.report()['messages'], t.report()['messages'])
    t.modem('SetGateway', (False,), '(b)')
    t.wait('gateway mode off: what came meanwhile is taken',
           lambda: t.query('SELECT _id FROM parts WHERE text = ?', 'For the gateway'))
    t.wait('and deleted off the modem', lambda: sms not in t.report()['messages'])


def main():
    build, source = sys.argv[1], sys.argv[2]
    tmp = tempfile.mkdtemp(prefix='test-sms-')
    procs = []
    try:
        system, system_address = start_bus(tmp, 'system')
        session, session_address = start_bus(tmp, 'session')
        procs += [system, session]
        env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS=session_address, NEO_MESSAGING_SYSTEM_BUS=system_address,
                   XDG_DATA_HOME=os.path.join(tmp, 'data'), XDG_CONFIG_HOME=os.path.join(tmp, 'config'),
                   GSETTINGS_BACKEND='memory', LANG='C.UTF-8', LC_ALL='C.UTF-8')
        mock = subprocess.Popen([sys.executable, os.path.join(source, 'tests', 'mock-modem.py'), system_address,
                                 '--waiting'], env=env, stdout=subprocess.PIPE, text=True)
        procs.append(mock)
        if mock.stdout.readline().strip() != 'ready':
            print('FAIL the mock modem did not start')
            return 1
        procs.append(subprocess.Popen([os.path.join(build, 'neo-messaging-sms')], env=env))
        run(Test(session_address, os.path.join(tmp, 'data', 'neo-messaging', 'bugle_db')))
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
