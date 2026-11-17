// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
//
// Unit test for whether each window can be seen, the GNOME Shell extension's
// per-window "hidden" (Visibility in
// contrib/gnome-shell/window-geometry@displayxr.org/lib.js, extension version
// 20, displayxr-runtime#2689).
//
// Hermetic: plain gjs, no GNOME Shell. Visibility lives outside lib.js's
// GI-dependent build(), so this drives the shipped code with stand-in stacks.
//
//   gjs scripts/test_gnome_extension_visibility.js
//
// What it checks:
//   1. The rect math: intersection, the uncovered area after subtracting
//      occluders, the 2 % sliver tolerance, an empty target is never covered,
//      a remainder too fragmented to tell is "visible".
//   2. Which windows cover: shown, not minimized, on the active workspace,
//      another process, a window type a user calls a window, not the desktop,
//      opaque. Docks, the desktop, notifications, menus, tooltips, combos and
//      drag icons never do.
//   3. A stack: only windows ABOVE count, several occluders together can
//      cover, partial cover is "", the precedence locked > minimized >
//      workspace > covered, and the target is clipped to its monitor.
//   4. The shipped wiring: the entry's "hidden" key, one stacking pass per
//      snapshot, and the emits on restack, workspace switch, lock and
//      monitor change.
'use strict';

const GLib = imports.gi.GLib;

const here = GLib.path_get_dirname(imports.system.programPath ?? imports.system.programInvocationName);
imports.searchPath.unshift(GLib.build_filenamev([here, '..', 'contrib', 'gnome-shell',
    'window-geometry@displayxr.org']));
imports.lib; // side effect: globalThis.displayxrWindowGeometry
const V = globalThis.displayxrWindowGeometry.Visibility;

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

// Meta.WindowType as mutter numbers it.
const T = {
    NORMAL: 0, DESKTOP: 1, DOCK: 2, DIALOG: 3, MODAL_DIALOG: 4, TOOLBAR: 5, MENU: 6, UTILITY: 7,
    SPLASHSCREEN: 8, DROPDOWN_MENU: 9, POPUP_MENU: 10, TOOLTIP: 11, NOTIFICATION: 12, COMBO: 13, DND: 14,
    OVERRIDE_OTHER: 15,
};

//! One stack entry, an opaque normal window of @p pid at @p frame by default.
function win(pid, frame, more = {}) {
    return {
        pid, shown: true, minimized: false, onActiveWorkspace: true, covers: true, desktop: false,
        opacity: 255, frame, target: frame, ...more,
    };
}

print('1. the rect math');
check(same(V.intersect([0, 0, 100, 100], [50, 50, 100, 100]), [50, 50, 50, 50]), 'intersection');
check(V.intersect([0, 0, 100, 100], [100, 0, 10, 10]) === null, 'edge-adjacent rects do not intersect');
check(V.intersect(null, [0, 0, 1, 1]) === null, 'no rect: none');
check(V.uncoveredArea([0, 0, 100, 100], []) === 10000, 'nothing over it: all of it');
check(V.uncoveredArea([0, 0, 100, 100], [[-10, -10, 200, 200]]) === 0, 'one bigger window: none');
check(V.uncoveredArea([0, 0, 100, 100], [[0, 0, 50, 100], [50, 0, 50, 100]]) === 0,
    'two halves together: none');
check(V.uncoveredArea([0, 0, 100, 100], [[25, 25, 50, 50]]) === 7500, 'a hole in the middle: the ring');
check(V.uncoveredArea([0, 0, 100, 100], [[0, 0, 100, 50], [0, 25, 100, 50]]) === 2500,
    'overlapping occluders count once');
check(V.covered([0, 0, 100, 100], [[0, 0, 99, 100]]), '1 % sliver left: covered (2 % tolerance)');
check(!V.covered([0, 0, 100, 100], [[0, 0, 97, 100]]), '3 % left: not covered');
check(V.covered([0, 0, 100, 100], [[0, 0, 98, 100]]), 'exactly 2 % left: covered');
check(!V.covered([0, 0, 100, 100], [[0, 0, 98, 100]], 0), 'zero tolerance: 2 % left is not covered');
check(!V.covered([0, 0, 0, 100], [[-10, -10, 200, 200]]), 'an empty target is never covered');
check(!V.covered(null, [[0, 0, 1, 1]]), 'no target (on no monitor): not covered');
// A comb of 1-px teeth: every tooth splits each piece, past the work list.
const comb = [];
for (let x = 0; x < 400; x += 2)
    comb.push([x, 0, 1, 1]);
