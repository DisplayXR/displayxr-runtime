// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
//
// DisplayXR Window Geometry — all of the extension's logic, in the one file
// BOTH of GJS's module systems can load (displayxr-runtime#817, #1663).
//
// GNOME Shell 45+ loads an extension as an ES module (`import … from
// 'gi://…'`, `export default class extends Extension`). GNOME 40–44 loads it
// with the legacy importer (`imports.gi`, a top-level `init()`), for which ESM
// syntax is a parse error. The two entry points are therefore separate small
// files — `extension.js` (modern) and `extension-gnome42.js` (legacy) — and
// everything they do lives HERE, once.
//
// The trick that makes one shared file possible: this file contains NO
// `import` and NO `export` statement, so it is simultaneously
//   * a valid (side-effect-only) ES module, loadable with `import './lib.js'`,
//   * a valid legacy GJS module, loadable with `Me.imports.lib`.
// It cannot *export* in either system's own idiom, so it publishes itself on
// `globalThis` instead, and takes the GI namespaces as an argument rather than
// importing them (`gi://Clutter` and `imports.gi.Clutter` are the same object,
// but only the entry point knows which spelling its shell understands).
//
//     globalThis.displayxrWindowGeometry.build({Clutter, GObject, Meta, Gio, GLib})
//         -> {WindowGeometryService}
//
// `build()` is memoised per shell process: GObject.registerClass() must run
// exactly once per GType name, and an extension that is disabled and re-enabled
// (lock screen, user toggle) goes through the entry point again.
//
// ── The D-Bus surface ───────────────────────────────────────────────────────
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
// Service   : org.displayxr.WindowGeometry          (same bus name)
// Object    : /org/displayxr/CaptureExclusion
// Interface : org.displayxr.CaptureExclusion1       (extension version 2+)
//   Method  Exclude(u pid) -> (u windows)   exclude a process's windows from
//                                           screen capture (pid 0 = caller)
//   Method  Release(u pid)                  drop the caller's exclusion
//   Method  GetState() -> (s)               JSON diagnostics
//   The exclusion is the GNOME equivalent of Windows'
//   SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE): the window keeps drawing
//   on screen but is absent from every OFF-SCREEN paint of the stage — mutter's
//   ScreenCast RecordArea, screenshots, window thumbnails taken outside a
//   frame. See docs/specs/runtime/wayland-window-geometry.md §6.
//
// Service   : org.displayxr.WindowGeometry          (same bus name)
// Object    : /org/displayxr/WindowPlacement
// Interface : org.displayxr.WindowPlacement1         (extension version 3+)
//   Method  MoveWindow(u pid, i x, i y) -> (b moved)
//   Method  MoveWindowBy(u pid, i dx, i dy) -> (b moved)      (version 6+)
//   Method  GetWindowOrigin(u pid) -> (i x, i y, b ok)        (version 6+)
//   Signal  WindowMoved(u pid, i x, i y, b applied)           (version 6+)
//   WindowMoved is the ACHIEVED frame position after every move request. A
//   client driving its own drag must close its loop on it, not on what it
//   asked for: integrating its own requests runs away the instant one is not
//   applied. GetWindowOrigin exists only alongside WindowMoved, so its presence
//   is the probe for both.
//   Moves the CALLING process's window frame to the given LOGICAL position.
//   A Wayland client cannot position itself, but a weaving window must land on
//   the display's interlace lattice or the 3D shimmers, and only the
//   compositor can put it there. The caller's PID is taken from the bus, not
//   from the argument: a client may only move its OWN windows, and the
//   argument must match (0 = "me"). Refused while an interactive grab is in
//   progress on that window — the user is still dragging it. See
//   displayxr-runtime#1609 and docs/specs/runtime/wayland-window-geometry.md §7.
//
// This extension is a SHARED asset — a vendor SDK runtime package may ship it
// to serve its own non-DisplayXR apps (one publisher, many consumers; ADR-033).
// Keep the UUID/bus/interface identifiers and the schema rules below intact:
// fields may be ADDED freely within a version, but changing the meaning of an
// existing field (e.g. logical -> physical pixels) MUST bump "version", since
// consumers refuse a version they don't understand rather than weave at a
// silently wrong phase. See docs/specs/runtime/wayland-window-geometry.md §4.
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
//       "monitor": { "x": 0, "y": 0, "w": 3840, "h": 2160, "scale": 1.0 },
//       "capture_excluded": false,          // ext v2+: CaptureExclusion1 active
//       "moving": false                     // ext v3+: an interactive grab
//                                           // (move/resize) is in progress on
//                                           // this window. A consumer that
//                                           // repositions a window waits for
//                                           // this to go false.
//     }, ...
//   ]
// }
//
// Coordinates are Mutter's global (stage) coordinates — logical pixels. At
// monitor scale 1.0 (the only mode windowed weaving supports anyway) these are
// physical desktop pixels, the same space X11's root coordinates live in. The
// runtime anchors to "buffer" (the main surface — where its pixels land) and
// falls back to "frame" (the window geometry, which includes a client-side
// title bar) only when "buffer" is absent (displayxr-runtime#1654).
//
// ── Portability rules for edits to this file ────────────────────────────────
//
// It is loaded by GNOME Shell 40 through 50, so:
//   * no `import`/`export` — that is the whole point (the CI lint in
//     scripts/check_gnome_extension.py parses this file under BOTH goals);
//   * no `this` at file scope (undefined in a module, the module object in the
//     legacy importer) — everything is inside the IIFE below;
//   * it is strict-mode code in the ESM case, so it must be strict-clean;
//   * a mutter API that arrived after 42 is reached through an existence check
//     with a working fallback (see `_queueEmit`), never assumed.

