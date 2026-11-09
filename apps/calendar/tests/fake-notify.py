#!/usr/bin/env python3
# fake-notify.py: a stand-in notification server for the calendar rig's private bus (try-calendar.sh reminders).
# Logs every Notify and CloseNotification, and emits ActionInvoked when its own TestInvoke method asks
# (try-calendar.sh invoke ID ACTION), as a tap on the notification or its button would.
from gi.repository import Gio, GLib
XML = '''<node><interface name="org.freedesktop.Notifications">
<method name="Notify"><arg type="s" direction="in"/><arg type="u" direction="in"/><arg type="s" direction="in"/>
<arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="as" direction="in"/><arg type="a{sv}" direction="in"/>
<arg type="i" direction="in"/><arg type="u" direction="out"/></method>
<method name="CloseNotification"><arg type="u" direction="in"/></method>
<method name="GetCapabilities"><arg type="as" direction="out"/></method>
<method name="GetServerInformation"><arg type="s" direction="out"/><arg type="s" direction="out"/><arg type="s" direction="out"/><arg type="s" direction="out"/></method>
<method name="TestInvoke"><arg type="u" direction="in"/><arg type="s" direction="in"/></method>
<signal name="ActionInvoked"><arg type="u"/><arg type="s"/></signal>
<signal name="NotificationClosed"><arg type="u"/><arg type="u"/></signal>
</interface></node>'''
nid = [0]
def call(conn, sender, path, iface, method, params, inv):
    if method == 'Notify':
        app, rep, icon, summ, body, actions, hints, timeout = params.unpack()
        i = rep or (nid[0] + 1)
        nid[0] = max(nid[0], i)
        print(f'{GLib.DateTime.new_now_local().format("%T")} Notify id={i} replaces={rep} app={app!r} summary={summ!r} body={body!r} actions={actions} hints={sorted(hints)}', flush=True)
        inv.return_value(GLib.Variant('(u)', (i,)))
    elif method == 'CloseNotification':
        print(f'{GLib.DateTime.new_now_local().format("%T")} Close id={params[0]}', flush=True)
        conn.emit_signal(None, path, iface, 'NotificationClosed', GLib.Variant('(uu)', (params[0], 3)))
        inv.return_value(None)
    elif method == 'GetCapabilities':
        inv.return_value(GLib.Variant('(as)', (['actions', 'body'],)))
    elif method == 'GetServerInformation':
        inv.return_value(GLib.Variant('(ssss)', ('fake', 'rig', '1', '1.2')))
    elif method == 'TestInvoke':
        conn.emit_signal(None, path, iface, 'ActionInvoked', GLib.Variant('(us)', params.unpack()))
        print(f'{GLib.DateTime.new_now_local().format("%T")} ActionInvoked id={params[0]} {params[1]}', flush=True)
        inv.return_value(None)
bus = Gio.bus_get_sync(Gio.BusType.SESSION)
node = Gio.DBusNodeInfo.new_for_xml(XML)
bus.register_object('/org/freedesktop/Notifications', node.interfaces[0], call, None, None)
Gio.bus_own_name_on_connection(bus, 'org.freedesktop.Notifications', 0, None, None)
GLib.MainLoop().run()
