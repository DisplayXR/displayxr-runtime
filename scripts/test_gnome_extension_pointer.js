// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
//
// Unit test for the GNOME Shell extension's pointer reply (PointerPick in
// contrib/gnome-shell/window-geometry@displayxr.org/lib.js, extension version 16,
// WindowGeometry1.GetPointer).
//
// Hermetic: plain gjs, no GNOME Shell. PointerPick lives outside lib.js's
// GI-dependent build(), so this drives the shipped code with stand-in actors.
//
//   gjs scripts/test_gnome_extension_pointer.js
//
// What it checks:
//   1. The window under the pointer is the first ancestor of the picked actor
//      that is a window actor (a surface or subsurface of it counts); shell UI
//      and the stage give none.
//   2. What a non-window hit is: the stage and the desktop background are the
//      desktop; a panel button is shell UI.
//   3. Desktop windows: type DESKTOP, or Ubuntu's desktop-icons window (a NORMAL
//      window before mutter 49) count as the desktop; an app window does not.
//   4. Points on no monitor (beside a smaller monitor, negative) are outside.
//   5. parent_id for a transient window (a popup), nothing otherwise.
//   6. maximized: is_maximized() (mutter 49+) or get_maximized() == BOTH; the
//      window types GetAllWindows lists.
//   7. The reply: locked -> nothing but {version, locked}; otherwise the point,
//      the layout mode when known, `over`, and the window entry.
'use strict';

const GLib = imports.gi.GLib;

const here = GLib.path_get_dirname(imports.system.programPath ?? imports.system.programInvocationName);
imports.searchPath.unshift(GLib.build_filenamev([here, '..', 'contrib', 'gnome-shell',
    'window-geometry@displayxr.org']));
imports.lib; // side effect: globalThis.displayxrWindowGeometry
const PP = globalThis.displayxrWindowGeometry.PointerPick;

let failed = 0;
function check(cond, what) {
    if (cond) {
        print(`  ok   ${what}`);
    } else {
        print(`  FAIL ${what}`);
        failed++;
    }
}

class Actor {
    constructor(parent = null, props = {}) {
        this._parent = parent;
        Object.assign(this, props);
    }

    get_parent() {
        return this._parent;
    }
}
class BackgroundActor extends Actor {}
class BackgroundGroup extends Actor {}
const Meta = {BackgroundActor, BackgroundGroup};

const stage = new Actor();
const windowGroup = new Actor(stage);
const metaWindow = {title: 'A'};
const windowActor = new Actor(windowGroup, {meta_window: metaWindow});
const surface = new Actor(windowActor);
const subsurface = new Actor(surface);
const uiGroup = new Actor(stage);
const panelButton = new Actor(new Actor(uiGroup));
const bgGroup = new BackgroundGroup(stage);
const bg = new BackgroundActor(bgGroup);
const bgChild = new Actor(bg);

print('1. the window under the pointer');
check(PP.windowOf(surface) === metaWindow, 'a surface actor -> its window');
check(PP.windowOf(subsurface) === metaWindow, 'a subsurface (e.g. a weave) -> its window');
check(PP.windowOf(windowActor) === metaWindow, 'the window actor itself');
check(PP.windowOf(panelButton) === null, 'shell UI -> no window');
check(PP.windowOf(stage) === null && PP.windowOf(null) === null, 'the stage / nothing -> no window');

print('2. what a non-window hit is');
check(PP.nonWindow(panelButton, stage, Meta) === 'shell', 'a panel button is shell UI');
check(PP.nonWindow(stage, stage, Meta) === 'desktop' && PP.nonWindow(null, stage, Meta) === 'desktop',
    'the stage / nothing is the desktop');
check(PP.nonWindow(bg, stage, Meta) === 'desktop' && PP.nonWindow(bgChild, stage, Meta) === 'desktop',
    'the desktop background is the desktop');