'use strict';

(function () {
    // One shared instance per shell process. A second load (the other entry
    // point, or a re-import after disable/enable) must NOT re-register the
    // GObject class, so the first one wins.
    if (globalThis.displayxrWindowGeometry)
        return;

    let built = null;

    globalThis.displayxrWindowGeometry = {
        //! gi: {Clutter, GObject, Meta, Gio, GLib} — however the caller's
        //! shell spells the import. Returns {WindowGeometryService}.
        build(gi) {
            if (!built)
                built = buildModule(gi);
            return built;
        },
    };

    function buildModule({Clutter, GObject, Meta, Gio, GLib}) {
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

        /*
         * ── Capture exclusion (extension version 2) ──────────────────────
         *
         * WHY THIS WORKS. mutter paints the stage in two situations:
         *
         *   ON-SCREEN — once per stage view per frame, from the frame clock.
         *   Every one of these paints runs between the stage's `before-paint`
         *   and `after-paint` signals (clutter-stage-view.c, the view's
         *   redraw).
         *
         *   OFF-SCREEN — clutter_stage_paint_to_framebuffer()/_to_buffer():
         *   the ScreenCast RecordArea source (it re-renders the recorded area
         *   from an idle callback, *after* the frame), Shell screenshots,
         *   window previews taken outside a frame. None of these is inside
         *   that bracket.
         *
         * So an effect on the window actor that forwards the paint while the
         * bracket is open and swallows it otherwise keeps the window on screen
         * and drops it from every off-screen paint. The discriminator is
         * TIMING rather than the paint context's framebuffer because the
         * stage-view check is not reachable from JS (the typelib exposes
         * get_framebuffer, not get_base_framebuffer); the spike cross-checked
         * the two on ~8,800 paints with zero disagreements. The shape is the
         * same on mutter 42: the area screencast still re-renders from a
         * g_idle_add outside the frame bracket.
         *
         * WHY THE WINDOW BEHIND SHOWS THROUGH (no hole). mutter culls whatever
         * an opaque window fully covers before painting. An actor with an
         * active effect is exempt from that culling (meta-cullable.c — the
         * same exemption in 42), so the windows and wallpaper behind ours are
         * still painted, and the off-screen paint, which skips ours, shows
         * them intact.
         *
         * WHAT IT DOES NOT COVER. RecordMonitor on a view that mutter can blit
         * straight from the view framebuffer is a copy of the ON-SCREEN paint,
         * so an excluded window IS present in such a stream. DisplayXR uses
         * RecordArea for exactly this reason; see
         * docs/specs/runtime/wayland-window-geometry.md §6.
         *
         * IDENTIFICATION. A client registers a PID over D-Bus; every window of
         * that PID — present now or mapped later — gets the effect, for as
         * long as the registering client stays on the bus. The extension does
         * not guess which processes are DisplayXR ones: it is a shared,
         * runtime-agnostic asset (§4), and "every window of a process that
         * happens to use a 3D panel" is not something it can know. The client
         * lifetime is the bus connection, so a crashed app cannot leave a
         * window invisible to screen capture forever.
         */

        const EXCLUDE_EFFECT_NAME = 'displayxr-exclude-from-capture';

        //! True only while the stage is painting a view on screen (see above).
        let inStagePaint = false;
        //! Diagnostics — cheap counters, surfaced by GetState().
        const excludeStats = {onscreen: 0, skipped: 0};

        const ExcludeFromCaptureEffect = GObject.registerClass(
        class DisplayXRExcludeFromCaptureEffect extends Clutter.Effect {
            // Signature is unchanged from Clutter 10 (GNOME 42) to Clutter 17
            // (GNOME 50): paint(node, paint_context, flags).
            vfunc_paint(node, paintContext, flags) {
                if (inStagePaint) {
                    excludeStats.onscreen++;
                    super.vfunc_paint(node, paintContext, flags);
                } else {
                    excludeStats.skipped++;
                }
            }
        });

        const EXCLUDE_IFACE_XML = `
<node>
  <interface name="org.displayxr.CaptureExclusion1">
    <method name="Exclude">
      <arg type="u" direction="in" name="pid"/>
      <arg type="u" direction="out" name="windows"/>
    </method>
    <method name="Release">
      <arg type="u" direction="in" name="pid"/>
    </method>
    <method name="GetState">
      <arg type="s" direction="out" name="json"/>
    </method>
  </interface>
</node>`;

        //! Protocol revision of CaptureExclusion1, reported by GetState().
        const CAPTURE_EXCLUSION_VERSION = 1;

        class CaptureExclusion {
            constructor(onChanged) {
                this._onChanged = onChanged;
                // bus unique name -> {pids: Set<number>, watchId}
                this._clients = new Map();
                this._stageSignals = [
                    global.stage.connect('before-paint', () => {
                        inStagePaint = true;
                    }),
                    global.stage.connect('after-paint', () => {
                        inStagePaint = false;
                    }),
                ];
                this._wmSignals = [
                    // 'map' fires with the actor already built and before its
                    // first on-screen frame, so a newly mapped window is never
                    // painted off-screen without the effect.
                    global.window_manager.connect('map', (_wm, actor) => this._applyActor(actor)),
                ];
                this._dbus = Gio.DBusExportedObject.wrapJSObject(EXCLUDE_IFACE_XML, this);
                this._dbus.export(Gio.DBus.session, '/org/displayxr/CaptureExclusion');
            }

            destroy() {
                for (const id of this._stageSignals)
                    global.stage.disconnect(id);
                this._stageSignals = [];
                for (const id of this._wmSignals)
                    global.window_manager.disconnect(id);
                this._wmSignals = [];
                for (const client of this._clients.values())
                    Gio.bus_unwatch_name(client.watchId);
                this._clients.clear();
                // Remove EVERY effect we added, whether or not its owner is
                // still registered: a disabled extension (lock screen, logout,
                // user toggle) must leave no window invisible to capture.
                for (const actor of global.get_window_actors())
                    actor.remove_effect_by_name(EXCLUDE_EFFECT_NAME);
                inStagePaint = false;
                if (this._dbus) {
                    this._dbus.unexport();
                    this._dbus = null;
                }
            }

            //! A window was created: its actor may not exist yet — 'map' covers that.
            onWindowCreated(win) {
                const actor = win.get_compositor_private();
                if (actor)
                    this._applyActor(actor);
            }

            isExcluded(win) {
                const actor = win.get_compositor_private();
                return !!(actor && actor.get_effect(EXCLUDE_EFFECT_NAME));
            }

            _wantedPids() {
                const pids = new Set();
                for (const client of this._clients.values())
                    for (const pid of client.pids)
                        pids.add(pid);
                return pids;
            }

            _applyActor(actor, wanted = this._wantedPids()) {
                const win = actor?.meta_window;
                if (!win)
                    return;
                const want = wanted.has(win.get_pid());
                const has = !!actor.get_effect(EXCLUDE_EFFECT_NAME);
                if (want && !has)
                    actor.add_effect_with_name(EXCLUDE_EFFECT_NAME, new ExcludeFromCaptureEffect());
                else if (!want && has)
                    actor.remove_effect_by_name(EXCLUDE_EFFECT_NAME);
            }

            _applyAll() {
                const wanted = this._wantedPids();
                for (const actor of global.get_window_actors())
                    this._applyActor(actor, wanted);
                this._onChanged?.();
            }

            _countFor(pid) {
                let n = 0;
                for (const actor of global.get_window_actors()) {
                    if (actor.meta_window?.get_pid() === pid && actor.get_effect(EXCLUDE_EFFECT_NAME))
                        n++;
                }
                return n;
            }

            _dropClient(sender) {
                const client = this._clients.get(sender);
                if (!client)
                    return;
                Gio.bus_unwatch_name(client.watchId);
                this._clients.delete(sender);
                this._applyAll();
            }

            //! Resolve the caller's PID from the bus daemon, then run cb(pid|null).
            _callerPid(sender, cb) {
                Gio.DBus.session.call(
                    'org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
                    'GetConnectionUnixProcessID', new GLib.Variant('(s)', [sender]),
                    new GLib.VariantType('(u)'), Gio.DBusCallFlags.NONE, -1, null,
                    (conn, res) => {
                        try {
                            cb(conn.call_finish(res).deepUnpack()[0]);
                        } catch (e) {
                            cb(null);
                        }
                    });
            }

            // Exclude(u pid) -> (u windows). pid 0 means "the caller". A caller
            // may only exclude its OWN process: hiding another process's
            // windows from screen recording is not something an unrelated
            // client should be able to do silently. (IPC/service mode, where
            // the window owner is not the DP's process, needs the client PID
            // plumbed — the same open item as §5.)
            ExcludeAsync(params, invocation) {
                const [pidArg] = params;
                const sender = invocation.get_sender();
                this._callerPid(sender, callerPid => {
                    if (callerPid === null) {
                        invocation.return_dbus_error('org.freedesktop.DBus.Error.Failed',
                            'could not resolve the caller PID');
                        return;
                    }
                    const pid = pidArg === 0 ? callerPid : pidArg;
                    if (pid !== callerPid) {
                        invocation.return_dbus_error('org.freedesktop.DBus.Error.AccessDenied',
                            `a client may only exclude its own windows (caller pid ${callerPid}, asked ${pid})`);
                        return;
                    }
                    if (!this._dbus) {
                        invocation.return_dbus_error('org.freedesktop.DBus.Error.Failed',
                            'capture exclusion is shutting down');
                        return;
                    }
                    let client = this._clients.get(sender);
                    if (!client) {
                        client = {pids: new Set(), watchId: 0};
                        // The registration lives exactly as long as the
                        // caller's bus connection: exit, crash or a closed
                        // private connection all release it.
                        client.watchId = Gio.bus_watch_name_on_connection(
                            Gio.DBus.session, sender, Gio.BusNameWatcherFlags.NONE,
                            null, () => this._dropClient(sender));
                        this._clients.set(sender, client);
                    }
                    client.pids.add(pid);
                    this._applyAll();
                    invocation.return_value(new GLib.Variant('(u)', [this._countFor(pid)]));
                });
            }

            ReleaseAsync(params, invocation) {
                const [pidArg] = params;
                const sender = invocation.get_sender();
                this._callerPid(sender, callerPid => {
                    const client = this._clients.get(sender);
                    if (client) {
                        const pid = pidArg === 0 ? callerPid : pidArg;
                        client.pids.delete(pid);
                        if (client.pids.size === 0)
                            this._dropClient(sender);
                        else
                            this._applyAll();
                    }
                    invocation.return_value(null);
                });
            }

            GetState() {
                const clients = [];
                for (const [sender, client] of this._clients)
                    clients.push({sender, pids: [...client.pids]});
                const windows = [];
                for (const actor of global.get_window_actors()) {
                    if (actor.get_effect(EXCLUDE_EFFECT_NAME)) {
                        const win = actor.meta_window;
                        windows.push({pid: win?.get_pid() ?? 0, title: win?.get_title() ?? ''});
                    }
                }
                return JSON.stringify({
                    version: CAPTURE_EXCLUSION_VERSION,
                    clients, windows,
                    paints: {onscreen: excludeStats.onscreen, skipped: excludeStats.skipped},
                });
            }
        }

        /*
         * ── Window placement (extension version 3) ───────────────────────
         *
         * The one thing a Wayland client cannot do for itself and the
         * compositor can: put a window at a given position. DisplayXR needs it
         * because the lenticular interlace phase is a function of the window's
         * position in panel pixels, so after a drag the window has to land on
         * the lattice the display can actually render — on X11 the client does
         * that itself during the drag.
         *
         * Deliberately NOT a general window-placement API: a caller may only
         * move a window of its own process, identified by the bus connection's
         * PID rather than by the argument it passes.
         */

        const PLACEMENT_IFACE_XML = `
<node>
  <interface name="org.displayxr.WindowPlacement1">
    <method name="MoveWindow">
      <arg type="u" direction="in" name="pid"/>
      <arg type="i" direction="in" name="x"/>
      <arg type="i" direction="in" name="y"/>
      <arg type="b" direction="out" name="moved"/>
    </method>
    <method name="MoveWindowBy">
      <arg type="u" direction="in" name="pid"/>
      <arg type="i" direction="in" name="dx"/>
      <arg type="i" direction="in" name="dy"/>
      <arg type="b" direction="out" name="moved"/>
    </method>
    <method name="GetWindowOrigin">
      <arg type="u" direction="in" name="pid"/>
      <arg type="i" direction="out" name="x"/>
      <arg type="i" direction="out" name="y"/>
      <arg type="b" direction="out" name="ok"/>
    </method>
    <signal name="WindowMoved">
      <arg type="u" name="pid"/>
      <arg type="i" name="x"/>
      <arg type="i" name="y"/>
      <arg type="b" name="applied"/>
    </signal>
  </interface>
</node>`;

        class WindowPlacement {
            constructor(getGrabbedWindow) {
                this._getGrabbedWindow = getGrabbedWindow;
                // DISPLAYXR_DEBUG=1 in the shell's environment turns on the per-move log.
                this._debug = GLib.getenv('DISPLAYXR_DEBUG') === '1';
                this._dbus = Gio.DBusExportedObject.wrapJSObject(PLACEMENT_IFACE_XML, this);
                this._dbus.export(Gio.DBus.session, '/org/displayxr/WindowPlacement');
            }

            destroy() {
                if (this._dbus) {
                    this._dbus.unexport();
                    this._dbus = null;
                }
            }

            //! The caller's PID, from the bus daemon. Async: never block the shell.
            _senderPid(sender, cb) {
                Gio.DBus.session.call(
                    'org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
                    'GetConnectionUnixProcessID', new GLib.Variant('(s)', [sender]),
                    new GLib.VariantType('(u)'), Gio.DBusCallFlags.NONE, 1000, null,
                    (conn, res) => {
                        let pid = 0;
                        try {
                            pid = conn.call_finish(res).deepUnpack()[0];
                        } catch (e) {
                            pid = 0;
                        }
                        cb(pid);
                    });
            }

            //! The caller's best window: focused first, else the largest — the
            //! same choice the runtime's geometry consumer makes.
            _windowOfPid(pid) {
                const focus = global.display.focus_window;
                let best = null;
                for (const actor of global.get_window_actors()) {
                    const win = actor.meta_window;
                    if (!win || win.get_pid() !== pid || win.get_window_type() !== Meta.WindowType.NORMAL)
                        continue;
                    if (best === null || (win === focus && best !== focus)) {
                        best = win;
                        continue;
                    }
                    if (best !== focus) {
                        const a = win.get_frame_rect(), b = best.get_frame_rect();
                        if (a.width * a.height > b.width * b.height)
                            best = win;
                    }
                }
                return best;
            }

            MoveWindowAsync([pid, x, y], invocation) {
                this._senderPid(invocation.get_sender(), senderPid => {
                    let moved = false;
                    if (senderPid > 0 && (pid === 0 || pid === senderPid)) {
                        const win = this._windowOfPid(senderPid);
                        // Never fight the user: while a grab is running on this
                        // window the position is theirs, and a move would
                        // stutter against it.
                        if (win && win !== this._getGrabbedWindow() && !win.is_fullscreen() &&
                            win.allows_move?.() !== false) {
                            win.move_frame(true, x, y);
                            moved = true;
                            const after = win.get_frame_rect();
                            this._emitMoved(senderPid, after, true);
                            if (this._debug) {
                                log(`displayxr: MoveWindow pid=${senderPid} to=(${x},${y}) ` +
                                    `after=(${after.x},${after.y})`);
                            }
                        }
                    }
                    invocation.return_value(new GLib.Variant('(b)', [moved]));
                });
            }

            _emitMoved(pid, rect, applied) {
                if (!this._dbus)
                    return;
                this._dbus.emit_signal('WindowMoved', new GLib.Variant('(uiib)',
                    [pid >>> 0, rect ? rect.x : 0, rect ? rect.y : 0, applied]));
            }

            /*
             * RELATIVE move. A client driving its own drag knows how far it
             * wants to go; the compositor knows where the window is. The
             * achieved position is always reported back (WindowMoved), and
             * every refusal names its reason under DISPLAYXR_DEBUG=1.
             */
            MoveWindowByAsync([pid, dx, dy], invocation) {
                this._senderPid(invocation.get_sender(), senderPid => {
                    let moved = false, after = null;
                    if (senderPid > 0 && (pid === 0 || pid === senderPid)) {
                        const win = this._windowOfPid(senderPid);
                        let why = 'ok';
                        if (!win)
                            why = 'no window for this pid';
                        else if (win === this._getGrabbedWindow())
                            why = 'a compositor grab op is running on it';
                        else if (win.is_fullscreen())
                            why = 'the window is fullscreen';
                        else if (win.allows_move?.() === false)
                            why = 'allows_move() is false';
                        if (why === 'ok') {
                            const before = win.get_frame_rect();
                            win.move_frame(true, before.x + dx, before.y + dy);
                            after = win.get_frame_rect();
                            moved = true;
                            if (this._debug) {
                                log(`displayxr: MoveWindowBy pid=${senderPid} d=(${dx},${dy}) ` +
                                    `before=(${before.x},${before.y}) after=(${after.x},${after.y})`);
                            }
                        } else if (this._debug) {
                            log(`displayxr: MoveWindowBy REFUSED pid=${senderPid} d=(${dx},${dy}) — ${why}`);
                        }
                    }
                    this._emitMoved(senderPid, after, moved);
                    invocation.return_value(new GLib.Variant('(b)', [moved]));
                });
            }

            //! Where the caller's own window is now (frame top-left, logical).
            GetWindowOriginAsync([pid], invocation) {
                this._senderPid(invocation.get_sender(), senderPid => {
                    let x = 0, y = 0, ok = false;
                    if (senderPid > 0 && (pid === 0 || pid === senderPid)) {
                        const win = this._windowOfPid(senderPid);
                        if (win) {
                            const r = win.get_frame_rect();
                            x = r.x;
                            y = r.y;
                            ok = true;
                        }
                    }
                    invocation.return_value(new GLib.Variant('(iib)', [x, y, ok]));
                });
            }
        }

        /*
         * ── The extension proper ─────────────────────────────────────────
         *
         * Plain object, not a subclass of Shell's `Extension`: that class does
         * not exist before GNOME 45, and nothing here needs what it provides
         * (no `this.metadata`, no `this.getSettings()`, no translations). Each
         * entry point owns one of these and forwards enable()/disable().
         */
        class WindowGeometryService {
            enable() {
                this._windowSignals = new Map(); // Meta.Window -> [handler ids]
                this._displaySignals = [];
                this._emitQueued = false;

                this._dbus = Gio.DBusExportedObject.wrapJSObject(IFACE_XML, this);
                this._dbus.export(Gio.DBus.session, '/org/displayxr/WindowGeometry');
                // Both objects are exported BEFORE the name is requested, so a
                // client that sees the name appear can always reach
                // CaptureExclusion1.
                this._captureExclusion = new CaptureExclusion(() => this._queueEmit());
                this._grabbedWindow = null;
                this._placement = new WindowPlacement(() => this._grabbedWindow);
                this._nameId = Gio.DBus.session.own_name(
                    'org.displayxr.WindowGeometry',
                    Gio.BusNameOwnerFlags.NONE, null, null);

                const display = global.display;
                this._displaySignals.push(
                    display.connect('window-created', (_d, win) => {
                        this._trackWindow(win);
                        this._captureExclusion?.onWindowCreated(win);
                        this._queueEmit();
                    }));
                this._displaySignals.push(
                    display.connect('notify::focus-window', () => this._queueEmit()));
                // Interactive grabs (move / resize). Published as `moving` so a
                // consumer that repositions a window knows to wait for the user
                // to let go. The signal's argument list grew a `screen`
                // parameter in older shells, so the window is found by type
                // rather than by position.
                const grabWindow = args => args.find(a => a instanceof Meta.Window) ?? null;
                this._displaySignals.push(
                    display.connect('grab-op-begin', (..._args) => {
                        this._grabbedWindow = grabWindow(_args);
                        this._queueEmit();
                    }));
                this._displaySignals.push(
                    display.connect('grab-op-end', (..._args) => {
                        this._grabbedWindow = null;
                        this._queueEmit();
                    }));

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

                if (this._captureExclusion) {
                    this._captureExclusion.destroy();
                    this._captureExclusion = null;
                }
                if (this._placement) {
                    this._placement.destroy();
                    this._placement = null;
                }
                this._grabbedWindow = null;

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

            // Coalesce bursts (interactive drags fire position-changed per
            // motion event) into one signal per compositor redraw via a Meta
            // later. Mutter grew the MetaLaters object in 43; before that the
            // same queue is reached through the free function. Either way the
            // callback runs BEFORE_REDRAW; with neither we emit immediately,
            // which is correct but uncoalesced.
            _queueEmit() {
                if (this._emitQueued || !this._dbus)
                    return;
                const laters = global.compositor?.get_laters?.();
                if (laters) {
                    this._emitQueued = true;
                    laters.add(Meta.LaterType.BEFORE_REDRAW, () => this._emitLater());
                    return;
                }
                if (typeof Meta.later_add === 'function') {
                    this._emitQueued = true;
                    Meta.later_add(Meta.LaterType.BEFORE_REDRAW, () => this._emitLater());
                    return;
                }
                this._emit();
            }

            _emitLater() {
                this._emitQueued = false;
                this._emit();
                return GLib.SOURCE_REMOVE;
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
                        capture_excluded: this._captureExclusion?.isExcluded(win) ?? false,
                        moving: win === this._grabbedWindow,
                    });
                }
                return JSON.stringify({version: 1, windows});
            }

            GetWindows() {
                return this._snapshotJson();
            }
        }

        return {WindowGeometryService};
    }
})();
