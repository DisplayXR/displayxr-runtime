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
//   Method  GetPlacementCapabilities() -> (u caps)            (version 6+)
//             bit 0: drag lattice (the table methods below)
//             bit 1: SetDragLatticeAt, an explicit drag start (version 8)
//   Method  SetDragLattice(u pid, b extend, i cell, i minDx, i minDy,
//                          i maxDx, i maxDy, ai dx, ai dy)
//             -> (b accepted, i startX, i startY)
//   Method  SetDragLatticeAt(u pid, i startX, i startY, b extend, i cell,
//                            i minDx, i minDy, i maxDx, i maxDy, ai dx, ai dy)
//             -> (b accepted, i startX, i startY)            (version 8)
//             Same, with the drag start (frame top-left, logical) given by the
//             caller: at a fractional scale a table is only valid for the
//             start it was built from (Mutter rounds each placement to the
//             physical pixel grid), and a table built mid-drag is built for a
//             position the window has already left.
//   Method  ClearDragLattice(u pid)
//   Signal  DragLatticeNeeded(u pid, i dx, i dy)
//   Signal  DragLatticeDone(u pid, u moves, u corrected, u misses,
//                           u maxCorrection, u tables, b landedOnTable)
//             (version 7) one summary per drag that had a table, at grab end
//   A table of displacements from the drag start that the caller found
//   phase-correct; while it is active, every compositor-proposed position of
//   the caller's window is replaced by the nearest entry (see "Drag lattice").
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
//       "lattice_drop": false,              // ext v7+: the last drag ended ON
//                                           // the drag lattice and the window
//                                           // has not moved since. A consumer
//                                           // that snaps drops leaves it alone.
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

    /*
     * ── Which table entry a drag move lands on (extension version 9) ──
     *
     * Pure logic, no GI: scripts/test_gnome_extension_lattice.js loads this
     * file under plain gjs and drives it directly.
     *
     * The table is every phase-correct position the compositor can reach, so
     * each entry is equally correct; the only freedom is WHICH one a move
     * lands on. Plain nearest (versions 6-8) treats every direction alike, so
     * along a straight drag it picks entries on either side of the drag line
     * in turn, and the window wiggles sideways (runtime#1748). The table
     * cannot be made dense enough to hide that: mutter places windows at whole
     * logical px, so above 100 % only some device px are reachable.
     *
     * So the choice spends the error where it shows least. The sideways
     * error counts PERP_WEIGHT times the error along the drag, which only
     * makes the window lead or lag the pointer a little. The lag is capped at
     * MAX_ALONG_PX; past it, and until the drag has a direction, the choice
     * is plain nearest. Everything is in DEVICE px (logical x monitor scale),
     * so it behaves the same at any output scale.
     */
    const LatticeChoice = {
        //! Sideways error weight relative to along-drag error (squared: 3x).
        PERP_WEIGHT: 9,
        //! Largest lead or lag along the drag the choice may add, device px.
        MAX_ALONG_PX: 6,
        //! How fast the drag direction follows a turn, device px of travel.
        DIR_MEMORY_PX: 8,
        //! Travel before the direction is trusted, device px (a drag from rest).
        MIN_TRAVEL_PX: 4,

        //! Per-drag state. Displacements are from the drag start, so the
        //! first move already has a direction from (0, 0).
        create() {
            return {ax: 0, ay: 0, lx: 0, ly: 0};
        },

        //! Feed the raw (compositor-proposed) displacement of each move.
        observe(st, dx, dy, scale) {
            const mx = (dx - st.lx) * scale, my = (dy - st.ly) * scale;
            const d = Math.hypot(mx, my);
            if (d > 0) {
                // The direction is an AXIS: a drag that reverses keeps it,
                // so a step against it is folded over before it is added.
                const sgn = mx * st.ax + my * st.ay < 0 ? -1 : 1;
                const decay = Math.exp(-d / LatticeChoice.DIR_MEMORY_PX);
                st.ax = st.ax * decay + sgn * mx;
                st.ay = st.ay * decay + sgn * my;
            }
            st.lx = dx;
            st.ly = dy;
        },

        /*
         * forEach(cb) calls cb(ex, ey) for each candidate entry (logical
         * displacements). Returns [ex, ey] or null. @p isotropic forces plain
         * nearest (DISPLAYXR_LATTICE_NEAREST=1, for A/B comparison).
         */
        choose(st, forEach, dx, dy, scale, isotropic = false) {
            const n = Math.hypot(st.ax, st.ay);
            const aniso = !isotropic && n >= LatticeChoice.MIN_TRAVEL_PX;
            const ux = aniso ? st.ax / n : 1, uy = aniso ? st.ay / n : 0;
            let best = null, bestC = Infinity, near = null, nearC = Infinity;
            forEach((ex, ey) => {
                const cx = (ex - dx) * scale, cy = (ey - dy) * scale;
                const d = cx * cx + cy * cy;
                if (d < nearC) {
                    nearC = d;
                    near = [ex, ey];
                }
                if (!aniso)
                    return;
                const al = cx * ux + cy * uy, pe = cy * ux - cx * uy;
                if (Math.abs(al) > LatticeChoice.MAX_ALONG_PX)
                    return;
                const c = al * al + LatticeChoice.PERP_WEIGHT * pe * pe;
                if (c < bestC) {
                    bestC = c;
                    best = [ex, ey];
                }
            });
            return best ?? near;
        },
    };

    globalThis.displayxrWindowGeometry = {
        //! gi: {Clutter, GObject, Meta, Gio, GLib} — however the caller's
        //! shell spells the import. Returns {WindowGeometryService}.
        build(gi) {
            if (!built)
                built = buildModule(gi);
            return built;
        },
        //! The drag-lattice entry choice, exported for its unit test.
        LatticeChoice,
    };

    function buildModule({Clutter, GObject, Meta, Gio, GLib, Graphene}) {
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
    <method name="GetPlacementCapabilities">
      <arg type="u" direction="out" name="caps"/>
    </method>
    <method name="SetDragLattice">
      <arg type="u" direction="in" name="pid"/>
      <arg type="b" direction="in" name="extend"/>
      <arg type="i" direction="in" name="cell"/>
      <arg type="i" direction="in" name="minDx"/>
      <arg type="i" direction="in" name="minDy"/>
      <arg type="i" direction="in" name="maxDx"/>
      <arg type="i" direction="in" name="maxDy"/>
      <arg type="ai" direction="in" name="dx"/>
      <arg type="ai" direction="in" name="dy"/>
      <arg type="b" direction="out" name="accepted"/>
      <arg type="i" direction="out" name="startX"/>
      <arg type="i" direction="out" name="startY"/>
    </method>
    <method name="SetDragLatticeAt">
      <arg type="u" direction="in" name="pid"/>
      <arg type="i" direction="in" name="startX"/>
      <arg type="i" direction="in" name="startY"/>
      <arg type="b" direction="in" name="extend"/>
      <arg type="i" direction="in" name="cell"/>
      <arg type="i" direction="in" name="minDx"/>
      <arg type="i" direction="in" name="minDy"/>
      <arg type="i" direction="in" name="maxDx"/>
      <arg type="i" direction="in" name="maxDy"/>
      <arg type="ai" direction="in" name="dx"/>
      <arg type="ai" direction="in" name="dy"/>
      <arg type="b" direction="out" name="accepted"/>
      <arg type="i" direction="out" name="startX"/>
      <arg type="i" direction="out" name="startY"/>
    </method>
    <method name="ClearDragLattice">
      <arg type="u" direction="in" name="pid"/>
    </method>

    <signal name="DragLatticeNeeded">
      <arg type="u" name="pid"/>
      <arg type="i" name="dx"/>
      <arg type="i" name="dy"/>
    </signal>
    <signal name="DragLatticeDone">
      <arg type="u" name="pid"/>
      <arg type="u" name="moves"/>
      <arg type="u" name="corrected"/>
      <arg type="u" name="misses"/>
      <arg type="u" name="maxCorrection"/>
      <arg type="u" name="tables"/>
      <arg type="b" name="landedOnTable"/>
    </signal>
  </interface>
</node>`;

        /*
         * ── Drag lattice (extension version 6) ───────────────────────────
         *
         * Keeps the interlace phase still WHILE the compositor runs a drag,
         * which the drop-time snap above cannot do: it only fixes where the
         * window comes to rest.
         *
         * The rule every platform follows — whoever decides the next position
         * snaps it before the window is SEEN there. At the press the app sends
         * a TABLE: the displacements from the drag start its own display
         * processor calls phase-correct (probed through its public snap, the
         * same oracle the X11 drag uses), restricted to positions the
         * compositor can place. mutter keeps running the drag — its feel, edge
         * tiling and workspace drag survive — and every time it moves the
         * window, `position-changed` fires SYNCHRONOUSLY inside mutter's
         * move_resize, and the handler moves the window on to the nearest
         * table entry before the stage next paints. Measured in a headless
         * mutter 50: raw (697,355) corrected to (696,356), 13 frames painted,
         * 13 on the lattice, 0 off.
         *
         * Why not Meta.ExternalConstraint, which is the proper hook: it is
         * called for every move, but GJS hands the constraint `new_rect` as a
         * COPY, and assigning the field throws "Writing field
         * Meta.ExternalConstraintInfo.new_rect is not supported". From an
         * extension it cannot move a window at all. It is kept behind
         * DISPLAYXR_LATTICE_CONSTRAINT=1 for when mutter makes the rect
         * writable; the correction then becomes unnecessary.
         *
         * What crosses the process boundary: the app's own transient answer
         * set, derived per drag. No lens pitch, slant or viewing distance —
         * the vendor retired those getters from its public API; do not
         * reintroduce them here. No call goes into the app during the grab,
         * so a busy app can never stall the desktop's move path.
         *
         * A drag that leaves the table's coverage moves unsnapped and asks for
         * more (DragLatticeNeeded, async). A table no grab follows expires.
         */
        const HAVE_EXTERNAL_CONSTRAINT =
            typeof Meta.ExternalConstraint !== 'undefined' &&
            typeof Meta.Window.prototype.add_external_constraint === 'function';
        const PLACEMENT_CAP_DRAG_LATTICE = 1;
        const PLACEMENT_CAP_EXPLICIT_START = 2;
        //! A table no grab follows stops constraining after this long.
        const LATTICE_PRE_GRAB_US = 2 * 1000 * 1000;
        //! DISPLAYXR_TEST=1 (never in production): tables audit their paints
        //! from the moment they land and wait longer for a grab to start. (A
        //! virtual-pointer grab driver crashed a headless mutter 50 inside
        //! begin_grab_op and was removed; do not reintroduce it.)
        const LATTICE_TEST = GLib.getenv('DISPLAYXR_TEST') === '1';
        //! DISPLAYXR_LATTICE_CONSTRAINT=1: register the Meta.ExternalConstraint
        //! as well. It CANNOT move the window from GJS on mutter 50 (the rect is
        //! a read-only copy — see "Drag lattice"), so it is off by default and
        //! kept only for when mutter makes the rect writable.
        const LATTICE_USE_CONSTRAINT = GLib.getenv('DISPLAYXR_LATTICE_CONSTRAINT') === '1';
        //! DISPLAYXR_LATTICE_NEAREST=1: land each move on the plain nearest
        //! entry, as versions 6-8 did, instead of LatticeChoice's
        //! direction-aware pick. For A/B comparison on a panel only.
        const LATTICE_NEAREST = GLib.getenv('DISPLAYXR_LATTICE_NEAREST') === '1';

        let DragLatticeConstraint = null;
        if (HAVE_EXTERNAL_CONSTRAINT) {
            DragLatticeConstraint = GObject.registerClass({
                Implements: [Meta.ExternalConstraint],
            }, class DisplayXRDragLatticeConstraint extends GObject.Object {
                vfunc_constrain(window, info) {
                    return this._lattice ? this._lattice.constrain(window, info) : true;
                }
            });
        }

        class DragLattice {
            constructor(emitNeeded, emitDone, isGrabbed, debug) {
                this._emitNeeded = emitNeeded;
                this._emitDone = emitDone;
                this._isGrabbed = isGrabbed; // (win) -> is a grab running on it NOW
                this._debug = debug;
                this._landed = new Map();       // Meta.Window -> [x, y] of an on-table drop
                this._tables = new Map();      // Meta.Window -> table
                this._constraints = new Map(); // Meta.Window -> constraint object
                this._posHandlers = new Map(); // Meta.Window -> position-changed handler id
                this._correcting = false;       // re-entry guard for our own move_frame
                this._stats = {constrained: 0, snapped: 0, misses: 0, propagate: 'untested'};
                this._resetDragStats();
            }

            _resetDragStats() {
                this._drag = {corrected: 0, moves: 0, paintedOn: 0, paintedOff: 0, offSamples: [],
                    maxCorrection: 0, misses: 0, tables: 0};
            }

            supported() {
                // The correction path moves the window with move_frame and needs
                // nothing newer than the placement service itself.
                return true;
            }

            /*
             * THE CORRECTION (what actually moves the window on mutter 50).
             *
             * mutter's move grab places the window from the pointer's
             * displacement against the grab anchor, not from where the window
             * currently is. So correcting each placement to the nearest table
             * entry does not fight the grab: the next pointer event simply
             * re-proposes the raw position and it is corrected again. The one
             * question is ORDER — whether the stage can paint the raw position
             * before the correction lands — and that is what auditPaint()
             * measures instead of assuming.
             */
            _onPositionChanged(win) {
                if (this._correcting)
                    return; // our own corrective move
                const t = this._tables.get(win);
                if (!t)
                    return;
                if (!t.grabbing) {
                    /*
                     * Correct ONLY what the user's drag proposes. A table that
                     * is waiting for its grab must not touch any other move — in
                     * particular the runtime's drop-time snap (MoveWindow),
                     * which it would pull 2 px off towards the table's own
                     * origin while the snap pulled it back (#1609 follow-up).
                     */
                    if (GLib.get_monotonic_time() > t.expiresUs)
                        this._tables.delete(win);
                    return;
                }
                this._drag.moves++;
                const r = win.get_frame_rect();
                const dx = r.x - t.startX, dy = r.y - t.startY;
                const inside = dx >= t.minDx && dx <= t.maxDx && dy >= t.minDy && dy <= t.maxDy;
                // Device px per logical px where the window is now: the
                // choice weighs its errors in device px (LatticeChoice).
                const mon = win.get_monitor();
                const scale = mon >= 0 ? win.get_display().get_monitor_scale(mon) : 1;
                LatticeChoice.observe(t.choice, dx, dy, scale);
                const best = inside
                    ? LatticeChoice.choose(t.choice, this._candidates(t, dx, dy), dx, dy, scale,
                        LATTICE_NEAREST)
                    : null;
                if (!best) {
                    this._stats.misses++;
                    this._drag.misses++;
                    if (!t.asked) {
                        t.asked = true;
                        const pid = t.pid;
                        GLib.idle_add(GLib.PRIORITY_DEFAULT, () => {
                            this._emitNeeded(pid, dx, dy);
                            return GLib.SOURCE_REMOVE;
                        });
                    }
                    return;
                }
                this._askAhead(t, dx, dy);
                const nx = t.startX + best[0], ny = t.startY + best[1];
                if (nx === r.x && ny === r.y)
                    return;
                const c = Math.max(Math.abs(nx - r.x), Math.abs(ny - r.y));
                if (c > this._drag.maxCorrection)
                    this._drag.maxCorrection = c;
                this._correcting = true;
                try {
                    win.move_frame(true, nx, ny);
                } finally {
                    this._correcting = false;
                }
                this._drag.corrected++;
                if (this._debug && this._drag.corrected <= 4) {
                    const a = win.get_frame_rect();
                    log(`displayxr: corrected raw=(${r.x},${r.y}) -> (${nx},${ny}); frame now (${a.x},${a.y})`);
                }
            }

            /*
             * Ask for the next piece EARLY and AHEAD (#1609 follow-up). Waiting
             * for a miss meant every long drag ran unconstrained at the table's
             * edge for as long as the app took to probe the next one (~100 ms
             * against a real display processor), which showed as an occasional
             * stutter. Ask once the drag is half-way from the table's centre to
             * its edge, centred where the window will be ~150 ms from now.
             */
            _askAhead(t, dx, dy) {
                const now = GLib.get_monotonic_time();
                let vx = 0, vy = 0;
                if (t.lastUs !== undefined && now > t.lastUs) {
                    const dt = (now - t.lastUs) / 1e6;
                    vx = (dx - t.lastDx) / dt;
                    vy = (dy - t.lastDy) / dt;
                }
                t.lastDx = dx;
                t.lastDy = dy;
                t.lastUs = now;
                if (t.asked)
                    return;
                const [pMinX, pMinY, pMaxX, pMaxY] = t.piece ?? [t.minDx, t.minDy, t.maxDx, t.maxDy];
                const cx = (pMinX + pMaxX) / 2, cy = (pMinY + pMaxY) / 2;
                const half = Math.min(pMaxX - pMinX, pMaxY - pMinY) / 2;
                if (Math.max(Math.abs(dx - cx), Math.abs(dy - cy)) < half / 2)
                    return;
                const lim = half / 2;
                const lead = v => Math.max(-lim, Math.min(lim, v * 0.15));
                const ax = Math.round(dx + lead(vx)), ay = Math.round(dy + lead(vy));
                t.asked = true;
                this._drag.asksAhead = (this._drag.asksAhead ?? 0) + 1;
                const pid = t.pid;
                GLib.idle_add(GLib.PRIORITY_DEFAULT, () => {
                    this._emitNeeded(pid, ax, ay);
                    return GLib.SOURCE_REMOVE;
                });
            }

            /*
             * What the stage actually PAINTED, per frame, during a drag: the
             * window actor's position (which is what is drawn), converted to the
             * frame origin and checked against the table. A raw, uncorrected
             * position reaching the screen shows up here as paintedOff.
             */
            auditPaint() {
                for (const [win, t] of this._tables) {
                    if (!t.grabbing)
                        continue;
                    const actor = win.get_compositor_private();
                    if (!actor)
                        continue;
                    const [ax, ay] = actor.get_position();
                    const f = win.get_frame_rect(), b = win.get_buffer_rect();
                    const px = Math.round(ax + (f.x - b.x)), py = Math.round(ay + (f.y - b.y));
                    const key = `${px - t.startX},${py - t.startY}`;
                    if (t.members.has(key)) {
                        this._drag.paintedOn++;
                    } else {
                        this._drag.paintedOff++;
                        if (this._drag.offSamples.length < 5)
                            this._drag.offSamples.push(`(${px - t.startX},${py - t.startY})`);
                    }
                }
            }

            //! Is a drag with a table in progress (so the audit is worth running)?
            anyGrabbing() {
                for (const t of this._tables.values()) {
                    if (t.grabbing)
                        return true;
                }
                return false;
            }

            set(win, pid, extend, cell, bounds, dxs, dys, explicitStart = null) {
                // The correction path needs no Meta.ExternalConstraint (it moves
                // the window with move_frame), so it works on any shell.
                if (!win || dxs.length !== dys.length || cell < 1)
                    return false;
                const prev = this._tables.get(win);
                // A replacement (extend) keeps the drag's origin — the
                // displacements in every table of one drag are relative to the
                // same start. A fresh table takes the window's position NOW,
                // before the grab moves it.
                let startX, startY;
                if (explicitStart) {
                    // The caller built the table for this start (v8).
                    [startX, startY] = explicitStart;
                } else if (extend && prev) {
                    startX = prev.startX;
                    startY = prev.startY;
                } else {
                    const r = win.get_frame_rect();
                    startX = r.x;
                    startY = r.y;
                }
                // An extension of the same drag ADDS to the table: every entry
                // of both is phase-correct for the same start, and replacing
                // would drop the entry the window is sitting on (the new
                // table samples its cells from a different grid origin).
                const merge = extend && prev && prev.startX === startX && prev.startY === startY;
                const buckets = merge ? prev.buckets : new Map();
                for (let i = 0; i < dxs.length; i++) {
                    const key = `${Math.floor(dxs[i] / cell)},${Math.floor(dys[i] / cell)}`;
                    let b = buckets.get(key);
                    if (!b)
                        buckets.set(key, b = []);
                    b.push(dxs[i], dys[i]);
                }
                // The newest piece's own extent: what "half-way to the edge"
                // is measured against (the union below has uncovered corners).
                const piece = bounds.slice();
                const members = merge ? prev.members : new Set();
                for (let i = 0; i < dxs.length; i++)
                    members.add(`${dxs[i]},${dys[i]}`);
                if (merge) {
                    bounds = [Math.min(bounds[0], prev.minDx), Math.min(bounds[1], prev.minDy),
                        Math.max(bounds[2], prev.maxDx), Math.max(bounds[3], prev.maxDy)];
                }
                this._tables.set(win, {
                    pid, startX, startY, cell, buckets, members,
                    minDx: bounds[0], minDy: bounds[1], maxDx: bounds[2], maxDy: bounds[3],
                    asked: false,
                    // A table can arrive MID-grab: the app sends one when its
                    // window enters the 3D panel during a drag that began on
                    // another monitor. Its start is the window's position now,
                    // and it constrains from the next move on.
                    grabbing: (prev?.grabbing ?? false) || this._isGrabbed(win),
                    expiresUs: GLib.get_monotonic_time() +
                        (LATTICE_TEST ? 120 * 1000 * 1000 : LATTICE_PRE_GRAB_US),
                    entries: dxs.length,
                    piece,
                    // The drag direction survives an extension of the same
                    // drag; a new start begins from rest.
                    choice: merge && prev.choice ? prev.choice : LatticeChoice.create(),
                });
                if (LATTICE_TEST) {
                    // Test mode: audit paints from the moment the table lands,
                    // so a programmatic move (a MOVE through the same
                    // move_resize path a grab uses) is measured without any
                    // virtual input, and report after a second.
                    this._tables.get(win).grabbing = true;
                    GLib.timeout_add(GLib.PRIORITY_DEFAULT, 1500, () => {
                        const d = this._drag;
                        log(`displayxr: TEST audit — ${d.moves} move(s), ${d.corrected} corrected ` +
                            `(largest ${d.maxCorrection} px); PAINTED on-lattice ${d.paintedOn}, ` +
                            `OFF-lattice ${d.paintedOff}` +
                            `${d.offSamples.length ? ` e.g. ${d.offSamples.join(' ')}` : ''}`);
                        return GLib.SOURCE_REMOVE;
                    });
                }
                // A fresh table outside a grab starts a new drag's statistics.
                if (!extend && !this._isGrabbed(win))
                    this._resetDragStats();
                this._drag.tables++;
                this._landed.delete(win);
                if (!this._posHandlers.has(win)) {
                    this._posHandlers.set(win,
                        win.connect('position-changed', () => this._onPositionChanged(win)));
                }
                if (LATTICE_USE_CONSTRAINT && HAVE_EXTERNAL_CONSTRAINT && !this._constraints.has(win)) {
                    const c = new DragLatticeConstraint();
                    c._lattice = this;
                    win.add_external_constraint(c);
                    this._constraints.set(win, c);
                }
                if (this._debug) {
                    log(`displayxr: SetDragLattice pid=${pid} extend=${extend} entries=${dxs.length} ` +
                        `cell=${cell} bounds=[${bounds}] start=(${startX},${startY})`);
                }
                return true;
            }

            clear(win) {
                this._tables.delete(win);
            }

            startOf(win) {
                const t = this._tables.get(win);
                return t ? [t.startX, t.startY] : null;
            }

            onGrabBegin(win) {
                if (win)
                    this._landed.delete(win);
                const t = win && this._tables.get(win);
                if (t)
                    t.grabbing = true; // no expiry while the user holds it
            }

            //! Did this window's last drag end on its table, and has it stayed there?
            landedOnTable(win) {
                const l = this._landed.get(win);
                if (!l)
                    return false;
                const r = win.get_frame_rect();
                return r.x === l[0] && r.y === l[1];
            }

            onGrabEnd(win) {
                if (win && this._tables.has(win)) {
                    const t = this._tables.get(win);
                    const r = win.get_frame_rect();
                    const onTable = t.members.has(`${r.x - t.startX},${r.y - t.startY}`);
                    if (onTable)
                        this._landed.set(win, [r.x, r.y]);
                    const d = this._drag;
                    this._emitDone(t.pid, d.moves, d.corrected, d.misses, d.maxCorrection, d.tables, onTable);
                    if (this._debug) {
                        const d = this._drag;
                        log(`displayxr: drag lattice done — ${d.moves} compositor move(s), ${d.corrected} ` +
                            `corrected (largest ${d.maxCorrection} logical px), ${d.misses} outside ` +
                            `coverage; PAINTED on-lattice ${d.paintedOn} frame(s), OFF-lattice ` +
                            `${d.paintedOff}${d.offSamples.length ? ` e.g. ${d.offSamples.join(' ')}` : ''}`);
                    }
                    this._tables.delete(win);
                    this._resetDragStats();
                }
            }

            forget(win) {
                this._tables.delete(win);
                this._landed.delete(win);
                const h = this._posHandlers.get(win);
                if (h) {
                    try {
                        win.disconnect(h);
                    } catch (e) {
                        // the window is already gone
                    }
                    this._posHandlers.delete(win);
                }
                const c = this._constraints.get(win);
                if (c) {
                    c._lattice = null;
                    try {
                        win.remove_external_constraint(c);
                    } catch (e) {
                        // the window is already gone
                    }
                    this._constraints.delete(win);
                }
            }

            destroy() {
                for (const win of new Set([...this._constraints.keys(), ...this._posHandlers.keys()]))
                    this.forget(win);
            }

            //! The window a test drag should act on: the one holding a table.
            anyTableWindow() {
                for (const win of this._tables.keys())
                    return win;
                return null;
            }

            //! The entries a move to (dx, dy) may land on: the 5 x 5 buckets
            //! around it, as a forEach for LatticeChoice.
            _candidates(t, dx, dy) {
                const bx = Math.floor(dx / t.cell), by = Math.floor(dy / t.cell);
                return cb => {
                    for (let j = -2; j <= 2; j++) {
                        for (let i = -2; i <= 2; i++) {
                            const b = t.buckets.get(`${bx + i},${by + j}`);
                            if (!b)
                                continue;
                            for (let k = 0; k < b.length; k += 2)
                                cb(b[k], b[k + 1]);
                        }
                    }
                };
            }

            _nearest(t, dx, dy) {
                let best = null, bestD = Infinity;
                this._candidates(t, dx, dy)((x, y) => {
                    const d = (x - dx) * (x - dx) + (y - dy) * (y - dy);
                    if (d < bestD) {
                        bestD = d;
                        best = [x, y];
                    }
                });
                return best;
            }

            constrain(window, info) {
                // Diagnostics: DISPLAYXR_LATTICE_NOOP=1 registers the constraint
                // but never touches the rect, to separate what mutter does on its
                // own from what this constraint does.
                if (this._noop === undefined)
                    this._noop = GLib.getenv('DISPLAYXR_LATTICE_NOOP') === '1';
                if (this._noop)
                    return true;
                const t = this._tables.get(window);
                if (!t || !(info.flags & Meta.ExternalConstraintFlags.MOVE))
                    return true;
                if (!t.grabbing && GLib.get_monotonic_time() > t.expiresUs) {
                    this._tables.delete(window); // nobody dragged: stop constraining
                    return true;
                }
                this._stats.constrained++;
                const r = info.new_rect;
                const dx = r.x - t.startX, dy = r.y - t.startY;
                const inside = dx >= t.minDx && dx <= t.maxDx && dy >= t.minDy && dy <= t.maxDy;
                const best = inside ? this._nearest(t, dx, dy) : null;
                if (!best) {
                    // Outside what we were told: move unsnapped rather than
                    // clamp the user's drag, and ask for more — once, async, and
                    // never from inside mutter's constraint pass.
                    this._stats.misses++;
                    if (!t.asked) {
                        t.asked = true;
                        const pid = t.pid;
                        GLib.idle_add(GLib.PRIORITY_DEFAULT, () => {
                            this._emitNeeded(pid, dx, dy);
                            return GLib.SOURCE_REMOVE;
                        });
                    }
                    return true;
                }
                const nx = t.startX + best[0], ny = t.startY + best[1];
                if (this._debug && this._stats.constrained <= 5) {
                    log(`displayxr: constrain proposed=(${r.x},${r.y}) d=(${dx},${dy}) ` +
                        `nearest=(${best[0]},${best[1]}) -> (${nx},${ny})`);
                }
                if (nx !== r.x || ny !== r.y) {
                    this._stats.snapped++;
                    // GJS may hand us the rect by value; write through the
                    // pointer if it did not alias, and record which it was so a
                    // failure to take effect is diagnosable rather than silent.
                    r.x = nx;
                    r.y = ny;
                    if (this._debug && this._stats.constrained <= 5) {
                        log(`displayxr: constrain wrote (${r.x},${r.y}); info.new_rect now ` +
                            `(${info.new_rect.x},${info.new_rect.y})`);
                    }
                    if (info.new_rect.x !== nx || info.new_rect.y !== ny) {
                        try {
                            info.new_rect = r;
                            this._stats.propagate = 'needed assignment';
                            if (this._debug && this._stats.constrained <= 5) {
                                log(`displayxr: constrain assigned; info.new_rect now ` +
                                    `(${info.new_rect.x},${info.new_rect.y}) flags=${info.flags} ` +
                                    `gravity=${info.resize_gravity}`);
                            }
                        } catch (e) {
                            this._stats.propagate = `DID NOT PROPAGATE (${e})`;
                            if (this._debug)
                                log(`displayxr: constrain assignment THREW: ${e}`);
                        }
                    } else if (this._stats.propagate === 'untested') {
                        this._stats.propagate = 'propagate in place';
                    }
                }
                return true;
            }
        }

        class WindowPlacement {
            constructor(getGrabbedWindow) {
                this._getGrabbedWindow = getGrabbedWindow;
                // DISPLAYXR_DEBUG=1 in the shell's environment: per-drag lattice
                // lines in the journal.
                this._debug = GLib.getenv('DISPLAYXR_DEBUG') === '1';
                this._lattice = new DragLattice(
                    (pid, dx, dy) => this._emitNeeded(pid, dx, dy),
                    (...a) => this._emitDone(...a),
                    win => win !== null && win === this._getGrabbedWindow(),
                    this._debug);
                this._dbus = Gio.DBusExportedObject.wrapJSObject(PLACEMENT_IFACE_XML, this);
                this._dbus.export(Gio.DBus.session, '/org/displayxr/WindowPlacement');
            }

            //! Grab lifecycle, forwarded by the service (it owns the display
            //! signals).
            onGrabBegin(win) {
                this._lattice.onGrabBegin(win);
            }

            onGrabEnd(win) {
                this._lattice.onGrabEnd(win);
            }

            onWindowUnmanaged(win) {
                this._lattice.forget(win);
            }

            _emitNeeded(pid, dx, dy) {
                if (this._dbus) {
                    this._dbus.emit_signal('DragLatticeNeeded',
                        new GLib.Variant('(uii)', [pid >>> 0, Math.round(dx), Math.round(dy)]));
                }
            }

            _emitDone(pid, moves, corrected, misses, maxCorrection, tables, landed) {
                if (this._dbus) {
                    this._dbus.emit_signal('DragLatticeDone', new GLib.Variant('(uuuuuub)',
                        [pid >>> 0, moves >>> 0, corrected >>> 0, misses >>> 0, maxCorrection >>> 0,
                            tables >>> 0, landed]));
                }
            }

            //! For the published snapshot: see "lattice_drop".
            latticeDrop(win) {
                return this._lattice?.landedOnTable(win) ?? false;
            }

            //! Per-frame paint audit, forwarded by the service's stage hook.
            auditPaint() {
                if (this._lattice?.anyGrabbing())
                    this._lattice.auditPaint();
            }

            GetPlacementCapabilities() {
                return this._lattice.supported()
                    ? PLACEMENT_CAP_DRAG_LATTICE | PLACEMENT_CAP_EXPLICIT_START : 0;
            }

            SetDragLatticeAsync([pid, extend, cell, minDx, minDy, maxDx, maxDy, dxs, dys], invocation) {
                this._senderPid(invocation.get_sender(), senderPid => {
                    let ok = false, sx = 0, sy = 0;
                    if (senderPid > 0 && (pid === 0 || pid === senderPid)) {
                        const win = this._windowOfPid(senderPid);
                        ok = this._lattice.set(win, senderPid, extend, cell,
                            [minDx, minDy, maxDx, maxDy], dxs, dys);
                        const start = ok ? this._lattice.startOf(win) : null;
                        if (start)
                            [sx, sy] = start;
                    }
                    // The drag's origin as recorded, so the caller can log and
                    // test against it without ever learning its position any
                    // other way.
                    invocation.return_value(new GLib.Variant('(bii)', [ok, sx, sy]));
                });
            }

            SetDragLatticeAtAsync([pid, startX, startY, extend, cell, minDx, minDy, maxDx, maxDy, dxs, dys],
                invocation) {
                this._senderPid(invocation.get_sender(), senderPid => {
                    let ok = false, sx = 0, sy = 0;
                    if (senderPid > 0 && (pid === 0 || pid === senderPid)) {
                        const win = this._windowOfPid(senderPid);
                        ok = this._lattice.set(win, senderPid, extend, cell,
                            [minDx, minDy, maxDx, maxDy], dxs, dys, [startX, startY]);
                        const start = ok ? this._lattice.startOf(win) : null;
                        if (start)
                            [sx, sy] = start;
                    }
                    invocation.return_value(new GLib.Variant('(bii)', [ok, sx, sy]));
                });
            }

            ClearDragLatticeAsync([pid], invocation) {
                this._senderPid(invocation.get_sender(), senderPid => {
                    if (senderPid > 0 && (pid === 0 || pid === senderPid)) {
                        const win = this._windowOfPid(senderPid);
                        if (win)
                            this._lattice.clear(win);
                    }
                    invocation.return_value(null);
                });
            }

            destroy() {
                this._lattice?.destroy();
                this._lattice = null;
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
                        }
                    }
                    invocation.return_value(new GLib.Variant('(b)', [moved]));
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
                        this._placement?.onGrabBegin(this._grabbedWindow);
                        this._queueEmit();
                    }));
                this._displaySignals.push(
                    display.connect('grab-op-end', (..._args) => {
                        this._placement?.onGrabEnd(grabWindow(_args) ?? this._grabbedWindow);
                        this._grabbedWindow = null;
                        this._queueEmit();
                    }));

                for (const actor of global.get_window_actors())
                    this._trackWindow(actor.meta_window);

                // Drag-lattice paint audit (debug builds of a drag only): what
                // each frame actually shows, checked against the table. Cheap —
                // it returns at once unless a drag with a table is in progress.
                if (GLib.getenv('DISPLAYXR_DEBUG') === '1') {
                    this._auditId = global.stage.connect('after-paint',
                        () => this._placement?.auditPaint());
                }
            }

            disable() {
                if (this._auditId) {
                    global.stage.disconnect(this._auditId);
                    this._auditId = 0;
                }
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
                        this._placement?.onWindowUnmanaged(win);
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
                        lattice_drop: this._placement?.latticeDrop(win) ?? false,
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