print('3. desktop windows');
{
    const M = {...Meta, WindowType: {NORMAL: 0, DESKTOP: 1}};
    const win = (type, cls, extra = {}) => ({get_window_type: () => type, get_wm_class: () => cls, ...extra});
    check(PP.isDesktopWindow(win(1, 'x'), M), 'type DESKTOP');
    check(PP.isDesktopWindow(win(0, 'com.rastersoft.ding'), M), 'ding by its app id (NORMAL before mutter 49)');
    check(PP.isDesktopWindow(win(0, '', {customJS_ding: {}}), M), 'ding by its marker property');
    check(!PP.isDesktopWindow(win(0, 'org.gnome.Nautilus'), M), 'an app window is not');
}

print('4. points on no monitor');
{
    const mons = [[0, 0, 2560, 1440], [2560, 0, 1920, 1080]];
    check(PP.onMonitor(10, 10, mons) && PP.onMonitor(2560, 1079, mons), 'on either monitor');
    check(!PP.onMonitor(3000, 1300, mons), 'beside the smaller monitor, inside the bounding box: outside');
    check(!PP.onMonitor(-1, 5, mons) && !PP.onMonitor(99999, 0, mons), 'negative / far away: outside');
    check(!PP.onMonitor(4480, 0, mons), 'the right edge is exclusive');
}

print('5. parent_id');
{
    const owner = {get_id: () => 4294967297};
    check(PP.parentOf({get_transient_for: () => owner}).parent_id === 4294967297, 'a popup: its owner id (> 2^32 exact)');
    check(Object.keys(PP.parentOf({get_transient_for: () => null})).length === 0, 'a toplevel: nothing');
}

print('6. maximized');
{
    const M = {MaximizeFlags: {HORIZONTAL: 1, VERTICAL: 2, BOTH: 3}};
    check(PP.isMaximized({is_maximized: () => true}, M) && !PP.isMaximized({is_maximized: () => false}, M),
        'mutter 49+: is_maximized()');
    check(PP.isMaximized({get_maximized: () => 3}, M), 'older mutter: get_maximized() BOTH');
    check(!PP.isMaximized({get_maximized: () => 2}, M) && !PP.isMaximized({get_maximized: () => 0}, M),
        'older mutter: one direction (half-tiled) or none is not maximized');
    check(!PP.isMaximized({}, M), 'neither method: not maximized');
}

print('6b. GetAllWindows types');
{
    const M = {WindowType: {NORMAL: 0, DESKTOP: 1, DOCK: 2, DIALOG: 3, MODAL_DIALOG: 4, TOOLBAR: 5, MENU: 6,
        UTILITY: 7, SPLASHSCREEN: 8, DROPDOWN_MENU: 9, POPUP_MENU: 10, TOOLTIP: 11}};
    check([0, 3, 4, 7].every(t => PP.inAllWindows(t, M)), 'normal, dialog, modal dialog, utility: listed');
    check([1, 2, 5, 6, 8, 9, 10, 11].every(t => !PP.inAllWindows(t, M)), 'desktop, dock, menus, popups, tooltips: not');
}

print('7. the reply');
{
    const r = PP.reply({locked: true});
    check(r.version === 1 && r.locked === true && r.x === undefined && r.window === undefined,
        'locked: nothing but {version, locked}');
    const entry = {pid: 7, title: 'A', frame: [1, 2, 3, 4]};
    const w = PP.reply({x: 10, y: 20, layout: 'logical', over: 'window', window: entry, windowType: 0});
    check(w.version === 1 && w.x === 10 && w.y === 20 && w.layout_mode === 'logical', 'point + layout mode');
    check(w.over === 'window' && w.window === entry && w.window_type === 0, 'over a window: its entry');
    const s = PP.reply({x: 5, y: 5, over: 'shell'});
    check(s.over === 'shell' && s.window === null && s.layout_mode === undefined, 'over shell UI, layout unknown');
    const o = PP.reply({x: 99999, y: 0, over: 'outside'});
    check(o.over === 'outside' && o.window === null, 'outside every monitor');
}

if (failed) {
    print(`${failed} check(s) FAILED`);
    imports.system.exit(1);
}
print('all pointer checks passed');
