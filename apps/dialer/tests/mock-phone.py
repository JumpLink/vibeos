#!/usr/bin/python3
# A phone for the call service to talk to, without a modem or a shell: what neo-dialer-calls finds on its two buses,
# for the tests (test-calls.py) and for trying the in-call screen with no real call.
#
# On the "system" bus (a bus of its own, NEO_DIALER_SYSTEM_BUS): ModemManager with one modem, its Voice interface
# and its calls, behaving as ModemManager 1.24 does: a call it makes is UNKNOWN until started, a terminated one stays
# until deleted, every state change comes as StateChanged and as a property change.
#
# On the session bus: callaudiod's CallAudio, a notification server, and the shell's LaunchFullScreen.
#
# neo.Test.Phone on the session bus drives it: a call comes in, the other side answers or hangs up; and it
# tells what the service did (the method calls it got, the notifications it holds).
import json
import sys

from gi.repository import Gio, GLib

MM = 'org.freedesktop.ModemManager1'
MM_PATH = '/org/freedesktop/ModemManager1'
MODEM_PATH = MM_PATH + '/Modem/0'
NOT_SUPPORTED = MM + '.Error.MobileEquipment.NotSupported'

# MMCallState, MMCallStateReason, MMCallDirection
UNKNOWN, DIALING, RINGING_OUT, RINGING_IN, ACTIVE, HELD, WAITING, TERMINATED = range(8)
R_UNKNOWN, R_OUTGOING_STARTED, R_INCOMING_NEW, R_ACCEPTED, R_TERMINATED, R_REFUSED_OR_BUSY, R_ERROR = range(7)
INCOMING, OUTGOING = 1, 2

MANAGER_XML = '''<node><interface name="org.freedesktop.DBus.ObjectManager">
<method name="GetManagedObjects"><arg type="a{oa{sa{sv}}}" direction="out"/></method>
<signal name="InterfacesAdded"><arg type="o"/><arg type="a{sa{sv}}"/></signal>
<signal name="InterfacesRemoved"><arg type="o"/><arg type="as"/></signal>
</interface></node>'''

MODEM_XML = '''<node>
<interface name="org.freedesktop.ModemManager1.Modem">
<property name="Manufacturer" type="s" access="read"/><property name="Model" type="s" access="read"/>
<property name="Revision" type="s" access="read"/><property name="State" type="i" access="read"/>
</interface>
<interface name="org.freedesktop.ModemManager1.Modem.Modem3gpp">
<property name="OperatorCode" type="s" access="read"/>
</interface>
<interface name="org.freedesktop.ModemManager1.Modem.Voice">
<method name="ListCalls"><arg type="ao" direction="out"/></method>
<method name="CreateCall"><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
<method name="DeleteCall"><arg type="o" direction="in"/></method>
<method name="HoldAndAccept"/><method name="HangupAndAccept"/><method name="HangupAll"/>
<signal name="CallAdded"><arg type="o"/></signal><signal name="CallDeleted"><arg type="o"/></signal>
<property name="Calls" type="ao" access="read"/><property name="EmergencyOnly" type="b" access="read"/>
</interface></node>'''

CALL_XML = '''<node><interface name="org.freedesktop.ModemManager1.Call">
<method name="Start"/><method name="Accept"/><method name="Hangup"/>
<method name="SendDtmf"><arg type="s" direction="in"/></method>
<signal name="StateChanged"><arg type="i"/><arg type="i"/><arg type="u"/></signal>
<property name="State" type="i" access="read"/><property name="StateReason" type="i" access="read"/>
<property name="Direction" type="i" access="read"/><property name="Number" type="s" access="read"/>
<property name="Multiparty" type="b" access="read"/>
</interface></node>'''

