// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
//
// Unit test for the GNOME Shell extension's own-window requests (OwnWindow in
// contrib/gnome-shell/window-geometry@displayxr.org/lib.js, extension version
// 15: SetKeepAbove / ActivateWindow / SetMinimized, placement capability bits
// 5-7).
//
// Hermetic: plain gjs, no GNOME Shell. OwnWindow lives outside lib.js's
// GI-dependent build(), so this drives the shipped code with stand-in windows.
//
//   gjs scripts/test_gnome_extension_own_window.js
//
// What it checks:
//   1. The gate: only the caller's own window (pid 0 or its own PID, from the
//      bus), never during a grab of that window or any extension pointer drag,
//      never on the lock screen.
//   2. Maximized is read on both sides of mutter 49's API change
//      (get_maximized() flags before, is_maximized() after).
//   3. SetKeepAbove sets and clears; a maximized window is refused and left
//      alone; one bus-name watch per client however many windows it raises.
//   4. Undo on disconnect: only the keep-above the extension set — never a
//      user's own "Always on Top" (also not one the user cleared and set again),
//      never a window whose keep-above was cleared since, never a window that
//      went away.
//   5. SetMinimized minimizes and restores, refuses a window mutter says
//      cannot be minimized, and is idempotent.
//   6. ActivateWindow's focus policy (version 17): 32-bit time order with
//      wrap-around; focus is taken only when the app had the user's latest
//      input and the user has not left it since (Alt+Tab, Super+H, the dock,
//      a click elsewhere); the app minimizing itself is not the user leaving;
//      coming back to the app (new input) lifts it.
'use strict';

const GLib = imports.gi.GLib;

const here = GLib.path_get_dirname(imports.system.programPath ?? imports.system.programInvocationName);
imports.searchPath.unshift(GLib.build_filenamev([here, '..', 'contrib', 'gnome-shell',
    'window-geometry@displayxr.org']));
imports.lib; // side effect: globalThis.displayxrWindowGeometry
const OW = globalThis.displayxrWindowGeometry.OwnWindow;

let failed = 0;
function check(cond, what) {
    if (cond) {
        print(`  ok   ${what}`);
    } else {
        print(`  FAIL ${what}`);
        failed++;
    }
}

const BOTH = 3; // Meta.MaximizeFlags.BOTH

//! A stand-in Meta.Window. api: 'new' (mutter 49+, is_maximized) or 'old'
//! (mutter 42-48, get_maximized returning flags).
class Win {
    constructor({above = false, maximized = false, minimized = false, canMinimize = true, api = 'new'} = {}) {
        this._above = above;
        this._maximized = maximized;
        this.minimized = minimized;
        this._canMinimize = canMinimize;
        this.calls = [];
        if (api === 'new')
            this.is_maximized = () => this._maximized;
        else
            this.get_maximized = () => (this._maximized ? BOTH : 0);
    }

    is_above() {
        return this._above;
    }

    make_above() {
        this.calls.push('make_above');
        this._above = true;
    }

    unmake_above() {
        this.calls.push('unmake_above');
        this._above = false;
    }

    can_minimize() {
        return this._canMinimize;
    }

    minimize() {
        this.calls.push('minimize');
        this.minimized = true;
    }

    unminimize() {
        this.calls.push('unminimize');
        this.minimized = false;
    }
}

print('1. the gate');
{
    const win = new Win(), other = new Win();
    const base = {senderPid: 100, pid: 0, win, grabbed: null, pointerDrag: false, locked: false};
    check(OW.allowed(base), 'pid 0 = the caller: allowed');
    check(OW.allowed({...base, pid: 100}), 'the caller\'s own PID: allowed');
    check(!OW.allowed({...base, pid: 101}), 'another PID: refused');
    check(!OW.allowed({...base, senderPid: 0}), 'no PID from the bus: refused');
    check(!OW.allowed({...base, win: null}), 'the caller has no window: refused');
    check(!OW.allowed({...base, grabbed: win}), 'the user is dragging this window: refused');
    check(OW.allowed({...base, grabbed: other}), 'a grab on another window: allowed');
    check(!OW.allowed({...base, pointerDrag: true}), 'an extension pointer drag is running: refused');
    check(!OW.allowed({...base, locked: true}), 'the screen is locked: refused');
}

print('2. maximized, both mutter APIs');
{
    check(OW.isMaximized(new Win({maximized: true}), BOTH), 'mutter 49+: is_maximized() true');
    check(!OW.isMaximized(new Win(), BOTH), 'mutter 49+: is_maximized() false');
    check(OW.isMaximized(new Win({maximized: true, api: 'old'}), BOTH), 'mutter <= 48: get_maximized() == BOTH');
    check(!OW.isMaximized(new Win({api: 'old'}), BOTH), 'mutter <= 48: get_maximized() == 0');
}

