// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
//
// DisplayXR window geometry for a STOCK KWin (KDE Plasma 6; SteamOS Desktop
// Mode). docs/specs/runtime/wayland-window-geometry.md, "KWin script bridge".
//
// The canonical publisher of org.displayxr.WindowGeometry1 is the compositor
// itself (the GNOME Shell extension; for KWin, the native plug-in tracked in
// displayxr-runtime#1928). Neither can be installed on a read-only OS image
// running Valve's KWin, so this is the stopgap a user can install in
// ~/.local/share/kwin/scripts: a KWin script cannot own a D-Bus name, only
// call one, so it PUSHES each snapshot to displayxr-kwin-geometry-bridge,
// which owns org.displayxr.WindowGeometry and serves GetWindows /
// WindowsChanged exactly like the extension. If a real publisher owns the
// name, the bridge never starts and these calls simply fail.
//
// Geometry only, schema version 1: pid, app_id, focus, xwayland, frame,
// buffer, monitor {x, y, w, h, scale, device_scale}, moving, minimized,
// above, fullscreen. KWin's layout is always logical with a fractional scale
// per output, so layout_mode is "logical" and device_scale is the output's
// devicePixelRatio. Deliberately NOT sent:
//   - title: the extension gives titles only to callers it authorises; a
//     bridge cannot tell callers apart that way, so none get them.
//   - buffer_device: KWin keeps positions fractional and snaps them to the
//     device grid when painting; a script cannot read the painted rect, and
//     the spec says to leave the field out rather than guess (#1991).
//   - placement, drag lattice, move sync, capture exclusion: need the
//     compositor's internals; the native plug-in (#1928) carries them.

const BUS = "org.displayxr.WindowGeometry";
const PATH = "/org/displayxr/WindowGeometry";
const FEED = "org.displayxr.WindowGeometryFeed1";

let lastJson = "";

function rect(r) {
    // KWin keeps window positions fractional (after a pointer drag); the
    // wire is integer logical px, as from Mutter. Round, never truncate.
    return [Math.round(r.x), Math.round(r.y), Math.round(r.width), Math.round(r.height)];
}

function monitorOf(w) {
    const o = w.output;
    if (!o) {
        return null; // on no output: the consumer refuses the rect (spec §3)
    }
    const g = o.geometry;
    const s = o.devicePixelRatio > 0 ? o.devicePixelRatio : 1.0;
    return {x: Math.round(g.x), y: Math.round(g.y), w: Math.round(g.width), h: Math.round(g.height), scale: s,
            device_scale: s};
}

function isXwayland(w) {
    // X11 windows carry their X window id; Wayland windows have none.
    return typeof w.windowId === "number" && w.windowId > 0;
}

function snapshot() {
    const active = workspace.activeWindow;
    const windows = [];
    for (const w of workspace.windowList()) {
        if (!w || !w.normalWindow || w.deleted) {
            continue;
        }
        windows.push({
            pid: w.pid,
            app_id: w.desktopFileName || w.resourceClass || "",
            focus: w === active,
            xwayland: isXwayland(w),
            frame: rect(w.frameGeometry),
            buffer: rect(w.bufferGeometry),
            monitor: monitorOf(w),
            moving: w.move === true || w.resize === true,
            minimized: w.minimized === true,
            above: w.keepAbove === true,
            fullscreen: w.fullScreen === true,
        });
    }
    return JSON.stringify({version: 1, layout_mode: "logical", windows: windows});
}

function publish() {
    const json = snapshot();
    if (json === lastJson) {
        return; // the extension coalesces to one per redraw; this drops repeats
    }
    lastJson = json;
    // Activates the bridge on first use (D-Bus service file). A failure (no
    // bridge, or a real publisher owns the name) is harmless: send it again
    // with the next change.
    callDBus(BUS, PATH, FEED, "Publish", json, function () {});
}

function watch(w) {
    // Only the windows a snapshot reports: panels, popups and the like never
    // enter it, so their changes need not trigger one.
    if (!w || !w.normalWindow) {
        return;
    }
    w.frameGeometryChanged.connect(publish);
    w.outputChanged.connect(publish);
    w.minimizedChanged.connect(publish);
    w.fullScreenChanged.connect(publish);
    w.keepAboveChanged.connect(publish);
    w.interactiveMoveResizeStarted.connect(publish);
    w.interactiveMoveResizeFinished.connect(publish);
}

for (const w of workspace.windowList()) {
    watch(w);
}
workspace.windowAdded.connect(function (w) {
    watch(w);
    publish();
});
workspace.windowRemoved.connect(publish);
workspace.windowActivated.connect(publish);
workspace.screensChanged.connect(publish);
publish();