SESSION_XML = '''<node>
<interface name="org.mobian_project.CallAudio">
<method name="SelectMode"><arg type="u" direction="in"/><arg type="b" direction="out"/></method>
<method name="EnableSpeaker"><arg type="b" direction="in"/><arg type="b" direction="out"/></method>
<method name="MuteMic"><arg type="b" direction="in"/><arg type="b" direction="out"/></method>
</interface>
<interface name="org.freedesktop.Notifications">
<method name="Notify"><arg type="s" direction="in"/><arg type="u" direction="in"/><arg type="s" direction="in"/>
<arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="as" direction="in"/>
<arg type="a{sv}" direction="in"/><arg type="i" direction="in"/><arg type="u" direction="out"/></method>
<method name="CloseNotification"><arg type="u" direction="in"/></method>
<signal name="ActionInvoked"><arg type="u"/><arg type="s"/></signal>
<signal name="NotificationClosed"><arg type="u"/><arg type="u"/></signal>
</interface>
<interface name="neo.GnomePhone">
<method name="LaunchFullScreen"><arg type="s" direction="in"/><arg type="b" direction="out"/></method>
</interface>
<interface name="neo.Test.Phone">
<method name="Incoming"><arg name="number" type="s" direction="in"/><arg type="o" direction="out"/></method>
<method name="RemoteAnswer"><arg type="o" direction="in"/></method>
<method name="RemoteHangup"><arg type="o" direction="in"/><arg name="reason" type="i" direction="in"/></method>
<method name="SetInUse"><arg name="in_use" type="b" direction="in"/></method>
<method name="SetHold"><arg name="supported" type="b" direction="in"/></method>
<method name="SetDtmf"><arg name="supported" type="b" direction="in"/></method>
<method name="SetModem"><arg name="present" type="b" direction="in"/></method>
<method name="InvokeAction"><arg type="u" direction="in"/><arg type="s" direction="in"/></method>
<method name="Dismiss"><arg type="u" direction="in"/></method>
<method name="Report"><arg type="s" direction="out"/></method>
<method name="Clear"/>
</interface></node>'''


class Call:
    def __init__(self, phone, number, direction, state):
        phone.next_call += 1
        self.path = '%s/Call/%d' % (MM_PATH, phone.next_call)
        self.number, self.direction, self.state, self.reason = number, direction, state, R_UNKNOWN
        self.ids = []

    def props(self):
        return {'State': GLib.Variant('i', self.state), 'StateReason': GLib.Variant('i', self.reason),
                'Direction': GLib.Variant('i', self.direction), 'Number': GLib.Variant('s', self.number),
                'Multiparty': GLib.Variant('b', False)}


