#!/usr/bin/python3
# A phone for the SMS service to talk to, without a modem or a shell: what vibeos-messaging-sms finds on its two buses,
# for the tests (test-sms.py).
#
# On the "system" bus (a bus of its own, VIBEOS_MESSAGING_SYSTEM_BUS): ModemManager with one modem and its Messaging
# interface, behaving as ModemManager 1.24 does: a message that comes is announced (Added, received TRUE) while its
# parts are still RECEIVING and turns RECEIVED when the last one is in; one made by Create is announced with
# received FALSE, and once sent carries the network's message reference; a deleted one is gone. A delivery report is
# a message of its own (a STATUS_REPORT carrying the reference of the one it reports on), as ModemManager does not
# tie it to the message it reports on. And the SIP gateway, whose Enabled is gateway mode.
#
# On the session bus: a notification server, and the Messaging app's org.freedesktop.Application (what a tap on a
# notification opens).
#
# vibeos.Test.Modem on the session bus drives it: a message comes, sending fails, the modem goes; and it tells
# what the service did (the method calls it got, the notifications it holds, the messages still on the modem).
import json
import sys

from gi.repository import Gio, GLib

MM = 'org.freedesktop.ModemManager1'
MM_PATH = '/org/freedesktop/ModemManager1'
MODEM_PATH = MM_PATH + '/Modem/0'
MESSAGING = MM + '.Modem.Messaging'
GATEWAY = 'com.callagent.Gsm2sipd'

# MMSmsState, MMSmsPduType
STORED, RECEIVING, RECEIVED, SENDING, SENT = 1, 2, 3, 4, 5
DELIVER, SUBMIT, STATUS_REPORT = 1, 2, 3

MANAGER_XML = '''<node><interface name="org.freedesktop.DBus.ObjectManager">
<method name="GetManagedObjects"><arg type="a{oa{sa{sv}}}" direction="out"/></method>
<signal name="InterfacesAdded"><arg type="o"/><arg type="a{sa{sv}}"/></signal>
<signal name="InterfacesRemoved"><arg type="o"/><arg type="as"/></signal>
</interface></node>'''

MODEM_XML = '''<node>
<interface name="org.freedesktop.ModemManager1.Modem">
<property name="State" type="i" access="read"/>
</interface>
<interface name="org.freedesktop.ModemManager1.Modem.Modem3gpp">
<property name="OperatorCode" type="s" access="read"/>
</interface>
<interface name="org.freedesktop.ModemManager1.Modem.Messaging">
<method name="List"><arg type="ao" direction="out"/></method>
<method name="Create"><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
<method name="Delete"><arg type="o" direction="in"/></method>
<signal name="Added"><arg type="o"/><arg type="b"/></signal><signal name="Deleted"><arg type="o"/></signal>
<property name="Messages" type="ao" access="read"/>
</interface></node>'''

SMS_XML = '''<node><interface name="org.freedesktop.ModemManager1.Sms">
<method name="Send"/>
<property name="State" type="u" access="read"/><property name="PduType" type="u" access="read"/>
<property name="Number" type="s" access="read"/><property name="Text" type="s" access="read"/>
<property name="Timestamp" type="s" access="read"/><property name="Data" type="ay" access="read"/>
<property name="MessageReference" type="u" access="read"/><property name="DeliveryState" type="u" access="read"/>
<property name="DeliveryReportRequest" type="b" access="read"/>
</interface></node>'''

GATEWAY_XML = '''<node><interface name="com.callagent.Gsm2sipd1">
<property name="Enabled" type="b" access="read"/>
</interface></node>'''

