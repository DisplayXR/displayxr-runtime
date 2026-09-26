// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
//
// Unit test for the GNOME Shell extension's drag-lattice entry choice
// (LatticeChoice in contrib/gnome-shell/window-geometry@displayxr.org/lib.js,
// extension version 9, runtime#1748).
//
// Hermetic: plain gjs, no GNOME Shell, no GI beyond what gjs itself loads.
// lib.js is written to load under the legacy importer, and LatticeChoice lives
// outside its GI-dependent build(), so this drives the shipped code directly.
//
//   gjs scripts/test_gnome_extension_lattice.js
//
// What it checks:
//   1. Until the drag has a direction, the choice is the plain nearest entry.
//   2. Along a drag, an entry on the drag line beats a closer one beside it,
//      and the lead/lag it may add is capped (past the cap: plain nearest).
//   3. The direction is an axis: a drag that reverses keeps it; a turn moves it.
//   4. Everything is in device px: the same device geometry at 100 % and
//      200 % gives the same choice.
//   5. Over a lens-shaped table at 150 % and 200 % (reachable positions only,
//      a slanted phase lattice), a straight drag moves less sideways than
//      under plain nearest, lands only on table entries, and never adds more
//      than the cap along the drag.
'use strict';

const GLib = imports.gi.GLib;

const here = GLib.path_get_dirname(imports.system.programPath ?? imports.system.programInvocationName);
imports.searchPath.unshift(GLib.build_filenamev([here, '..', 'contrib', 'gnome-shell',
    'window-geometry@displayxr.org']));
imports.lib; // side effect: globalThis.displayxrWindowGeometry
const LC = globalThis.displayxrWindowGeometry.LatticeChoice;

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
const listOf = entries => cb => entries.forEach(e => cb(e[0], e[1]));

// A drag that has moved from the start to (x, y) in logical steps of `step`.
function dragTo(st, x, y, scale, step = 1) {
    const n = Math.max(Math.abs(x - st.lx), Math.abs(y - st.ly)) / step;
    const x0 = st.lx, y0 = st.ly;
    for (let i = 1; i <= n; i++)
        LC.observe(st, Math.round(x0 + (x - x0) * i / n), Math.round(y0 + (y - y0) * i / n), scale);
}

print('1. no direction yet -> plain nearest');
{
    const st = LC.create();
    LC.observe(st, 1, 0, 2); // 2 device px: below MIN_TRAVEL_PX
    const e = [[3, 0], [1, 1]];
    check(same(LC.choose(st, listOf(e), 1, 0, 2), [1, 1]), 'first 2 device px pick the nearest entry');
}

print('2. an entry on the drag line beats a closer one beside it; the lag is capped');
{
    const st = LC.create();
    dragTo(st, 20, 0, 2);
    // raw at (20, 0): (22, 0) is 4 device px ahead, (20, 1) is 2 device px aside.
    check(same(LC.choose(st, listOf([[22, 0], [20, 1]]), 20, 0, 2), [22, 0]),
        'ahead 4 px wins over aside 2 px on a horizontal drag');
    check(same(LC.choose(st, listOf([[22, 0], [20, 1]]), 20, 0, 2, true), [20, 1]),
        'the isotropic switch (DISPLAYXR_LATTICE_NEAREST) still picks aside');
    // (24, 0) is 8 device px ahead: past MAX_ALONG_PX, so it is not eligible.
    check(LC.MAX_ALONG_PX < 8, 'the cap is below 8 device px');
    check(same(LC.choose(st, listOf([[24, 0], [20, 1]]), 20, 0, 2), [20, 1]),
        'past the cap the aside entry is taken');
    // Nothing within the cap at all: plain nearest, never no answer.
    check(same(LC.choose(st, listOf([[25, 0], [26, 0]]), 20, 0, 2), [25, 0]),
        'no entry within the cap -> nearest');
    check(LC.choose(st, listOf([]), 20, 0, 2) === null, 'no candidates -> null (a coverage miss)');
}

print('3. the direction is an axis');
{
    const st = LC.create();
    dragTo(st, 20, 0, 2);
    dragTo(st, 10, 0, 2); // back along the same line
    check(same(LC.choose(st, listOf([[8, 0], [10, 1]]), 10, 0, 2), [8, 0]),
        'after reversing, the drag line is still horizontal');
    dragTo(st, 10, 30, 2); // then 60 device px down
    check(same(LC.choose(st, listOf([[10, 32], [11, 30]]), 10, 30, 2), [10, 32]),
        'after turning, the drag line is vertical');
}