class Phone:
    def __init__(self, system, session):
        self.system, self.session = system, session
        self.calls = {}
        self.next_call = 0
        self.log = []                   # [method, args...] as the service called them
        self.notifications = {}         # id -> {title, body, actions, hints}
        self.next_notification = 0
        self.in_use = False             # the shell declines the full screen (the phone is in use, unlocked)
        self.hold = False
        self.dtmf = False               # SendDtmf refused, as a modem that cannot (the service then learns it)
        self.chld = False               # a +CHLD is moving the calls
        self.present = True             # the modem is there at all (ModemManager found it)
        self.modem_ids = []             # its interfaces' registrations

        modem = Gio.DBusNodeInfo.new_for_xml(MODEM_XML)
        self.modem_ifaces = {i.name: i for i in modem.interfaces}
        self.call_iface = Gio.DBusNodeInfo.new_for_xml(CALL_XML).interfaces[0]
        system.register_object(MM_PATH, Gio.DBusNodeInfo.new_for_xml(MANAGER_XML).interfaces[0],
                               self.manager_call, None, None)
        for iface in self.modem_ifaces.values():
            self.modem_ids.append(system.register_object(MODEM_PATH, iface, self.modem_call, self.modem_get, None))
        for iface in Gio.DBusNodeInfo.new_for_xml(SESSION_XML).interfaces:
            session.register_object('/org/mobian_project/CallAudio' if 'CallAudio' in iface.name else
                                    '/org/freedesktop/Notifications' if 'Notifications' in iface.name else
                                    '/neo/GnomePhone' if 'GnomePhone' in iface.name else
                                    '/neo/Test/Phone', iface, self.session_call, None, None)

    # --- ModemManager
    def modem_props(self, name):
        return {
            MM + '.Modem': {'Manufacturer': GLib.Variant('s', 'Mock'), 'Model': GLib.Variant('s', 'Modem'),
                            'Revision': GLib.Variant('s', '1'), 'State': GLib.Variant('i', 11)},
            MM + '.Modem.Modem3gpp': {'OperatorCode': GLib.Variant('s', '26203')},
            MM + '.Modem.Voice': {'Calls': GLib.Variant('ao', list(self.calls)),
                                  'EmergencyOnly': GLib.Variant('b', False)},
        }[name]

    def manager_call(self, conn, sender, path, iface, method, params, inv):
        objects = {MODEM_PATH: {name: self.modem_props(name) for name in self.modem_ifaces}} if self.present else {}
        inv.return_value(GLib.Variant('(a{oa{sa{sv}}})', (objects,)))

    def modem_get(self, conn, sender, path, iface, prop):
        return self.modem_props(iface)[prop]

    def voice_changed(self):
        self.system.emit_signal(None, MODEM_PATH, 'org.freedesktop.DBus.Properties', 'PropertiesChanged',
                                GLib.Variant('(sa{sv}as)', (MM + '.Modem.Voice',
                                                            {'Calls': GLib.Variant('ao', list(self.calls))}, [])))

    def add_call(self, call):
        self.calls[call.path] = call
        call.ids.append(self.system.register_object(call.path, self.call_iface, self.call_call, self.call_get, None))
        self.system.emit_signal(None, MODEM_PATH, MM + '.Modem.Voice', 'CallAdded', GLib.Variant('(o)', (call.path,)))
        self.voice_changed()

    def in_state(self, state):
        return [call for call in self.calls.values() if call.state == state]

    def set_state(self, call, state, reason):
        old, call.state, call.reason = call.state, state, reason
        # ModemManager's order: the signal first, then the properties
        self.system.emit_signal(None, call.path, MM + '.Call', 'StateChanged',
                                GLib.Variant('(iiu)', (old, state, reason)))
        self.system.emit_signal(None, call.path, 'org.freedesktop.DBus.Properties', 'PropertiesChanged',
                                GLib.Variant('(sa{sv}as)', (MM + '.Call', {
                                    'State': GLib.Variant('i', state), 'StateReason': GLib.Variant('i', reason)},
                                    [])))
        # a waiting call left alone rings as a call of its own (the network's +CCWA becomes a RING), unless a +CHLD
        # is moving the calls
        if state == TERMINATED and not self.chld and not (self.in_state(ACTIVE) or self.in_state(HELD)):
            for waiting in self.in_state(WAITING):
                self.set_state(waiting, RINGING_IN, R_INCOMING_NEW)

    def modem_call(self, conn, sender, path, iface, method, params, inv):
        args = params.unpack()
        self.log.append([method] + [a for a in args if not isinstance(a, dict)] +
                        [a.get('number') for a in args if isinstance(a, dict)])
        if method == 'ListCalls':
            return inv.return_value(GLib.Variant('(ao)', (list(self.calls),)))
        if method == 'CreateCall':
            call = Call(self, args[0]['number'], OUTGOING, UNKNOWN)
            self.add_call(call)
            return inv.return_value(GLib.Variant('(o)', (call.path,)))
        if method == 'DeleteCall':
            call = self.calls.pop(args[0], None)
            if call:
                for i in call.ids:
                    self.system.unregister_object(i)
                self.system.emit_signal(None, MODEM_PATH, MM + '.Modem.Voice', 'CallDeleted',
                                        GLib.Variant('(o)', (call.path,)))
                self.voice_changed()
            return inv.return_value(None)
        if method in ('HoldAndAccept', 'HangupAndAccept') and not self.hold:
            return inv.return_dbus_error(NOT_SUPPORTED, 'Operation not supported')
        inv.return_value(None)
        if method in ('HoldAndAccept', 'HangupAndAccept'):
            # +CHLD=2 / =1: the calls that are up go on hold or end, and the waiting call (else the held ones) comes
            # up; with no call at all (the service's hold probe) nothing happens
            active, held = self.in_state(ACTIVE), self.in_state(HELD)
            others = self.in_state(WAITING)[:1] or held
            self.chld = True
            for call in active:
                if method == 'HoldAndAccept':
                    self.set_state(call, HELD, call.reason)
                else:
                    self.set_state(call, TERMINATED, R_TERMINATED)
            for call in others:
                self.set_state(call, ACTIVE, R_ACCEPTED)
            self.chld = False

    def call_get(self, conn, sender, path, iface, prop):
        return self.calls[path].props()[prop]

    def call_call(self, conn, sender, path, iface, method, params, inv):
        call = self.calls.get(path)
        self.log.append([method, path] + list(params.unpack()))
        if method == 'SendDtmf' and not self.dtmf:
            return inv.return_dbus_error(NOT_SUPPORTED, 'Operation not supported')
        inv.return_value(None)
        if method == 'Start':
            self.set_state(call, DIALING, R_OUTGOING_STARTED)
            GLib.timeout_add(100, self.ringing_out, call)
        elif method == 'Accept':
            self.set_state(call, ACTIVE, R_ACCEPTED)
        elif method == 'Hangup':
            self.set_state(call, TERMINATED, R_TERMINATED)

    def ringing_out(self, call):
        # the other phone rings, unless it was answered already
        if call.state == DIALING:
            self.set_state(call, RINGING_OUT, R_OUTGOING_STARTED)
        return False

    # --- the session's services and the test's controls
    def session_call(self, conn, sender, path, iface, method, params, inv):
        args = params.unpack()
        if iface == 'neo.Test.Phone':
            return self.control(method, args, inv)
        self.log.append([method] + [a for a in args if not isinstance(a, dict)])
        if method == 'Notify':
            app, replaces, icon, title, body, actions, hints, timeout = args
            nid = replaces if replaces in self.notifications else self.next_notification + 1
            self.next_notification = max(self.next_notification, nid)
            self.notifications[nid] = {'title': title, 'body': body, 'actions': actions[::2],
                                       'urgency': hints.get('urgency', 1),
                                       'no-peek': hints.get('x-neo-no-peek', False),
                                       'chronometer': hints.get('x-neo-chronometer', 0)}
            return inv.return_value(GLib.Variant('(u)', (nid,)))
        if method == 'CloseNotification':
            if self.notifications.pop(args[0], None) is not None:
                self.session.emit_signal(None, '/org/freedesktop/Notifications', 'org.freedesktop.Notifications',
                                         'NotificationClosed', GLib.Variant('(uu)', (args[0], 3)))
            return inv.return_value(None)
        if method == 'LaunchFullScreen':
            return inv.return_value(GLib.Variant('(b)', (not self.in_use,)))
        inv.return_value(GLib.Variant('(b)', (True,)))

    def set_modem(self, present):
        # the whole modem goes and comes, as at a ModemManager restart or when the modem resets: its object is
        # removed (all interfaces) and added again (object-removed / object-added to a GDBusObjectManagerClient)
        if present == self.present:
            return
        self.present = present
        if present:
            for iface in self.modem_ifaces.values():
                self.modem_ids.append(self.system.register_object(MODEM_PATH, iface, self.modem_call,
                                                                  self.modem_get, None))
            self.system.emit_signal(None, MM_PATH, 'org.freedesktop.DBus.ObjectManager', 'InterfacesAdded',
                                    GLib.Variant('(oa{sa{sv}})', (MODEM_PATH, {name: self.modem_props(name)
                                                                               for name in self.modem_ifaces})))
        else:
            for i in self.modem_ids:
                self.system.unregister_object(i)
            self.modem_ids = []
            self.system.emit_signal(None, MM_PATH, 'org.freedesktop.DBus.ObjectManager', 'InterfacesRemoved',
                                    GLib.Variant('(oas)', (MODEM_PATH, list(self.modem_ifaces))))

    def control(self, method, args, inv):
        if method == 'SetModem':
            self.set_modem(args[0])
            return inv.return_value(None)
        if method == 'Incoming':
            # a second call, while one is up or held, waits (ModemManager's WAITING, +CCWA)
            busy = self.in_state(ACTIVE) or self.in_state(HELD)
            call = Call(self, args[0], INCOMING, WAITING if busy else RINGING_IN)
            call.reason = R_INCOMING_NEW
            self.add_call(call)
            return inv.return_value(GLib.Variant('(o)', (call.path,)))
        if method == 'RemoteAnswer':
            self.set_state(self.calls[args[0]], ACTIVE, R_ACCEPTED)
        elif method == 'RemoteHangup':
            self.set_state(self.calls[args[0]], TERMINATED, args[1])
        elif method == 'SetInUse':
            self.in_use = args[0]
        elif method == 'SetHold':
            self.hold = args[0]
        elif method == 'SetDtmf':
            self.dtmf = args[0]
        elif method == 'InvokeAction':
            self.session.emit_signal(None, '/org/freedesktop/Notifications', 'org.freedesktop.Notifications',
                                     'ActionInvoked', GLib.Variant('(us)', tuple(args)))
        elif method == 'Dismiss':
            self.notifications.pop(args[0], None)
            self.session.emit_signal(None, '/org/freedesktop/Notifications', 'org.freedesktop.Notifications',
                                     'NotificationClosed', GLib.Variant('(uu)', (args[0], 2)))
        elif method == 'Report':
            return inv.return_value(GLib.Variant('(s)', (json.dumps({
                'log': self.log, 'notifications': self.notifications,
                'calls': {p: [c.state, c.direction, c.number] for p, c in self.calls.items()}}),)))
        elif method == 'Clear':
            self.log = []
        inv.return_value(None)


def main():
    system = Gio.DBusConnection.new_for_address_sync(
        sys.argv[1], Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT |
        Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
    session = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    phone = Phone(system, session)
    names = [(system, MM)] + [(session, n) for n in ('org.mobian_project.CallAudio', 'org.freedesktop.Notifications',
                                                     'neo.GnomePhone', 'neo.Test.Phone')]
    for conn, name in names:
        conn.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus', 'RequestName',
                       GLib.Variant('(su)', (name, 4)), None, Gio.DBusCallFlags.NONE, -1, None)
    print('ready', flush=True)
    GLib.MainLoop().run()


if __name__ == '__main__':
    main()