print('3. SetKeepAbove');
{
    const t = OW.newTracker();
    const a = new Win(), b = new Win();
    let r = OW.keepAbove(t, ':1.5', a, true, BOTH);
    check(r.ok && a.is_above() && r.watch, 'set: above, first window of the client -> watch its name');
    r = OW.keepAbove(t, ':1.5', a, true, BOTH);
    check(r.ok && !r.watch && a.calls.length === 1, 'set again: ok, no second make_above, no second watch');
    r = OW.keepAbove(t, ':1.5', b, true, BOTH);
    check(r.ok && !r.watch, 'a second window of the same client: no second watch');
    r = OW.keepAbove(t, ':1.5', a, false, BOTH);
    check(r.ok && !a.is_above(), 'clear: no longer above');
    r = OW.keepAbove(t, ':1.5', a, false, BOTH);
    check(r.ok && a.calls.filter(c => c === 'unmake_above').length === 1, 'clear again: ok, nothing to undo');

    const m = new Win({maximized: true});
    r = OW.keepAbove(t, ':1.6', m, true, BOTH);
    check(!r.ok && !m.is_above() && m.calls.length === 0 && !r.watch, 'maximized: refused, untouched');
    const mo = new Win({maximized: true, api: 'old'});
    check(!OW.keepAbove(t, ':1.6', mo, true, BOTH).ok, 'maximized (mutter <= 48): refused');

    const user = new Win({above: true});
    r = OW.keepAbove(t, ':1.7', user, true, BOTH);
    check(r.ok && !r.watch && user.calls.length === 0, 'already above (the user\'s): ok, not recorded');
}

print('4. undo on disconnect: only what the extension set');
{
    const t = OW.newTracker();
    const ours = new Win(), user = new Win({above: true}), cleared = new Win(), gone = new Win();
    OW.keepAbove(t, ':1.8', ours, true, BOTH);
    OW.keepAbove(t, ':1.8', user, true, BOTH);      // the user's own: not recorded
    OW.keepAbove(t, ':1.8', cleared, true, BOTH);
    OW.keepAbove(t, ':1.8', cleared, false, BOTH);  // cleared since: forgotten
    OW.keepAbove(t, ':1.8', gone, true, BOTH);
    OW.forgetWindow(t, gone);                       // the window was unmanaged
    const other = new Win();
    OW.keepAbove(t, ':1.9', other, true, BOTH);     // another client

    const n = OW.dropClient(t, ':1.8');
    check(n === 1 && !ours.is_above(), 'the window the client raised is lowered again');
    check(user.is_above() && user.calls.length === 0, 'the user\'s own "Always on Top" is left alone');
    check(cleared.calls.filter(c => c === 'unmake_above').length === 1, 'a cleared window is not touched again');
    check(gone.calls.filter(c => c === 'unmake_above').length === 0, 'a window that went away is not touched');
    check(other.is_above(), 'another client\'s window is left alone');
    check(OW.dropClient(t, ':1.8') === 0, 'a second disconnect does nothing');
    check(!t.has(':1.8') && t.has(':1.9'), 'only that client\'s record is dropped');

    // The user cleared it by hand meanwhile: nothing to undo, nothing re-set.
    const handCleared = new Win();
    OW.keepAbove(t, ':1.10', handCleared, true, BOTH);
    handCleared.unmake_above();
    check(OW.dropClient(t, ':1.10') === 0 && !handCleared.is_above(), 'cleared by the user meanwhile: left alone');

    // The user cleared it and set it again (window menu off, then on): theirs now. The
    // service calls forgetWindow on every clear (notify::above going false).
    const reset = new Win();
    OW.keepAbove(t, ':1.11', reset, true, BOTH);
    reset.unmake_above();
    OW.forgetWindow(t, reset);
    reset.make_above();
    check(OW.dropClient(t, ':1.11') === 0 && reset.is_above(), 'cleared and set again by the user: left above');
}

print('5. SetMinimized');
{
    const w = new Win();
    check(OW.minimize(w, true) && w.minimized, 'minimize');
    check(OW.minimize(w, true) && w.calls.filter(c => c === 'minimize').length === 1, 'minimize again: ok, idempotent');
    check(OW.minimize(w, false) && !w.minimized, 'restore');
    check(OW.minimize(w, false) && w.calls.filter(c => c === 'unminimize').length === 1, 'restore again: ok, idempotent');
    const fixed = new Win({canMinimize: false});
    check(!OW.minimize(fixed, true) && !fixed.minimized && fixed.calls.length === 0,
        'a window mutter cannot minimize: refused, untouched');
}

