// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
//
// DisplayXR Window Geometry — entry point for GNOME Shell 40 to 44
// (Ubuntu 22.04 ships GNOME 42).
//
// Before GNOME 45 the extension loader is `ExtensionUtils.installImporter()`
// followed by `extension.imports.extension` and a call to a top-level
// `init()`: the legacy GJS importer, for which `import`/`export` is a parse
// error and `resource:///org/gnome/shell/extensions/extension.js` does not
// exist. So this file is the same thin entry point as `extension.js` written
// in the other module system, and the logic they share lives in `lib.js` —
// one file, loadable by both (see its header).
//
// A package NEVER installs this file under this name: GNOME only ever loads
// `<extension dir>/extension.js`. `scripts/linux/displayxr-gnome-extension-enable`
// copies this file over that slot when the running shell is older than 45.
//
// Portability rules for edits, same as lib.js: no ESM syntax (CI parses this
// file under the script goal, where `import`/`export` is rejected), and only
// `function` declarations are visible to the legacy importer — a top-level
// `let`/`const`/`class` is NOT exported from a legacy GJS module.

'use strict';

const {Clutter, GObject, Meta, Gio, GLib, Graphene} = imports.gi;
const ExtensionUtils = imports.misc.extensionUtils;

let service = null;

function init() {
    const Me = ExtensionUtils.getCurrentExtension();
    // Evaluating lib.js is what defines globalThis.displayxrWindowGeometry.
    // The legacy importer exports nothing from it (it declares no top-level
    // `var`), which is exactly why it publishes itself on globalThis.
    void Me.imports.lib;

    const {WindowGeometryService} =
        globalThis.displayxrWindowGeometry.build({Clutter, GObject, Meta, Gio, GLib, Graphene});
    service = new WindowGeometryService();
    // Shells that take init()'s return value use it as the state object and
    // call enable()/disable() on IT; shells that ignore the return value call
    // the module's own enable()/disable() below. Both reach this instance.
    return service;
}

function enable() {
    service?.enable();
}

function disable() {
    service?.disable();
}
