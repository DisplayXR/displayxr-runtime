// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
//
// Unit test for the GNOME Shell extension's workspace-hotkey bookkeeping
// (WorkspaceHotkey in contrib/gnome-shell/window-geometry@displayxr.org/lib.js,
// extension version 13, org.displayxr.WorkspaceHotkey1).
//
// Hermetic: plain gjs, no GNOME Shell. WorkspaceHotkey lives outside lib.js's
// GI-dependent build(), so this drives the shipped code directly.
//
//   gjs scripts/test_gnome_extension_workspace_hotkey.js
//
// What it checks:
//   1. Accelerator / unit validation (what Configure accepts), including every
//      accelerator the runtime's service_hotkey_to_accelerator can produce.
//   2. A press with the client registered only emits Activated.
//   3. A press with the client gone emits AND starts its unit, and the next
//      Configure reports it as pending exactly once, within 30 s only.
//   4. Configure("") releases, forgets the unit and never reports pending.
//   5. Suspend: no grab, no press; capped at 60 s; resumes by itself.
//   6. The cache file round trip never registers an owner.
'use strict';

const GLib = imports.gi.GLib;

const here = GLib.path_get_dirname(imports.system.programPath ?? imports.system.programInvocationName);
imports.searchPath.unshift(GLib.build_filenamev([here, '..', 'contrib', 'gnome-shell',
    'window-geometry@displayxr.org']));
imports.lib; // side effect: globalThis.displayxrWindowGeometry
const WH = globalThis.displayxrWindowGeometry.WorkspaceHotkey;

let failed = 0;
function check(cond, what) {
    if (cond) {
        print(`  ok   ${what}`);
    } else {
        print(`  FAIL ${what}`);
        failed++;
    }
}

print('1. validation');
{
    // The runtime's mapping (service_hotkey_to_accelerator) writes these.
    const runtime = ['<Control>space', '<Control><Shift>F5', '<Alt><Super>a', '<Super>Page_Down',
        '<Control>equal', '<Control>Return', '<Control>apostrophe', '<Control>grave', '<Shift><Alt>7',
        '<Control><Shift><Alt><Super>bracketright'];
    check(runtime.every(a => WH.validAccelerator(a)), 'every runtime-produced accelerator is accepted');
    check(WH.validAccelerator(''), '"" (release) is accepted');
    check(!WH.validAccelerator('space'), 'no modifier is refused');
    check(!WH.validAccelerator('<Control><Control>space'), 'a repeated modifier is refused');
    check(!WH.validAccelerator('<Hyper>space'), 'an unknown modifier is refused');
    check(!WH.validAccelerator('<Control>'), 'no key is refused');
    check(!WH.validAccelerator('<Control>space extra'), 'trailing junk is refused');
    check(!WH.validAccelerator(42), 'a non-string is refused');
    check(WH.validUnit('') && WH.validUnit('displayxr.service') && WH.validUnit('displayxr-dev.service'),
        'plain .service units are accepted');
    check(WH.validUnit('foo\\x2dbar.service'), 'a systemd-escaped unit name is accepted');
    check(!WH.validUnit('displayxr.socket'), 'a non-service unit is refused');
    check(!WH.validUnit('../evil.service') && !WH.validUnit('a b.service'), 'paths and spaces are refused');
}

print('2. registered client: a press only emits');
{
    const st = WH.create();
    check(!WH.wantGrab(st, 0), 'nothing grabbed before a Configure');
    check(WH.press(st, 0).emit === false, 'an unconfigured press does nothing');
    check(WH.configure(st, ':1.42', '<Control>space', 'displayxr.service', 1000) === false,
        'first Configure: nothing pending');
    check(WH.wantGrab(st, 1000), 'configured -> grabbed');
    const r = WH.press(st, 2000);
    check(r.emit && r.startUnit === null, 'press with the client alive: emit, no StartUnit');
    check(st.pendingUntil === 0, 'nothing becomes pending');
}

