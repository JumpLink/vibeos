#!/usr/bin/python3 -I
"""A connector that speaks the UnifiedPush D-Bus spec itself (no libvibeos), for tools/unifiedpush/e2e.sh: it owns
SERVICE, registers TOKEN with the distributor REPEAT times (each Register after the NewEndpoint of the one before;
with "burst", all at once), then unregisters, and prints every call it gets, one per line:

    REGISTER <success> [<reason>]
    NEWENDPOINT <endpoint>
    MESSAGE <id> <bytes>
    UNREGISTERED

usage: spec-client.py SERVICE TOKEN [REPEAT [burst]]
Exits 0 when each Register was followed by a NewEndpoint, the same one each time, and the Unregister by an
Unregistered."""

import sys

from gi.repository import Gio, GLib

DISTRIBUTOR = "org.unifiedpush.Distributor.vibeos"
CONNECTOR_XML = """<node><interface name='org.unifiedpush.Connector2'>
<method name='Message'><arg type='a{sv}' direction='in'/><arg type='a{sv}' direction='out'/></method>
<method name='NewEndpoint'><arg type='a{sv}' direction='in'/><arg type='a{sv}' direction='out'/></method>
<method name='Unregistered'><arg type='a{sv}' direction='in'/><arg type='a{sv}' direction='out'/></method>
</interface></node>"""

service, token = sys.argv[1], sys.argv[2]
repeat = int(sys.argv[3]) if len(sys.argv) > 3 else 2
burst = len(sys.argv) > 4 and sys.argv[4] == "burst"
endpoints = []
state = {"registers": 0, "unregistered": False, "ok": True}
loop = GLib.MainLoop()


def say(*words):
    print(*words, flush=True)


def call_distributor(connection, method, options, done):
    def finish(conn, result):
        try:
            reply = conn.call_finish(result).unpack()[0]
        except GLib.Error as error:
            say("ERROR", method, error.message)
            state["ok"] = False
            loop.quit()
            return
        done(reply)

    connection.call(DISTRIBUTOR, "/org/unifiedpush/Distributor", "org.unifiedpush.Distributor2", method,
                    GLib.Variant("(a{sv})", (options,)), GLib.VariantType("(a{sv})"), Gio.DBusCallFlags.NONE, 25000,
                    None, finish)


def register(connection):
    state["registers"] += 1

    def done(reply):
        say("REGISTER", reply.get("success"), reply.get("reason", ""))
        if reply.get("success") != "REGISTRATION_SUCCEEDED":
            state["ok"] = False
            loop.quit()

    call_distributor(connection, "Register", {"service": GLib.Variant("s", service),
                                              "token": GLib.Variant("s", token),
                                              "description": GLib.Variant("s", "Spec client")}, done)


def unregister(connection):
    call_distributor(connection, "Unregister", {"token": GLib.Variant("s", token)}, lambda reply: None)


def on_call(connection, sender, path, interface, method, parameters, invocation):
    options = parameters.unpack()[0]
    reply = {}
    if method == "Message":
        say("MESSAGE", options.get("id"), len(options.get("message", b"")))
        if "id" in options:
            reply["id"] = GLib.Variant("s", options["id"])
    invocation.return_value(GLib.Variant("(a{sv})", (reply,)))
    if options.get("token") != token:
        return
    if method == "NewEndpoint":
        say("NEWENDPOINT", options.get("endpoint"))
        endpoints.append(options.get("endpoint"))
        if burst:
            if len(endpoints) == repeat:
                unregister(connection)
            return
        if len(endpoints) < state["registers"]:
            return
        if state["registers"] < repeat:
            register(connection)
        else:
            unregister(connection)
    elif method == "Unregistered":
        say("UNREGISTERED")
        state["unregistered"] = True
        loop.quit()


def on_bus(connection, name):
    info = Gio.DBusNodeInfo.new_for_xml(CONNECTOR_XML).interfaces[0]
    connection.register_object("/org/unifiedpush/Connector", info, on_call, None, None)


def on_name(connection, name):
    for _ in range(repeat if burst else 1):
        register(connection)


def on_lost(connection, name):
    say("ERROR", "could not own", name)
    state["ok"] = False
    loop.quit()


Gio.bus_own_name(Gio.BusType.SESSION, service, Gio.BusNameOwnerFlags.NONE, on_bus, on_name, on_lost)
GLib.timeout_add_seconds(60, loop.quit)
loop.run()
ok = (state["ok"] and state["unregistered"] and len(endpoints) == repeat and len(set(endpoints)) == 1)
sys.exit(0 if ok else 1)
