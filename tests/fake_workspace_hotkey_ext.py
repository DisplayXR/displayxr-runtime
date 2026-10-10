#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
"""A stand-in for the GNOME Shell extension's org.displayxr.WorkspaceHotkey1
(window-geometry@displayxr.org version 13), for tests_service_hotkey_linux_dbus.

Run under `dbus-run-session` so it owns org.displayxr.WindowGeometry on a
PRIVATE session bus — never the desktop's. It records every Configure /
Suspend, answers Configure's `pending` as told, and exposes a control
interface (org.displayxr.Test1) the C test drives: Press() emits Activated,
SetPending(b) primes the next Configure, Reown() drops and re-takes the bus
name (an extension restart), State() returns the record as JSON.

    dbus-run-session -- python3 fake_workspace_hotkey_ext.py <test binary> [args]

Exits with the test binary's exit code.
"""

import json
import os
import sys

from gi.repository import Gio, GLib

NAME = "org.displayxr.WindowGeometry"
PATH = "/org/displayxr/WorkspaceHotkey"

XML = """
<node>
  <interface name="org.displayxr.WorkspaceHotkey1">
    <method name="Configure">
      <arg type="s" direction="in" name="accelerator"/>
      <arg type="s" direction="in" name="unit"/>
      <arg type="b" direction="out" name="pending"/>
    </method>
    <method name="Suspend">
      <arg type="b" direction="in" name="suspend"/>
      <arg type="u" direction="in" name="timeoutMs"/>
    </method>
    <method name="GetState">
      <arg type="s" direction="out" name="json"/>
    </method>
    <signal name="Activated">
      <arg type="u" name="timestamp"/>
    </signal>
  </interface>
  <interface name="org.displayxr.Test1">
    <method name="Press"/>
    <method name="SetPending">
      <arg type="b" direction="in" name="pending"/>
    </method>
    <method name="Reown"/>
    <method name="State">
      <arg type="s" direction="out" name="json"/>
    </method>
  </interface>
</node>
"""

state = {"configures": [], "suspends": [], "owner_changes": 0}
pending = [False]
conn_ref = [None]
name_id = [0]
loop = GLib.MainLoop()
exit_code = [1]


def on_call(conn, sender, path, iface, method, params, inv):
    if iface == "org.displayxr.WorkspaceHotkey1":
        if method == "Configure":
            accel, unit = params.unpack()
            state["configures"].append({"accelerator": accel, "unit": unit, "sender": sender})
            p = pending[0]
            pending[0] = False
            inv.return_value(GLib.Variant("(b)", (p,)))
        elif method == "Suspend":
            on, ms = params.unpack()
            state["suspends"].append({"suspend": on, "timeoutMs": ms})
            inv.return_value(None)
        elif method == "GetState":
            inv.return_value(GLib.Variant("(s)", (json.dumps(state),)))
        return
    if method == "Press":
        conn.emit_signal(None, PATH, "org.displayxr.WorkspaceHotkey1", "Activated", GLib.Variant("(u)", (1234,)))
        inv.return_value(None)
    elif method == "SetPending":
        pending[0] = params.unpack()[0]
        inv.return_value(None)
    elif method == "Reown":
        Gio.bus_unown_name(name_id[0])
        state["owner_changes"] += 1
        # Re-take it on the next loop turn, so the bus sees a real gap.
        GLib.timeout_add(100, lambda: (own(), False)[1])
        inv.return_value(None)
    elif method == "State":
        inv.return_value(GLib.Variant("(s)", (json.dumps(state),)))


def own():
    name_id[0] = Gio.bus_own_name_on_connection(conn_ref[0], NAME, Gio.BusNameOwnerFlags.NONE, None, None)


def main():
    if len(sys.argv) < 2:
        print("usage: fake_workspace_hotkey_ext.py <test binary> [args]", file=sys.stderr)
        return 2
    conn = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    conn_ref[0] = conn
    node = Gio.DBusNodeInfo.new_for_xml(XML)
    for iface in node.interfaces:
        conn.register_object(PATH, iface, on_call, None, None)
    own()

    # The test must never see the desktop's X server.
    os.environ.pop("DISPLAY", None)
    proc = Gio.Subprocess.new(sys.argv[1:], Gio.SubprocessFlags.NONE)

    def done(p, res):
        try:
            p.wait_finish(res)
            exit_code[0] = p.get_exit_status() if p.get_if_exited() else 1
        except GLib.Error:
            exit_code[0] = 1
        loop.quit()

    proc.wait_async(None, done)
    GLib.timeout_add_seconds(120, lambda: (loop.quit(), False)[1])
    loop.run()
    return exit_code[0]


if __name__ == "__main__":
    sys.exit(main())
