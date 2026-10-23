// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
//
// Unit test for the monitor's work area in the GNOME Shell extension's window
// entries (WorkArea in contrib/gnome-shell/window-geometry@displayxr.org/lib.js,
// extension version 19, monitor.work_area).
//
// Hermetic: plain gjs, no GNOME Shell. WorkArea lives outside lib.js's
// GI-dependent build(), so this drives the shipped code with stand-in windows
// and workspaces.
//
//   gjs scripts/test_gnome_extension_work_area.js
//
// What it checks:
//   1. Which workspace is asked: the window's own; the active one when the
//      window has none (or its lookup throws); none when neither answers.
//   2. The rect: [x, y, w, h] from a Meta.Rectangle (GNOME 42-44) or an
//      Mtk.Rectangle (45+), both x, y, width, height; per monitor.
//   3. Absent when it cannot be told: no workspace, no such method, a bad
//      monitor index, a throw, an empty or non-integer rect.
//   4. The shipped wiring: a window entry's monitor.work_area comes from
//      WorkArea.of, only when it can be told; a work-area change emits
//      WindowsChanged.
'use strict';

const GLib = imports.gi.GLib;

const here = GLib.path_get_dirname(imports.system.programPath ?? imports.system.programInvocationName);
imports.searchPath.unshift(GLib.build_filenamev([here, '..', 'contrib', 'gnome-shell',
    'window-geometry@displayxr.org']));
imports.lib; // side effect: globalThis.displayxrWindowGeometry
const WA = globalThis.displayxrWindowGeometry.WorkArea;

let failed = 0;
function check(cond, what) {
    if (cond) {
        print(`  ok   ${what}`);
    } else {
        print(`  FAIL ${what}`);
        failed++;
    }
}

function same(a, b) {
    return JSON.stringify(a) === JSON.stringify(b);
}

//! A stand-in Meta.Workspace: @p areas[i] is monitor i's work area, as mutter
//! hands it back (an object with x, y, width, height).
class Workspace {
    constructor(name, areas) {
        this.name = name;
        this._areas = areas;
        this.asked = [];
    }

    get_work_area_for_monitor(mon) {
        this.asked.push(mon);
        const a = this._areas[mon];
        if (a instanceof Error)
            throw a;
        return a;
    }
}

class Win {
    constructor(ws, {throws = false} = {}) {
        this._ws = ws;
        this._throws = throws;
    }

    get_workspace() {
        if (this._throws)
            throw new Error('unmanaging');
        return this._ws;
    }
}

const manager = active => ({get_active_workspace: () => active});

// A 1920x1080 primary with a 32 px top bar, a 2560x1440 monitor to its right
// with a 64 px fixed dock on its left edge.
const area0 = {x: 0, y: 32, width: 1920, height: 1048};
const area1 = {x: 1984, y: 0, width: 2496, height: 1440};
const own = new Workspace('own', [area0, area1]);
const active = new Workspace('active', [{x: 0, y: 40, width: 1920, height: 1040}, area1]);

print('1. which workspace is asked');
check(WA.workspaceOf(new Win(own), manager(active)) === own, 'the window\'s own workspace');
check(WA.workspaceOf(new Win(null), manager(active)) === active,
    'no workspace of its own (mutter has none to give): the active one');
check(WA.workspaceOf(new Win(own, {throws: true}), manager(active)) === active,
    'its lookup throws (a window going away): the active one');
check(WA.workspaceOf({}, manager(active)) === active, 'no get_workspace at all: the active one');
check(WA.workspaceOf(new Win(null), null) === null, 'no workspace manager either: none');
check(WA.workspaceOf(new Win(null), {get_active_workspace: () => {
    throw new Error('x');
}}) === null, 'the active lookup throws: none');
check(WA.workspaceOf(null, manager(active)) === active, 'no window: the active one');

print('2. the rect');
check(same(WA.rect(own, 0), [0, 32, 1920, 1048]), 'monitor 0: below a 32 px top bar');
check(same(WA.rect(own, 1), [1984, 0, 2496, 1440]), 'monitor 1: right of a 64 px dock');
check(same(own.asked, [0, 1]), 'asked for exactly the monitor named');
// mutter 45+ hands back an Mtk.Rectangle, 42-44 a Meta.Rectangle: both a boxed
// struct with x, y, width, height (and more members the reader ignores).
class MtkRectangle {
    constructor() {
        this.x = 10;
        this.y = 20;
        this.width = 300;
        this.height = 400;
    }

    equal() {
        return false;
    }
}
check(same(WA.rect(new Workspace('mtk', [new MtkRectangle()]), 0), [10, 20, 300, 400]),
    'a boxed rectangle with methods reads the same');
check(same(WA.of(new Win(own), 0, manager(active)), [0, 32, 1920, 1048]), 'of(): the window\'s own workspace');
check(same(WA.of(new Win(null), 0, manager(active)), [0, 40, 1920, 1040]), 'of(): else the active one');

print('3. absent when it cannot be told');
check(WA.rect(null, 0) === null, 'no workspace');
check(WA.rect({}, 0) === null, 'a workspace without get_work_area_for_monitor');
check(WA.rect(own, -1) === null, 'monitor -1 (the window is on none)');
check(WA.rect(own, 1.5) === null && WA.rect(own, undefined) === null && WA.rect(own, '0') === null,
    'a monitor index that is not a whole number');
check(WA.rect(new Workspace('t', [new Error('bad monitor')]), 0) === null, 'mutter throws');
check(WA.rect(new Workspace('n', [null]), 0) === null, 'nothing handed back');
check(WA.rect(new Workspace('z', [{x: 0, y: 0, width: 0, height: 1080}]), 0) === null, 'zero width');
check(WA.rect(new Workspace('z', [{x: 0, y: 0, width: 1920, height: -4}]), 0) === null, 'negative height');
check(WA.rect(new Workspace('f', [{x: 0.5, y: 0, width: 1920, height: 1080}]), 0) === null,
    'not whole px');
check(WA.rect(new Workspace('s', [{x: '0', y: 0, width: 1920, height: 1080}]), 0) === null, 'not numbers');
check(WA.of(new Win(null), 0, null) === null, 'of(): no workspace anywhere');

print('4. in a window entry (the shipped wiring)');
// _windowEntry and enable() need GNOME Shell; read the shipped source instead.
const [, bytes] = GLib.file_get_contents(GLib.build_filenamev([here, '..', 'contrib', 'gnome-shell',
    'window-geometry@displayxr.org', 'lib.js']));
const src = new TextDecoder().decode(bytes);
const entry = src.slice(src.indexOf('_windowEntry(win, focus, layout, titles) {'));
const ds = entry.indexOf('monitor.device_scale = deviceScale;');
const wa = entry.indexOf('const workArea = WorkArea.of(win, mon, global.workspace_manager);');
const set = entry.indexOf('monitor.work_area = workArea;');
check(ds > 0 && wa > ds && set > wa, 'monitor.work_area is set from WorkArea.of after device_scale');
check(entry.indexOf('if (workArea)') > wa && entry.indexOf('if (workArea)') < set,
    'and only when it can be told (otherwise the key is absent, as before version 19)');
check(/display\.connect\('workareas-changed', \(\) => this\._queueEmit\(\)\)/.test(src),
    'a work-area change queues a WindowsChanged');

if (failed) {
    print(`${failed} check(s) FAILED`);
    imports.system.exit(1);
}
print('all work-area checks passed');