SESSION_XML = '''<node>
<interface name="org.freedesktop.Notifications">
<method name="Notify"><arg type="s" direction="in"/><arg type="u" direction="in"/><arg type="s" direction="in"/>
<arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="as" direction="in"/>
<arg type="a{sv}" direction="in"/><arg type="i" direction="in"/><arg type="u" direction="out"/></method>
<method name="CloseNotification"><arg type="u" direction="in"/></method>
<signal name="ActionInvoked"><arg type="u"/><arg type="s"/></signal>
<signal name="NotificationClosed"><arg type="u"/><arg type="u"/></signal>
</interface>
<interface name="org.freedesktop.Application">
<method name="ActivateAction"><arg type="s" direction="in"/><arg type="av" direction="in"/>
<arg type="a{sv}" direction="in"/></method>
</interface>
<interface name="vibeos.Test.Modem">
<method name="Incoming"><arg name="number" type="s" direction="in"/><arg name="parts" type="as" direction="in"/>
<arg name="timestamp" type="s" direction="in"/><arg type="o" direction="out"/></method>
<method name="StatusReport"><arg type="o" direction="out"/></method>
<method name="DeliveryReport"><arg name="number" type="s" direction="in"/>
<arg name="reference" type="u" direction="in"/><arg name="state" type="u" direction="in"/>
<arg type="o" direction="out"/></method>
<method name="IncomingBinary"><arg name="number" type="s" direction="in"/><arg type="o" direction="out"/></method>
<method name="SetModem"><arg name="present" type="b" direction="in"/></method>
<method name="Take"><arg name="sms" type="o" direction="in"/></method>
<method name="SetSendError"><arg name="error" type="s" direction="in"/></method>
<method name="SetMessaging"><arg name="present" type="b" direction="in"/></method>
<method name="SetGateway"><arg name="enabled" type="b" direction="in"/></method>
<method name="InvokeAction"><arg type="u" direction="in"/><arg type="s" direction="in"/></method>
<method name="Dismiss"><arg type="u" direction="in"/></method>
<method name="Report"><arg type="s" direction="out"/></method>
<method name="Clear"/>
</interface></node>'''


class Sms:
    def __init__(self, modem, number, text, state, pdu_type, timestamp='', data=b''):
        modem.next_sms += 1
        self.path = '%s/SMS/%d' % (MM_PATH, modem.next_sms)
        self.number, self.text, self.state, self.pdu_type, self.timestamp = number, text, state, pdu_type, timestamp
        self.data = data                # a binary message's bytes (an MMS notification), with no text
        self.reference = 0              # TP-MR: a sent one's, or the one a delivery report reports on
        self.delivery_state = 0x100     # a delivery report's (MMSmsDeliveryState); UNKNOWN for any other message
        self.report_request = False     # Create's "delivery-report-request"
        self.ids = []

    def props(self):
        return {'State': GLib.Variant('u', self.state), 'PduType': GLib.Variant('u', self.pdu_type),
                'Number': GLib.Variant('s', self.number), 'Text': GLib.Variant('s', self.text),
                'Timestamp': GLib.Variant('s', self.timestamp), 'Data': GLib.Variant('ay', self.data),
                'MessageReference': GLib.Variant('u', self.reference),
                'DeliveryState': GLib.Variant('u', self.delivery_state),
                'DeliveryReportRequest': GLib.Variant('b', self.report_request)}


