// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
//
// Unit test for who gets window titles from the GNOME Shell extension
// (TitleAccess in contrib/gnome-shell/window-geometry@displayxr.org/lib.js,
// extension version 18, WindowGeometry1.SetTitleAccess).
//
// Hermetic: plain gjs, no GNOME Shell. TitleAccess lives outside lib.js's
// GI-dependent build(), so this drives the shipped code with stand-in windows.
//
//   gjs scripts/test_gnome_extension_title_access.js
//
// What it checks:
//   1. The grant gate: the caller names itself (pid 0 or its own PID, from the
//      bus), its PID must be known, never on the lock screen.
//   2. Who reads titles: only a bus name that was granted, never while locked,
//      never once the extension is disabled (no tracker).
//   3. The bookkeeping: one record (and one name watch) per connection, a
//      revoke forgets it, a revoke of an unknown name is harmless.
//   4. A withheld title is a missing key, never "": an untitled window still
//      reads "" to a granted caller.
//   5. The shipped file reads a window title in exactly one place
//      (TitleAccess.field), so no reply can carry one past the gate.
'use strict';

const GLib = imports.gi.GLib;

const here = GLib.path_get_dirname(imports.system.programPath ?? imports.system.programInvocationName);
const extDir = GLib.build_filenamev([here, '..', 'contrib', 'gnome-shell', 'window-geometry@displayxr.org']);
imports.searchPath.unshift(extDir);
imports.lib; // side effect: globalThis.displayxrWindowGeometry
const TA = globalThis.displayxrWindowGeometry.TitleAccess;

let failed = 0;
function check(cond, what) {
    if (cond) {
        print(`  ok   ${what}`);
    } else {
        print(`  FAIL ${what}`);
        failed++;
    }
}

class Win {
    constructor(title) {
        this._title = title;
    }

    get_title() {
        return this._title;
    }
}

print('1. the grant gate');
check(TA.mayGrant({senderPid: 4242, pid: 0, locked: false}), 'pid 0 = the caller itself');
check(TA.mayGrant({senderPid: 4242, pid: 4242, locked: false}), 'the caller\'s own pid');
check(!TA.mayGrant({senderPid: 4242, pid: 4243, locked: false}), 'another process\'s pid: refused');
check(!TA.mayGrant({senderPid: 0, pid: 0, locked: false}), 'the bus daemon could not say who asks: refused');
check(!TA.mayGrant({senderPid: 4242, pid: 0, locked: true}), 'on the lock screen: refused');

print('2. who reads titles');
const t = TA.newTracker();
check(!TA.mayRead(t, ':1.42', false), 'nobody granted: no titles');
check(TA.grant(t, ':1.42', 4242), 'first grant of a connection: new (watch its name)');
check(TA.mayRead(t, ':1.42', false), 'the granted connection reads titles');
check(!TA.mayRead(t, ':1.43', false), 'another connection (even of the same process) does not');
check(!TA.mayRead(t, ':1.42', true), 'not while the screen is locked, grant or not');
check(!TA.mayRead(null, ':1.42', false), 'not once the extension is disabled (no tracker)');
check(!TA.mayRead(t, null, false) && !TA.mayRead(t, undefined, false), 'no sender: no titles');

print('3. the bookkeeping');
check(!TA.grant(t, ':1.42', 4242), 'a second grant of the same connection: not new (no second watch)');
check(t.size === 1, 'one record per connection');
t.get(':1.42').watchId = 7;
const rec = TA.revoke(t, ':1.42');
check(rec !== null && rec.pid === 4242 && rec.watchId === 7, 'revoke hands back the record (to unwatch it)');
check(!TA.mayRead(t, ':1.42', false), 'revoked: no titles');
check(TA.revoke(t, ':1.42') === null, 'revoking again is harmless');
check(TA.revoke(t, ':1.99') === null, 'revoking a name never granted is harmless');
check(TA.grant(t, ':1.42', 4242), 'a grant after a revoke is new again (a fresh watch)');

print('4. a withheld title is a missing key');
const withheld = {app_id: 'gedit', ...TA.field(new Win('secret.txt — Text Editor'), false), focus: true};
check(!('title' in withheld), 'not granted: no "title" key at all');
check(Object.keys(withheld).join(',') === 'app_id,focus', 'and nothing in its place');
const shown = {app_id: 'gedit', ...TA.field(new Win('notes.txt'), true), focus: true};
check(shown.title === 'notes.txt', 'granted: the title');
check(Object.keys(shown).join(',') === 'app_id,title,focus', 'in its usual place in the entry');
check(TA.field(new Win(''), true).title === '', 'granted, untitled window: ""');
check(TA.field(new Win(null), true).title === '', 'granted, no title at all: ""');
check(JSON.stringify({...TA.field(new Win('x'), false)}) === '{}', 'withheld survives JSON as nothing');

print('5. one place reads a title');
const [, bytes] = GLib.file_get_contents(GLib.build_filenamev([extDir, 'lib.js']));
const src = new TextDecoder().decode(bytes);
const reads = src.split('get_title(').length - 1;
check(reads === 1, `lib.js calls get_title() exactly once, in TitleAccess.field (found ${reads})`);

if (failed) {
    print(`${failed} check(s) FAILED`);
    imports.system.exit(1);
}
print('all title-access checks passed');
