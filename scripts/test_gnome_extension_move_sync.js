// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
//
// Unit test for the GNOME Shell extension's move-sync frame logic
// (MoveSyncChoice in contrib/gnome-shell/window-geometry@displayxr.org/lib.js,
// extension version 9, runtime#1748).
//
// Hermetic: plain gjs, no GNOME Shell. MoveSyncChoice lives outside lib.js's
// GI-dependent build(), so this drives the shipped code directly.
//
//   gjs scripts/test_gnome_extension_move_sync.js
//
// What it checks:
//   1. The tag decode: offset of the 1x1 tag surface from the main surface,
//      mod 256, for negative stage positions and across the wrap; and it
//      agrees with the runtime's encode (comp_vk_native_wl_move_sync_encode).
//   2. Resolution: the newest history entry with the tag's position.
//   3. A drag: the actor is always shown at the position the painted buffer
//      was woven for, advances only on a newer woven frame, and catches up.
//   4. A reversal: frames woven on the way to the turn are shown where they
//      were woven for (the rule this replaced painted them off-origin).
//   5. A stalled app: after 100 ms AND 6 frames behind, the actor follows the
//      window; it re-syncs only on a frame woven for a recent position.
//   6. A stopped window with the actor behind asks for a frame tick; a moving
//      one does not.
'use strict';

const GLib = imports.gi.GLib;

const here = GLib.path_get_dirname(imports.system.programPath ?? imports.system.programInvocationName);
imports.searchPath.unshift(GLib.build_filenamev([here, '..', 'contrib', 'gnome-shell',
    'window-geometry@displayxr.org']));
imports.lib; // side effect: globalThis.displayxrWindowGeometry
const MS = globalThis.displayxrWindowGeometry.MoveSyncChoice;

let failed = 0;
function check(cond, what) {
    if (cond) {
        print(`  ok   ${what}`);
    } else {
        print(`  FAIL ${what}`);
        failed++;
    }
}

// The runtime's encode, comp_vk_native_wl_move_sync_encode, transcribed: C
// `%` truncates toward zero, as JS `%` does for integers.
const cEncode = v => ((v % 256) + 256) % 256;
// A tag as the extension reads it: the tag surface at main + encode(woven).
const tagFor = (woven, main) => MS.decode(main[0] + cEncode(woven[0]), main[1] + cEncode(woven[1]), main[0], main[1]);
const at = (p, x, y) => p !== null && p.x === x && p.y === y;

print('1. tag decode');
{
    check(MS.TAG_MOD === 256, 'TAG_MOD is 256 (the runtime\'s COMP_VK_NATIVE_WL_MOVE_SYNC_TAG_MOD)');
    let ok = true;
    for (const v of [0, 1, 255, 256, 257, 1919, -1, -255, -256, -257, -3000])
        ok = ok && MS.mod(v, 256) === cEncode(v);
    check(ok, 'mod() equals the runtime encode for positive, negative and wrapping values');
    // The main surface's position inside the window actor is not 0 (client-side
    // decorations put it below a title bar); the tag is relative to it.
    let rt = true;
    for (const w of [[0, 0], [511, 290], [-40, -700], [1920 + 255, 3]])
        for (const main of [[0, 0], [0, 29], [12, 58]])
            rt = rt && tagFor(w, main)[0] === cEncode(w[0]) && tagFor(w, main)[1] === cEncode(w[1]);
    check(rt, 'decode(main + encode(p), main) == encode(p) for any main-surface offset');
    check(MS.decode(3, 1, 10, 0)[0] === 249, 'a tag left of the main surface wraps (3 - 10 -> 249)');
}

print('2. resolution');
{
    const hist = [{x: 100, y: 5, t: 1}, {x: 356, y: 5, t: 2}, {x: 120, y: 5, t: 3}, {x: 100, y: 5, t: 4}];
    check(MS.resolve(hist, [100, 5]).t === 4, 'the NEWEST entry with the tag\'s position (100 and 356 share tag 100)');
    check(MS.resolve(hist, [120, 5]).t === 3, 'an older unique position');
    check(MS.resolve(hist, [7, 7]) === null, 'no match -> null');
}

// A simulated drag. The window moves +dx per stage frame; the runtime weaves
// each frame for the position it last read, and that frame is painted `lag`
// frames later (a FIFO queue). Returns per-frame {shown, woven} pairs.
function simulate({frames, move, lag = 3, stallFrom = -1, stallTo = -1, frameUs = 16667}) {
    let t = 1000000;
    let pos = [500, 300];
    const st = MS.create(pos[0], pos[1], t);
    const queue = []; // positions frames were woven for, oldest first
    let onScreen = null; // the woven position of the buffer on screen
    const out = [];
    for (let i = 0; i < frames; i++) {
        t += frameUs;
        const d = move(i);
        if (d[0] || d[1]) {
            pos = [pos[0] + d[0], pos[1] + d[1]];
            MS.onMove(st, pos[0], pos[1], t);
        }
        const stalled = i >= stallFrom && i < stallTo;
        if (!stalled)
            queue.push(pos.slice());
        if (queue.length > lag)
            onScreen = queue.shift();
        const tag = onScreen ? tagFor(onScreen, [0, 29]) : null;
        const r = MS.onFrame(st, tag, t);
        out.push({i, shown: {x: st.shown.x, y: st.shown.y}, woven: onScreen, cur: r.cur, r, pos: pos.slice()});
    }
    return {st, out};
}