class Modem:
    def __init__(self, system, session, waiting):
        self.system, self.session = system, session
        self.messages = {}
        self.next_sms = 0
        self.next_reference = 7         # the modem's TP-MR counter, a byte
        self.log = []                   # [method, args...] as the service called them
        self.notifications = {}         # id -> {title, body, actions, no-peek}
        self.next_notification = 0
        self.send_error = ''            # the D-Bus error Send answers, '' to send
        self.messaging = True           # the modem has its Messaging interface (it is enabled)
        self.present = True             # the modem is there at all (ModemManager found it)
        self.modem_ids = []             # its interfaces' registrations, but Messaging's (messaging_ids)
        self.gateway = False
        self.messaging_ids = []

        nodes = Gio.DBusNodeInfo.new_for_xml(MODEM_XML)
        self.modem_ifaces = {i.name: i for i in nodes.interfaces}
        self.sms_iface = Gio.DBusNodeInfo.new_for_xml(SMS_XML).interfaces[0]
        system.register_object(MM_PATH, Gio.DBusNodeInfo.new_for_xml(MANAGER_XML).interfaces[0],
                               self.manager_call, None, None)
        for name, iface in self.modem_ifaces.items():
            ident = system.register_object(MODEM_PATH, iface, self.modem_call, self.modem_get, None)
            (self.messaging_ids if name == MESSAGING else self.modem_ids).append(ident)
        system.register_object('/com/callagent/Gsm2sipd', Gio.DBusNodeInfo.new_for_xml(GATEWAY_XML).interfaces[0],
                               lambda *a: None, lambda *a: GLib.Variant('b', self.gateway), None)
        for iface in Gio.DBusNodeInfo.new_for_xml(SESSION_XML).interfaces:
            session.register_object('/org/freedesktop/Notifications' if 'Notifications' in iface.name else
                                    '/vibeos/Messaging' if 'Application' in iface.name else
                                    '/vibeos/Test/Modem', iface, self.session_call, None, None)
        # what came while the service was not running: on the modem when it starts
        if waiting:
            self.add(Sms(self, '+491709999999', 'While you were away', RECEIVED, DELIVER, '2026-11-08T01:00:00+01'),
                     True, announce=False)

    # --- ModemManager
    def modem_props(self, name):
        return {
            MM + '.Modem': {'State': GLib.Variant('i', 11)},
            MM + '.Modem.Modem3gpp': {'OperatorCode': GLib.Variant('s', '26203')},
            MESSAGING: {'Messages': GLib.Variant('ao', list(self.messages))},
        }[name]

    def ifaces(self):
        return [name for name in self.modem_ifaces if name != MESSAGING or self.messaging]

    def manager_call(self, conn, sender, path, iface, method, params, inv):
        objects = {MODEM_PATH: {name: self.modem_props(name) for name in self.ifaces()}} if self.present else {}
        inv.return_value(GLib.Variant('(a{oa{sa{sv}}})', (objects,)))

    def modem_get(self, conn, sender, path, iface, prop):
        return self.modem_props(iface)[prop]

    def add(self, sms, received, announce=True):
        self.messages[sms.path] = sms
        sms.ids.append(self.system.register_object(sms.path, self.sms_iface, self.sms_call, self.sms_get, None))
        if announce:
            self.system.emit_signal(None, MODEM_PATH, MESSAGING, 'Added', GLib.Variant('(ob)', (sms.path, received)))

    def set_state(self, sms, state):
        # with the text: a long message's grows as its parts come
        sms.state = state
        self.system.emit_signal(None, sms.path, 'org.freedesktop.DBus.Properties', 'PropertiesChanged',
                                GLib.Variant('(sa{sv}as)', (MM + '.Sms', {'State': GLib.Variant('u', state),
                                                                         'Text': GLib.Variant('s', sms.text)}, [])))

    def modem_call(self, conn, sender, path, iface, method, params, inv):
        args = params.unpack()
        # Create's properties: [number, text], and the report request when it is there at all
        self.log.append([method] + [a for a in args if not isinstance(a, dict)] +
                        [[a.get('number'), a.get('text')] + ([a['delivery-report-request']]
                                                             if 'delivery-report-request' in a else [])
                         for a in args if isinstance(a, dict)])
        if method == 'List':
            return inv.return_value(GLib.Variant('(ao)', (list(self.messages),)))
        if method == 'Create':
            sms = Sms(self, args[0]['number'], args[0]['text'], STORED, SUBMIT)
            sms.report_request = args[0].get('delivery-report-request', False)
            self.add(sms, False)
            return inv.return_value(GLib.Variant('(o)', (sms.path,)))
        if method == 'Delete':
            sms = self.messages.pop(args[0], None)
            if sms:
                for i in sms.ids:
                    self.system.unregister_object(i)
                self.system.emit_signal(None, MODEM_PATH, MESSAGING, 'Deleted', GLib.Variant('(o)', (sms.path,)))
            return inv.return_value(None)
        inv.return_value(None)

    def sms_get(self, conn, sender, path, iface, prop):
        return self.messages[path].props()[prop]

    def sms_call(self, conn, sender, path, iface, method, params, inv):
        sms = self.messages.get(path)
        if self.send_error:
            self.log.append([method, path, sms.number if sms else None])
            return inv.return_dbus_error(self.send_error, 'Send failed (mock)')
        # sent: the network's reference for it, set before the reply as ModemManager does
        sms.reference = self.next_reference
        self.next_reference = (self.next_reference + 1) % 256
        self.log.append([method, path, sms.number if sms else None, sms.reference])
        self.set_state(sms, SENT)
        inv.return_value(None)

    def finish(self, sms, rest):
        # the next part is in: RECEIVED with the last
        sms.text += rest.pop(0)
        if not rest:
            self.set_state(sms, RECEIVED)
            return False
        return True

    def set_messaging(self, present):
        if present == self.messaging:
            return
        self.messaging = present
        if present:
            self.messaging_ids.append(self.system.register_object(MODEM_PATH, self.modem_ifaces[MESSAGING],
                                                                  self.modem_call, self.modem_get, None))
            self.system.emit_signal(None, MM_PATH, 'org.freedesktop.DBus.ObjectManager', 'InterfacesAdded',
                                    GLib.Variant('(oa{sa{sv}})', (MODEM_PATH,
                                                                  {MESSAGING: self.modem_props(MESSAGING)})))
        else:
            for i in self.messaging_ids:
                self.system.unregister_object(i)
            self.messaging_ids = []
            self.system.emit_signal(None, MM_PATH, 'org.freedesktop.DBus.ObjectManager', 'InterfacesRemoved',
                                    GLib.Variant('(oas)', (MODEM_PATH, [MESSAGING])))

    def set_modem(self, present):
        # the whole modem goes and comes, as at a ModemManager restart or when the modem resets: its object is
        # removed (all interfaces) and added again (object-removed / object-added to a GDBusObjectManagerClient)
        if present == self.present:
            return
        self.present = present
        if present:
            for name, iface in self.modem_ifaces.items():
                ident = self.system.register_object(MODEM_PATH, iface, self.modem_call, self.modem_get, None)
                (self.messaging_ids if name == MESSAGING else self.modem_ids).append(ident)
            self.messaging = True
            self.system.emit_signal(None, MM_PATH, 'org.freedesktop.DBus.ObjectManager', 'InterfacesAdded',
                                    GLib.Variant('(oa{sa{sv}})', (MODEM_PATH, {name: self.modem_props(name)
                                                                               for name in self.ifaces()})))
        else:
            names = self.ifaces()
            for i in self.modem_ids + self.messaging_ids:
                self.system.unregister_object(i)
            self.modem_ids, self.messaging_ids = [], []
            self.system.emit_signal(None, MM_PATH, 'org.freedesktop.DBus.ObjectManager', 'InterfacesRemoved',
                                    GLib.Variant('(oas)', (MODEM_PATH, names)))

    # --- the session's services and the test's controls
    def session_call(self, conn, sender, path, iface, method, params, inv):
        args = params.unpack()
        if iface == 'vibeos.Test.Modem':
            return self.control(method, args, inv)
        self.log.append([method] + [a for a in args if not isinstance(a, dict)])
        if method == 'Notify':
            app, replaces, icon, title, body, actions, hints, timeout = args
            nid = replaces if replaces in self.notifications else self.next_notification + 1
            self.next_notification = max(self.next_notification, nid)
            self.notifications[nid] = {'app': app, 'title': title, 'body': body, 'actions': actions[::2],
                                       'desktop-entry': hints.get('desktop-entry'),
                                       'no-peek': hints.get('x-vibeos-no-peek', False)}
            return inv.return_value(GLib.Variant('(u)', (nid,)))
        if method == 'CloseNotification':
            if self.notifications.pop(args[0], None) is not None:
                self.session.emit_signal(None, '/org/freedesktop/Notifications', 'org.freedesktop.Notifications',
                                         'NotificationClosed', GLib.Variant('(uu)', (args[0], 3)))
        inv.return_value(None)

    def control(self, method, args, inv):
        if method == 'Incoming':
            number, parts, timestamp = args
            parts = list(parts)
            sms = Sms(self, number, parts.pop(0), RECEIVING if parts else RECEIVED, DELIVER, timestamp)
            self.add(sms, True)
            if parts:
                GLib.timeout_add(100, self.finish, sms, parts)
            return inv.return_value(GLib.Variant('(o)', (sms.path,)))
        if method == 'StatusReport':
            sms = Sms(self, '+491701234567', '', RECEIVED, STATUS_REPORT)
            self.add(sms, True)
            return inv.return_value(GLib.Variant('(o)', (sms.path,)))
        if method == 'DeliveryReport':
            # what the network says became of a message sent with a report asked for (TP-ST as MMSmsDeliveryState)
            number, reference, state = args
            sms = Sms(self, number, '', RECEIVED, STATUS_REPORT)
            sms.reference, sms.delivery_state = reference, state
            self.add(sms, True)
            return inv.return_value(GLib.Variant('(o)', (sms.path,)))
        if method == 'IncomingBinary':
            # an MMS notification: a WAP push (WSP header, application/vnd.wap.mms-message), no text
            sms = Sms(self, args[0], '', RECEIVED, DELIVER, '2026-11-08T01:40:00+01',
                      data=bytes.fromhex('0106226170706c69636174696f6e2f766e642e7761702e6d6d732d6d65737361676500'))
            self.add(sms, True)
            return inv.return_value(GLib.Variant('(o)', (sms.path,)))
        if method == 'Take':
            # what mmsd-tng does with an MMS notification it has read: delete it off the modem
            sms = self.messages.pop(args[0], None)
            if sms:
                for i in sms.ids:
                    self.system.unregister_object(i)
                self.system.emit_signal(None, MODEM_PATH, MESSAGING, 'Deleted', GLib.Variant('(o)', (sms.path,)))
            return inv.return_value(None)
        if method == 'SetModem':
            self.set_modem(args[0])
            return inv.return_value(None)
        if method == 'SetSendError':
            self.send_error = args[0]
        elif method == 'SetMessaging':
            self.set_messaging(args[0])
        elif method == 'SetGateway':
            self.gateway = args[0]
            self.system.emit_signal(None, '/com/callagent/Gsm2sipd', 'org.freedesktop.DBus.Properties',
                                    'PropertiesChanged', GLib.Variant('(sa{sv}as)', (
                                        'com.callagent.Gsm2sipd1', {'Enabled': GLib.Variant('b', args[0])}, [])))
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
                'messages': {p: [m.state, m.pdu_type, m.number, m.text] for p, m in self.messages.items()}}),)))
        elif method == 'Clear':
            self.log = []
        inv.return_value(None)


def main():
    system = Gio.DBusConnection.new_for_address_sync(
        sys.argv[1], Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT |
        Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
    session = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    Modem(system, session, '--waiting' in sys.argv)
    # --no-app: the real app runs on the session bus and is vibeos.Messaging itself (to try the UI against it)
    app = [] if '--no-app' in sys.argv else ['vibeos.Messaging']
    names = [(system, MM), (system, GATEWAY)] + [(session, n) for n in ['org.freedesktop.Notifications'] + app +
                                                 ['vibeos.Test.Modem']]
    for conn, name in names:
        conn.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus', 'RequestName',
                       GLib.Variant('(su)', (name, 4)), None, Gio.DBusCallFlags.NONE, -1, None)
    print('ready', flush=True)
    GLib.MainLoop().run()


if __name__ == '__main__':
    main()
