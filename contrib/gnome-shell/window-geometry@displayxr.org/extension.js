// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
//
// DisplayXR Window Geometry — entry point for GNOME Shell 45 and later
// (ES modules + the `Extension` base class).
//
// This file is deliberately thin: everything it does lives in `lib.js`, which
// the GNOME 40–44 entry point (`extension-gnome42.js`) loads through the
// legacy importer. See the header of `lib.js` for the D-Bus surface, the JSON
// schema and why the shared file is written the way it is, and `README.md`
// for how a package picks the entry point matching the running shell.

import Clutter from 'gi://Clutter';
import GObject from 'gi://GObject';
import Meta from 'gi://Meta';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

// Side-effect import: defines globalThis.displayxrWindowGeometry. `lib.js`
// has no `export` of its own — that is what lets the legacy importer read the
// very same file.
import './lib.js';

const {WindowGeometryService} =
    globalThis.displayxrWindowGeometry.build({Clutter, GObject, Meta, Gio, GLib});

export default class WindowGeometryExtension extends Extension {
    enable() {
        this._service = new WindowGeometryService();
        this._service.enable();
    }

    disable() {
        this._service?.disable();
        this._service = null;
    }
}