print('3. a straight drag');
{
    const {st, out} = simulate({frames: 120, move: i => (i < 90 ? [2, 0] : [0, 0])});
    const tagged = out.filter(f => f.woven);
    check(tagged.every(f => f.shown.x === f.woven[0] && f.shown.y === f.woven[1]),
        'every tagged frame is shown exactly where it was woven for');
    let back = 0;
    for (let k = 1; k < out.length; k++)
        back += out[k].shown.x < out[k - 1].shown.x ? 1 : 0;
    check(back === 0, 'never moves backwards');
    const last = out[out.length - 1];
    check(last.r.caughtUp && last.shown.x === last.pos[0], 'catches up once the window stops');
    check(st.stats.timeouts === 0, 'no timeout while frames keep coming');
}

print('4. a reversal');
{
    // Right 30 frames, left 30, at 3 logical px a frame: positions repeat.
    const {out} = simulate({frames: 80, move: i => (i < 30 ? [3, 0] : (i < 60 ? [-3, 0] : [0, 0]))});
    const tagged = out.filter(f => f.woven);
    const off = tagged.filter(f => f.shown.x !== f.woven[0] || f.shown.y !== f.woven[1]).length;
    check(off === 0, `every frame of a reversal is shown where it was woven for (${off} off-origin)`);
    // The old rule (ignore a tag that resolves OLDER than the one shown) did not.
    const maxX = Math.max(...out.map(f => f.shown.x));
    check(maxX === 500 + 3 * 30, 'the turn itself is shown (the path is not cut short)');
}

print('5. a stalled app');
{
    // The app stops presenting for 40 frames mid-drag, then resumes.
    const {st, out} = simulate({frames: 140, move: i => (i < 110 ? [2, 0] : [0, 0]), stallFrom: 30, stallTo: 70});
    check(st.stats.timeouts === 1, 'exactly one timeout');
    const tFirstFollow = out.findIndex(f => f.shown.x === f.cur.x && f.woven && f.woven[0] !== f.shown.x);
    check(tFirstFollow > 30, 'the actor follows the window only after the stall');
    // The last frame on which a newly woven buffer advanced the actor.
    const followAt = out.find(f => f.r.timedOut);
    let lastAdvance = -1;
    for (let k = 1; followAt && k < followAt.i; k++) {
        if (out[k].shown.x !== out[k - 1].shown.x)
            lastAdvance = k;
    }
    const waited = followAt ? followAt.i - lastAdvance : 0;
    check(followAt !== undefined && waited * 16667 > MS.TIMEOUT_US && waited >= MS.TIMEOUT_FRAMES &&
        (waited - 1) * 16667 <= MS.TIMEOUT_US,
    `the timeout fires on the first frame past ${MS.TIMEOUT_US / 1000} ms and ${MS.TIMEOUT_FRAMES} frames ` +
        `without a newer woven frame (${waited} frames)`);
    const resynced = out.filter(f => f.i > 73 && f.woven && f.shown.x === f.woven[0]);
    check(resynced.length > 20, 're-syncs on frames woven after the stall');
    const last = out[out.length - 1];
    check(last.r.caughtUp, 'caught up at the end');
    // Frames woven BEFORE the stall that reach the screen after it are not
    // re-synced on (they would hop the window far back).
    let hop = 0;
    for (let k = 1; k < out.length; k++)
        hop = Math.max(hop, out[k - 1].shown.x - out[k].shown.x);
    check(hop <= 2 * 4, `no large backwards hop on re-sync (largest ${hop} logical px)`);
}

print('6. frame ticks');
{
    const t0 = 5000000;
    const st = MS.create(0, 0, t0);
    MS.onMove(st, 4, 0, t0 + 1000);
    const a = MS.onFrame(st, [0, 0], t0 + 2000); // behind, and the window just moved
    const b = MS.onFrame(st, [0, 0], t0 + 18000); // behind, and the window has not moved since
    check(!a.needTick, 'no tick requested while the window is moving');
    check(b.needTick, 'a tick is requested while the actor is behind a stopped window');
    const c = MS.onFrame(st, [4, 0], t0 + 34000);
    check(c.caughtUp && !c.needTick, 'no tick once caught up');
}

if (failed) {
    print(`test_gnome_extension_move_sync: ${failed} check(s) FAILED`);
    imports.system.exit(1);
}
print('test_gnome_extension_move_sync: all checks passed');