print('6. ActivateWindow focus policy (version 17)');
{
    check(OW.timeBefore(100, 200) && !OW.timeBefore(200, 100) && !OW.timeBefore(5, 5), 'time order');
    check(OW.timeBefore(0xfffffff0, 0x10) && !OW.timeBefore(0x10, 0xfffffff0), 'time order across the 32-bit wrap');
    check(OW.timeBefore(0, 1) && OW.timeBefore(0, 0xf0000000) && !OW.timeBefore(1, 0), '0 = never set: before any time');
    check(OW.latestTime([]) === 0 && OW.latestTime([5, 900, 30]) === 900, 'latest of several');
    check(OW.latestTime([0xfffffff0, 0x10, 0xffffff00]) === 0x10, 'latest across the wrap');
    // 25 days awake: a window untouched since boot is 2^31 ms or more behind and must not
    // read as the newest (the plain order would say it is, against a time 2^31+ ahead).
    const now = 0x90000000;
    check(OW.latestTime([5, 0x8ff00000, 0x8fff0000], now) === 0x8fff0000, 'with now: newest by age');
    check(OW.latestTime([0x10000000, 0x8fff0000], now) === 0x8fff0000 &&
        OW.latestTime([0x8fff0000, 0x10000000], now) === 0x8fff0000, 'with now: an ancient time is dropped, any order');
    check(OW.latestTime([0, 0], now) === 0, 'with now: nothing set -> 0');
    check(OW.latestTime([0x20, 0x30], 0x10) === 0x30, 'with now: every time dropped -> the plain order');

    const APP = 100, ED = 200;
    // The user typed in the editor after the app's last input: refused.
    let st = OW.newFocusState();
    check(!OW.mayTakeFocus(st, APP, 1000, 1500), 'the user\'s latest input is in another app: no focus');
    // The app had the latest input (the user clicked it, nothing since): allowed.
    check(OW.mayTakeFocus(st, APP, 1500, 1500), 'the app had the latest input: focus');
    // No input anywhere yet: nothing to take it from.
    check(OW.mayTakeFocus(st, APP, 0, 0), 'no input anywhere: focus');
    // No window has focus (the app minimized itself, nothing else open): nothing to take.
    check(OW.mayTakeFocus(st, APP, 0, 1500, true), 'no window has focus: focus');

    // Alt+Tab away: the keys were the app's input, then focus left it.
    OW.focusMoved(st, APP, ED, 1500, false);
    check(!OW.mayTakeFocus(st, APP, 1500, 1500), 'left with Alt+Tab / Super+H (no newer input): no focus');
    // The user clicks the app again: newer input than the leave, and focus returns.
    OW.focusMoved(st, ED, APP, 1500, false);
    check(OW.mayTakeFocus(st, APP, 1600, 1600) && !st.away.has(APP), 'back in the app: focus, the leave is forgotten');

    // Minimized from the dock (a shell click: no window input), focus to the editor.
    st = OW.newFocusState();
    OW.focusMoved(st, APP, ED, 1600, false);
    check(!OW.mayTakeFocus(st, APP, 1600, 1600), 'minimized by the user from the dock: no focus');
    // A newer input in the app (it comes back) lifts it even if focus went elsewhere.
    check(OW.mayTakeFocus(st, APP, 1700, 1700), 'newer input in the app since the leave: focus');

    // The app minimized itself: focus moving away is not the user leaving.
    st = OW.newFocusState();
    const w = new Win();
    OW.minimize(w, true, st);
    check(st.selfMinimized.has(w), 'its own minimize is recorded');
    OW.focusMoved(st, APP, ED, 1600, st.selfMinimized.has(w) && w.minimized);
    check(OW.mayTakeFocus(st, APP, 1600, 1600), 'after minimizing itself, nothing newer: focus');
    check(!OW.mayTakeFocus(st, APP, 1600, 1700), 'after minimizing itself, the user typed elsewhere: no focus');
    OW.minimize(w, false, st);
    check(!st.selfMinimized.has(w), 'a restore ends "minimized by its own app"');
    const userMin = new Win({minimized: true});
    OW.minimize(userMin, true, st);
    check(!st.selfMinimized.has(userMin), 'already minimized (by the user): not recorded as its own');

    // Focus to no window (the desktop, the lock screen) counts as leaving.
    st = OW.newFocusState();
    OW.focusMoved(st, APP, 0, 1800, false);
    check(!OW.mayTakeFocus(st, APP, 1800, 1800), 'focus to no window: left');
    check(!OW.mayTakeFocus(st, APP, 1800, 1800, true), 'left, then nothing has focus: still no focus');
    // Focus between two windows of the same app is not leaving it.
    st = OW.newFocusState();
    OW.focusMoved(st, APP, APP, 1800, false);
    check(OW.mayTakeFocus(st, APP, 1800, 1800), 'focus between the app\'s own windows: not left');
    // Another app's leave does not affect this one.
    OW.focusMoved(st, ED, APP, 1900, false);
    check(OW.mayTakeFocus(st, APP, 1900, 1900) && !OW.mayTakeFocus(st, ED, 1800, 1900), 'per app');
}

if (failed) {
    print(`${failed} check(s) FAILED`);
    imports.system.exit(1);
}
print('all own-window checks passed');
