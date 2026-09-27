// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
//
// Unit test for the GNOME Shell extension's pointer drag (PointerDrag in
// contrib/gnome-shell/window-geometry@displayxr.org/lib.js, extension
// version 9, placement capability 4).
//
// Hermetic: plain gjs, no GNOME Shell, no GI beyond what gjs itself loads.
// lib.js is written to load under the legacy importer, and PointerDrag lives
// outside its GI-dependent build(), so this drives the shipped code directly.
//
//   gjs scripts/test_gnome_extension_pointer_drag.js
//
// What it checks (the drag must end on the release of the button that
// started it — the right button included, which mutter's own move grab does
// not end on):
//   1. The masks are Clutter's BUTTON1_MASK..BUTTON5_MASK; the side buttons
//      have none, so no drag starts on them, and none starts on a button that
//      is not held. A held RIGHT button reads as BUTTON2_MASK on mutter 50.1
//      (its evdev-ordered mask table) — either bit is accepted for middle and
//      right, and the one actually set is the one watched.
//   2. The window follows the pointer's displacement from the start, and a
//      poll with no pointer motion moves nothing.
//   3. The drag ends on the first poll whose mask lacks the button — with the
//      last pointer position still applied — and never on another button's
//      bit going away.
//   4. A release anywhere (a fast flick far outside the window) ends it the
//      same way: the mask, not a position or an event, decides.
'use strict';

const GLib = imports.gi.GLib;

const here = GLib.path_get_dirname(imports.system.programPath ?? imports.system.programInvocationName);
imports.searchPath.unshift(GLib.build_filenamev([here, '..', 'contrib', 'gnome-shell',
    'window-geometry@displayxr.org']));
imports.lib; // side effect: globalThis.displayxrWindowGeometry
const PD = globalThis.displayxrWindowGeometry.PointerDrag;

let failed = 0;
function check(cond, what) {
    if (cond) {
        print(`  ok   ${what}`);
    } else {
        print(`  FAIL ${what}`);
        failed++;
    }
}
const same = (a, b) => a !== null && b !== null && a[0] === b[0] && a[1] === b[1];
const B1 = 1 << 8, B2 = 1 << 9, B3 = 1 << 10, SHIFT = 1 << 0;

print('1. masks');
check(PD.maskFor(1) === B1 && PD.maskFor(2) === B2 && PD.maskFor(3) === B3, 'buttons 1-3 -> BUTTON1..3_MASK');
check(PD.maskFor(5) === 1 << 12, 'button 5 -> BUTTON5_MASK');
check(PD.maskFor(8) === 0 && PD.maskFor(9) === 0 && PD.maskFor(0) === 0, 'side buttons / 0: no mask');
check(PD.create(8, 0, 0, 0, 0, 0xffff) === null, 'no drag on a button without a mask');
check(PD.create(3, 0, 0, 0, 0, SHIFT) === null, 'no drag when the button is not held');
check(PD.create(3, 0, 0, 0, 0, B2).mask === B2, 'right held, read as BUTTON2 (mutter 50.1): watched');
check(PD.create(3, 0, 0, 0, 0, B3).mask === B3, 'right held, read as BUTTON3 (a fixed mutter): watched');
check(PD.create(1, 0, 0, 0, 0, B2 | B3) === null, 'left drag: middle/right bits do not count');

print('2. follows the pointer');
{
    const st = PD.create(3, 100, 200, 500, 600, B3);
    let r = PD.step(st, 500, 600, B3);
    check(!r.end && r.move === null, 'no motion -> no move');
    r = PD.step(st, 510, 603, B3 | SHIFT);
    check(!r.end && same(r.move, [110, 203]), 'displacement from the start, modifiers ignored');
    r = PD.step(st, 490, 590, B3);
    check(!r.end && same(r.move, [90, 190]), 'back past the start');
    r = PD.step(st, 490, 590, B3);
    check(r.move === null, 'same position again -> no move');
}

print('3. ends on the button, and only on it');
{
    const st = PD.create(3, 0, 0, 0, 0, B3);
    let r = PD.step(st, 5, 0, B3 | B1);
    check(!r.end, 'another button held too');
    r = PD.step(st, 6, 0, B3);
    check(!r.end, 'the OTHER button released -> still dragging');
    r = PD.step(st, 9, 1, 0);
    check(r.end && same(r.move, [9, 1]), 'right button up -> ends, with the last position applied');
}
{
    const st = PD.create(1, 0, 0, 0, 0, B1);
    const r = PD.step(st, 0, 0, B3);
    check(r.end && r.move === null, 'left drag: its own bit gone ends it, whatever else is held');
}

print('4. a release anywhere');
{
    const st = PD.create(3, 1000, 500, 1200, 700, B2); // mutter 50.1's bit for the right button
    let r;
    for (let i = 1; i <= 10; i++)
        r = PD.step(st, 1200 + 300 * i, 700 - 40 * i, B2); // a fast flick, far off the window
    check(!r.end && same(r.move, [1000 + 3000, 500 - 400]), 'a fast flick is followed');
    r = PD.step(st, 4300, 300, 0);
    check(r.end, 'released far outside the window -> ends');
    check(st.moves === 11, 'every distinct position counted');
}

if (failed) {
    print(`${failed} check(s) FAILED`);
    imports.system.exit(1);
}
print('all pointer-drag checks passed');