print('3. client gone: a press starts the unit and is carried to the next Configure');
{
    const st = WH.create();
    WH.configure(st, ':1.42', '<Control>space', 'displayxr.service', 0);
    check(WH.ownerGone(st, ':1.7') === false, 'an unrelated name vanishing is ignored');
    check(WH.ownerGone(st, ':1.42') === true && st.owner === null, 'the client vanishing unregisters it');
    check(WH.wantGrab(st, 10), 'the grab survives the client (the cold-start path)');
    const r = WH.press(st, 10000);
    check(r.emit && r.startUnit === 'displayxr.service', 'press: emit + StartUnit(displayxr.service)');
    check(WH.configure(st, ':1.99', '<Control>space', 'displayxr.service', 12000) === true,
        'the restarted service is told about the press');
    check(WH.configure(st, ':1.99', '<Control>space', 'displayxr.service', 12500) === false,
        'exactly once');

    WH.ownerGone(st, ':1.99');
    WH.press(st, 20000);
    check(WH.configure(st, ':1.100', '<Control>space', 'displayxr.service', 20000 + WH.PENDING_MS + 1) === false,
        'a press older than 30 s is dropped');

    const noUnit = WH.create();
    WH.configure(noUnit, ':1.5', '<Control>space', '', 0);
    WH.ownerGone(noUnit, ':1.5');
    const r2 = WH.press(noUnit, 100);
    check(r2.emit && r2.startUnit === null, 'no unit (service run by hand): emit only');
}

print('4. Configure("") releases');
{
    const st = WH.create();
    WH.configure(st, ':1.42', '<Control>space', 'displayxr.service', 0);
    WH.ownerGone(st, ':1.42');
    WH.press(st, 100); // pending
    check(WH.configure(st, ':1.43', '', 'displayxr.service', 200) === false, 'release never reports pending');
    check(!WH.wantGrab(st, 200) && st.unit === '', 'released: no grab, unit forgotten');
    check(WH.press(st, 300).emit === false, 'a released hotkey does nothing');
}

print('5. suspend');
{
    const st = WH.create();
    WH.configure(st, ':1.42', '<Control>space', 'displayxr.service', 0);
    check(WH.suspend(st, true, 60000, 1000) === 60000, 'suspend for 60 s');
    check(!WH.wantGrab(st, 1000) && WH.press(st, 1000).emit === false, 'suspended: no grab, no press');
    check(WH.wantGrab(st, 61001), 'resumes by itself after the timeout');
    check(WH.suspend(st, true, 10 * 60000, 0) === WH.MAX_SUSPEND_MS, 'a longer ask is capped at 60 s');
    check(WH.suspend(st, true, 0, 0) === WH.MAX_SUSPEND_MS, 'timeout 0 means the maximum');
    check(WH.suspend(st, false, 0, 5) === 0 && WH.wantGrab(st, 5), 'explicit resume');
}

print('6. cache round trip');
{
    const st = WH.create();
    WH.configure(st, ':1.42', '<Control><Alt>w', 'displayxr.service', 0);
    const text = WH.serialize(st);
    const back = WH.create();
    check(WH.deserialize(back, text), 'deserialize accepts what serialize wrote');
    check(back.accelerator === '<Control><Alt>w' && back.unit === 'displayxr.service', 'fields restored');
    check(back.owner === null, 'a restored hotkey has no owner (the client must Configure again)');
    const bad = WH.create();
    check(!WH.deserialize(bad, '{"version":1,"accelerator":"space","unit":""}') && bad.accelerator === '',
        'a malformed accelerator is not restored');
    check(!WH.deserialize(bad, 'not json') && !WH.deserialize(bad, '{"version":2}'),
        'junk and unknown versions are refused');
}

if (failed > 0) {
    print(`FAILED: ${failed} check(s)`);
    imports.system.exit(1);
}
print('all workspace-hotkey checks passed');