for (let y = 2; y < 400; y += 2)
    comb.push([0, y, 400, 1]);
check(V.uncoveredArea([0, 0, 400, 400], comb) === -1 && !V.covered([0, 0, 400, 400], comb),
    'too fragmented to tell: not covered (the lens stays 3D)');

print('2. which windows cover');
check(V.occluderSkip(win(2, [0, 0, 1, 1]), 1) === '', 'an opaque normal window of another app');
check(V.occluderSkip(win(2, [0, 0, 1, 1], {shown: false}), 1) === 'not-shown', 'not shown');
check(V.occluderSkip(win(2, [0, 0, 1, 1], {minimized: true}), 1) === 'minimized', 'minimized');
check(V.occluderSkip(win(2, [0, 0, 1, 1], {onActiveWorkspace: false}), 1) === 'workspace',
    'on another workspace');
check(V.occluderSkip(win(1, [0, 0, 1, 1]), 1) === 'same-pid', 'the same process (its own dialog)');
check(V.occluderSkip(win(0, [0, 0, 1, 1]), 0) === '', 'pid 0 on both sides is not "the same process"');
check(V.occluderSkip(win(2, [0, 0, 1, 1], {opacity: 254}), 1) === 'translucent', 'actor opacity below 255');
check(V.occluderSkip(win(2, [0, 0, 1, 1], {opacity: undefined}), 1) === 'translucent',
    'no opacity known: translucent (the visible side)');
check(V.occluderSkip(win(2, [0, 0, 1, 1], {desktop: true}), 1) === 'type', 'the desktop-icons window');
check(V.occluderSkip(win(2, [0, 0, 1, 1], {covers: false}), 1) === 'type', 'a type that does not cover');
for (const t of ['NORMAL', 'DIALOG', 'MODAL_DIALOG', 'UTILITY', 'TOOLBAR', 'SPLASHSCREEN'])
    check(V.coversType(T[t], T), `${t} covers`);
for (const t of ['DESKTOP', 'DOCK', 'MENU', 'DROPDOWN_MENU', 'POPUP_MENU', 'TOOLTIP', 'NOTIFICATION', 'COMBO',
    'DND', 'OVERRIDE_OTHER'])
    check(!V.coversType(T[t], T), `${t} never covers`);

print('3. a stack (bottom to top)');
const mon = [0, 0, 1920, 1080];
const target = win(1, [100, 100, 800, 600]);
check(same(V.compute([target], false), ['']), 'alone: seen');
check(same(V.compute([target, win(2, [0, 0, 1920, 1080])], false), ['covered', '']),
    'a maximized window above: the target is "covered", the top one seen');
check(V.compute([win(2, [0, 0, 1920, 1080]), target], false)[0] === '', 'the same window BELOW: seen');
check(V.compute([target, win(2, [0, 0, 500, 1080]), win(3, [500, 0, 1420, 1080])], false)[0] === 'covered',
    'two windows side by side cover it together');
check(V.compute([target, win(2, [0, 0, 600, 1080])], false)[0] === '', 'partly covered: seen');
check(V.compute([target, win(1, [0, 0, 1920, 1080])], false)[0] === '', 'its own app\'s window over it: seen');
check(V.compute([target, win(2, [0, 0, 1920, 1080], {opacity: 200})], false)[0] === '',
    'a translucent window over it: seen');
check(V.compute([target, win(2, [0, 0, 1920, 1080], {covers: false})], false)[0] === '',
    'a dock / notification / menu over it: seen');
check(V.compute([target, win(2, [0, 0, 1920, 1080], {minimized: true})], false)[0] === '',
    'a minimized window "over" it: seen');
check(V.compute([target, win(2, [0, 0, 1920, 1080], {onActiveWorkspace: false})], false)[0] === '',
    'a window on another workspace "over" it: seen');
