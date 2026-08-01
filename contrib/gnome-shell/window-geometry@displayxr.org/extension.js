// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
//
// DisplayXR Window Geometry — GNOME Shell extension (displayxr-runtime#817).
//
// Wayland never tells a client where its surface sits on the desktop, but the
// lenticular interlacing phase depends on exactly that (windowed weaving).
// Mutter knows every window's global geometry authoritatively, so this
// extension publishes it over the session bus for the DisplayXR runtime's
// comp_vk_native_wl_geom provider to consume.
//
// Service   : org.displayxr.WindowGeometry
// Object    : /org/displayxr/WindowGeometry
// Interface : org.displayxr.WindowGeometry1
//   Method  GetWindows() -> (s)   JSON snapshot (schema below)
//   Signal  WindowsChanged(s)     same JSON, emitted on any geometry change
//
// JSON schema (version 1):
// {
//   "version": 1,
//   "windows": [
//     {
//       "pid": 1234,
//       "app_id": "cube_handle_vk_linux",   // wm_class / Wayland app-id
//       "title": "...",
//       "focus": true,
//       "xwayland": false,
//       "frame":  [x, y, w, h],             // Meta.Window.get_frame_rect()
//       "buffer": [x, y, w, h],             // Meta.Window.get_buffer_rect()
//       "monitor": { "x": 0, "y": 0, "w": 3840, "h": 2160, "scale": 1.0 }
//     }, ...
//   ]
// }
//
// Coordinates are Mutter's global (stage) coordinates — logical pixels. At
// monitor scale 1.0 (the only mode windowed weaving supports anyway) these are
// physical desktop pixels, the same space X11's root coordinates live in. The
// runtime reads "frame" by default; "buffer" is published so validation can
// decide how CSD shadow margins should be handled.

import Meta from 'gi://Meta';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const IFACE_XML = `
<node>
  <interface name="org.displayxr.WindowGeometry1">
    <method name="GetWindows">
      <arg type="s" direction="out" name="json"/>
    </method>
    <signal name="WindowsChanged">
      <arg type="s" name="json"/>
    </signal>
  </interface>
</node>`;

export default class WindowGeometryExtension extends Extension {
    enable() {
        this._windowSignals = new Map(); // Meta.Window -> [handler ids]
        this._displaySignals = [];
        this._emitQueued = false;

        this._dbus = Gio.DBusExportedObject.wrapJSObject(IFACE_XML, this);
        this._dbus.export(Gio.DBus.session, '/org/displayxr/WindowGeometry');
        this._nameId = Gio.DBus.session.own_name(
            'org.displayxr.WindowGeometry',
            Gio.BusNameOwnerFlags.NONE, null, null);

        const display = global.display;
        this._displaySignals.push(
            display.connect('window-created', (_d, win) => {
                this._trackWindow(win);
                this._queueEmit();
            }));
        this._displaySignals.push(
            display.connect('notify::focus-window', () => this._queueEmit()));

        for (const actor of global.get_window_actors())
            this._trackWindow(actor.meta_window);
    }

    disable() {
        for (const [win, ids] of this._windowSignals)
            for (const id of ids)
                win.disconnect(id);
        this._windowSignals.clear();
        for (const id of this._displaySignals)
            global.display.disconnect(id);
        this._displaySignals = [];

        if (this._nameId) {
            Gio.DBus.session.unown_name(this._nameId);
            this._nameId = 0;
        }
        if (this._dbus) {
            this._dbus.unexport();
            this._dbus = null;
        }
    }

    _trackWindow(win) {
        if (this._windowSignals.has(win))
            return;
        const ids = [
            win.connect('position-changed', () => this._queueEmit()),
            win.connect('size-changed', () => this._queueEmit()),
            win.connect('unmanaged', () => {
                const old = this._windowSignals.get(win);
                if (old) {
                    for (const id of old)
                        win.disconnect(id);
                    this._windowSignals.delete(win);
                }
                this._queueEmit();
            }),
        ];
        this._windowSignals.set(win, ids);
    }

    // Coalesce bursts (interactive drags fire position-changed per motion
    // event) into one signal per compositor redraw via a Meta later.
    _queueEmit() {
        if (this._emitQueued || !this._dbus)
            return;
        const laters = global.compositor?.get_laters?.();
        if (!laters) {
            this._emit();
            return;
        }
        this._emitQueued = true;
        laters.add(Meta.LaterType.BEFORE_REDRAW, () => {
            this._emitQueued = false;
            this._emit();
            return GLib.SOURCE_REMOVE;
        });
    }

    _emit() {
        if (this._dbus) {
            this._dbus.emit_signal(
                'WindowsChanged',
                new GLib.Variant('(s)', [this._snapshotJson()]));
        }
    }

    _snapshotJson() {
        const display = global.display;
        const focus = display.focus_window;
        const windows = [];
        for (const actor of global.get_window_actors()) {
            const win = actor.meta_window;
            if (!win || win.get_window_type() !== Meta.WindowType.NORMAL)
                continue;
            const frame = win.get_frame_rect();
            const buffer = win.get_buffer_rect();
            const mon = win.get_monitor();
            let monitor = null;
            if (mon >= 0) {
                const g = display.get_monitor_geometry(mon);
                monitor = {
                    x: g.x, y: g.y, w: g.width, h: g.height,
                    scale: display.get_monitor_scale(mon),
                };
            }
            windows.push({
                pid: win.get_pid(),
                app_id: win.get_wm_class() ?? '',
                title: win.get_title() ?? '',
                focus: win === focus,
                xwayland: win.get_client_type() === Meta.WindowClientType.X11,
                frame: [frame.x, frame.y, frame.width, frame.height],
                buffer: [buffer.x, buffer.y, buffer.width, buffer.height],
                monitor,
            });
        }
        return JSON.stringify({version: 1, windows});
    }

    GetWindows() {
        return this._snapshotJson();
    }
}
