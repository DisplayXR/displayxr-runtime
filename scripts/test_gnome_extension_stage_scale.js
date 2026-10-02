// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
//
// Unit test for the GNOME Shell extension's stage-space resolution
// (StageScale in contrib/gnome-shell/window-geometry@displayxr.org/lib.js,
// extension version 11).
//
// Hermetic: plain gjs, no GNOME Shell. StageScale lives outside lib.js's
// GI-dependent build(), so this drives the shipped code directly.
//
//   gjs scripts/test_gnome_extension_stage_scale.js
//
// The inputs are measured on a headless mutter 50.1 with one 3840x2160
// virtual monitor, switching the layout mode with `gdctl set --layout-mode`:
//
//   layout     scale   monitor geometry   stage view scale
//   physical   2       3840x2160          1
//   logical    2       1920x1080          2
//   logical    1.5     2560x1440          1.5
//   physical   1       3840x2160          1
//
// What it checks:
//   1. The layout mode is read from the view scales: any view scale != 1 is
//      LOGICAL, every view at 1 with a scaled monitor is PHYSICAL, and with
//      every monitor at 1 (or no view information) nothing is claimed.
//   2. The device scale (device px per stage px): 1 in PHYSICAL whatever the
//      monitor scale, the monitor scale in LOGICAL, and nothing for a scaled
//      monitor when the mode is unknown (the consumer then asks mutter).
//   3. The measured monitors convert to the panel's 3840x2160 in every row.
'use strict';

const GLib = imports.gi.GLib;

const here = GLib.path_get_dirname(imports.system.programPath ?? imports.system.programInvocationName);
imports.searchPath.unshift(GLib.build_filenamev([here, '..', 'contrib', 'gnome-shell',
    'window-geometry@displayxr.org']));
imports.lib; // side effect: globalThis.displayxrWindowGeometry
const SS = globalThis.displayxrWindowGeometry.StageScale;

let failed = 0;
function check(cond, what) {
    if (cond) {
        print(`  ok   ${what}`);
    } else {
        print(`  FAIL ${what}`);
        failed++;
    }
}

print('1. layout mode from the stage views');
{
    check(SS.layoutMode([1], [2]) === 'physical', 'scale 2, view at 1 -> physical (the reported box)');
    check(SS.layoutMode([2], [2]) === 'logical', 'scale 2, view at 2 -> logical');
    check(SS.layoutMode([1.5], [1.5]) === 'logical', 'scale 1.5, view at 1.5 -> logical');
    check(SS.layoutMode([1, 2], [1, 2]) === 'logical', 'mixed 1 + 2, views 1 + 2 -> logical');
    check(SS.layoutMode([1, 1], [1, 2]) === 'physical', 'mixed 1 + 2, views both 1 -> physical');
    check(SS.layoutMode([1], [1]) === null, 'every monitor at 1 -> not claimed (both modes agree)');
    check(SS.layoutMode([], [2]) === null, 'no views -> not claimed');
    check(SS.layoutMode(null, [2]) === null, 'no view API -> not claimed');
    check(SS.layoutMode([1.00001], [2]) === 'physical', 'float noise on a view scale is ignored');
}

print('2. device px per stage px');
{
    check(SS.deviceScale('physical', 2) === 1, 'physical, scale 2 -> 1');
    check(SS.deviceScale('physical', 1.5) === 1, 'physical, any scale -> 1');
    check(SS.deviceScale('logical', 2) === 2, 'logical, scale 2 -> 2');
    check(SS.deviceScale('logical', 1.25) === 1.25, 'logical, scale 1.25 -> 1.25');
    check(SS.deviceScale(null, 1) === 1, 'unknown, scale 1 -> 1 (true in both modes)');
    check(SS.deviceScale(null, 2) === null, 'unknown, scale 2 -> not published');
}

print('3. the measured monitors are 3840x2160 device px in every mode');
{
    const rows = [
        {layout: 'physical', scale: 2, w: 3840, h: 2160, views: [1]},
        {layout: 'logical', scale: 2, w: 1920, h: 1080, views: [2]},
        {layout: 'logical', scale: 1.5, w: 2560, h: 1440, views: [1.5]},
        {layout: 'physical', scale: 1, w: 3840, h: 2160, views: [1]},
    ];
    for (const r of rows) {
        const mode = SS.layoutMode(r.views, [r.scale]);
        const k = SS.deviceScale(mode, r.scale);
        check(k !== null && Math.round(r.w * k) === 3840 && Math.round(r.h * k) === 2160,
            `${r.layout} at ${r.scale}: ${r.w}x${r.h} stage -> 3840x2160 device`);
        check(Math.round(r.w * r.scale) === 3840 || r.layout === 'physical',
            `${r.layout} at ${r.scale}: the monitor scale alone is right only in logical`);
    }
    // The reported failure, pinned: physical at 2 multiplied by the monitor
    // scale is 7680x4320.
    check(3840 * 2 === 7680, 'physical at 2 x monitor scale = 7680 (what a scale-multiplying consumer reads)');
}

if (failed) {
    print(`test_gnome_extension_stage_scale: ${failed} check(s) FAILED`);
    imports.system.exit(1);
}
print('test_gnome_extension_stage_scale: all checks passed');