check(V.compute([win(1, [0, 0, 10, 10], {minimized: true})], false)[0] === 'minimized', 'minimized');
check(V.compute([win(1, [0, 0, 10, 10], {onActiveWorkspace: false})], false)[0] === 'workspace',
    'on another workspace');
check(V.compute([win(1, [0, 0, 10, 10], {minimized: true})], true)[0] === 'locked', 'locked beats minimized');
check(V.compute([target, win(2, [0, 0, 1920, 1080])], true).every(r => r === 'locked'),
    'locked: every window says "locked"');
check(V.compute([win(1, [0, 0, 10, 10], {minimized: true, onActiveWorkspace: false})], false)[0] === 'minimized',
    'minimized beats workspace');
check(V.compute([win(1, [0, 0, 10, 10], {onActiveWorkspace: false}), win(2, [0, 0, 1920, 1080])], false)[0] ===
    'workspace', 'workspace beats covered');
// Clipped to the monitor: a window hanging off the right edge, the part on
// its monitor covered.
const hanging = win(1, [1500, 100, 800, 600], {target: V.intersect([1500, 100, 800, 600], mon)});
check(V.compute([hanging, win(2, [1400, 0, 520, 1080])], false)[0] === 'covered',
    'only the part on its monitor is judged');
check(V.compute([win(1, [0, 0, 10, 10], {target: null}), win(2, [0, 0, 1920, 1080])], false)[0] === '',
    'on no monitor: seen');
const reasons = V.compute([win(1, [0, 0, 1920, 1080]), win(2, [100, 100, 400, 300]),
    win(3, [0, 0, 1920, 1080])], false);
check(same(reasons, ['covered', 'covered', '']), 'every window of the stack gets its own answer');

print('4. the shipped wiring');
const [, bytes] = GLib.file_get_contents(GLib.build_filenamev([here, '..', 'contrib', 'gnome-shell',
    'window-geometry@displayxr.org', 'lib.js']));
const src = new TextDecoder().decode(bytes);
const entry = src.slice(src.indexOf('_windowEntry(win, focus, layout, titles) {'));
check(/hidden: this\._hiddenFor\(win\),/.test(entry.slice(0, entry.indexOf('_snapshotJson('))),
    'a window entry carries "hidden"');
check(/this\._hiddenCache = this\._hiddenMap\(\);/.test(src) && /this\._hiddenCache = null;/.test(src),
    'one stacking pass per snapshot');
check(/Visibility\.compute\(stack, Main\?\.sessionMode\?\.isLocked === true\)/.test(src),
    'the stack is judged with the lock state');
check(/for \(const actor of global\.get_window_actors\(\)\)[\s\S]{0,400}const f = win\.get_frame_rect\(\);/.test(src),
    'the stack is global.get_window_actors(), bottom to top');
check(/target = Visibility\.intersect\(\[f\.x, f\.y, f\.width, f\.height\]/.test(src) &&
    !/target = Visibility\.intersect\(\[b\.x/.test(src),
    'the covered target is the FRAME rect (a CSD buffer rect holds its shadow)');
check(/this\._connectOther\(display, 'restacked'\);/.test(src), 'a restack emits');
check(/this\._connectOther\(global\.workspace_manager, 'active-workspace-changed'\);/.test(src),
    'a workspace switch emits');
check(/this\._connectOther\(Main\?\.layoutManager, 'monitors-changed'\);/.test(src), 'a monitor change emits');
check(/sessionMode\.connect\('updated', \(\) => \{[\s\S]{0,300}this\._queueEmit\(\);/.test(src),
    'the lock screen coming or going emits');
check(/\['workspace-changed', \(\) => this\._queueEmit\(\)\]/.test(src), 'a window moved to another workspace emits');
check(/win\.connect\('notify::minimized'/.test(src), 'minimize / unminimize emits (since version 15)');
check(/if \(now !== opaque\) \{/.test(src), 'an actor opacity change emits only at the opaque edge');

if (failed) {
    print(`${failed} check(s) FAILED`);
    imports.system.exit(1);
}
print('all visibility checks passed');