print('4. device px at any scale');
{
    const a = LC.create(), b = LC.create();
    dragTo(a, 40, 0, 1);
    dragTo(b, 20, 0, 2);
    // 4 device px ahead vs 2 aside: ahead, at both scales.
    check(same(LC.choose(a, listOf([[44, 0], [40, 2]]), 40, 0, 1), [44, 0]) &&
        same(LC.choose(b, listOf([[22, 0], [20, 1]]), 20, 0, 2), [22, 0]), 'ahead within the cap, at 100 % and 200 %');
    // 8 device px ahead vs 2 aside: past the cap, at both scales.
    check(same(LC.choose(a, listOf([[48, 0], [40, 2]]), 40, 0, 1), [40, 2]) &&
        same(LC.choose(b, listOf([[24, 0], [20, 1]]), 20, 0, 2), [20, 1]), 'past the cap, at 100 % and 200 %');
}

print('5. a lens-shaped table: less sideways motion, on-table, lag capped');
{
    // Reachable positions whose DEVICE phase (x + slant*y)/period is within
    // tol of the start's: a test double for the app's table, nothing more.
    const period = 2.76, slant = 0.287, tol = 0.045;
    const l2p = (v, s) => Math.sign(v) * Math.floor(Math.abs(v * s) + 0.5);
    for (const scale of [1.5, 2]) {
        const r0x = 601, r0y = 237;
        const dev = (lx, ly) => [l2p(r0x + lx, scale) - l2p(r0x, scale), l2p(r0y + ly, scale) - l2p(r0y, scale)];
        const cell = 3, buckets = new Map(), members = new Set();
        for (let y = -40; y <= 40; y++) {
            for (let x = -40; x <= 200; x++) {
                const [px, py] = dev(x, y);
                const v = (px + slant * py) / period;
                if (Math.abs(v - Math.round(v)) > tol)
                    continue;
                const key = `${Math.floor(x / cell)},${Math.floor(y / cell)}`;
                if (!buckets.has(key))
                    buckets.set(key, []);
                buckets.get(key).push(x, y);
                members.add(`${x},${y}`);
            }
        }
        const near = (dx, dy) => cb => {
            const bx = Math.floor(dx / cell), by = Math.floor(dy / cell);
            for (let j = -2; j <= 2; j++) {
                for (let i = -2; i <= 2; i++) {
                    const b = buckets.get(`${bx + i},${by + j}`) ?? [];
                    for (let k = 0; k < b.length; k += 2)
                        cb(b[k], b[k + 1]);
                }
            }
        };
        const run = isotropic => {
            const st = LC.create();
            let side2 = 0, n = 0, swTravel = 0, lastSide = null, alongMax = 0, onTable = true;
            for (let x = 1; x <= 150; x++) {
                LC.observe(st, x, 0, scale);
                const e = LC.choose(st, near(x, 0), x, 0, scale, isotropic);
                const [ex, ey] = dev(e[0], e[1]), [rx] = dev(x, 0);
                const side = ey;
                side2 += side * side;
                n++;
                if (lastSide !== null)
                    swTravel += Math.abs(side - lastSide);
                lastSide = side;
                if (x > 10)
                    alongMax = Math.max(alongMax, Math.abs(ex - rx));
                onTable = onTable && members.has(`${e[0]},${e[1]}`);
            }
            return {rms: Math.sqrt(side2 / n), swTravel, alongMax, onTable};
        };
        const iso = run(true), dir = run(false);
        print(`  scale ${scale}: sideways rms ${iso.rms.toFixed(2)} -> ${dir.rms.toFixed(2)} device px, ` +
            `sideways travel ${iso.swTravel} -> ${dir.swTravel}, along max ${iso.alongMax} -> ${dir.alongMax}`);
        check(dir.onTable && iso.onTable, `scale ${scale}: every move lands on a table entry`);
        check(dir.rms < 0.8 * iso.rms, `scale ${scale}: sideways rms at least 20 % lower`);
        check(dir.swTravel < 0.8 * iso.swTravel, `scale ${scale}: sideways travel at least 20 % lower`);
        check(dir.alongMax <= LC.MAX_ALONG_PX + 0.5, `scale ${scale}: lead/lag within the cap`);
    }
}

if (failed) {
    print(`test_gnome_extension_lattice: ${failed} check(s) FAILED`);
    imports.system.exit(1);
}
print('test_gnome_extension_lattice: all checks passed');
