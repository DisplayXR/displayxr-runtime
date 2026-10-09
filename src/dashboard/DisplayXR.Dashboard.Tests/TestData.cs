// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.IO;
using System.Linq;
using DisplayXR.Dashboard;
using DisplayXR.Dashboard.Model;

namespace DisplayXR.Dashboard.Tests;

internal static class TestData
{
    static TestData() => DashboardLog.Disabled = true;

    public static string Path(string name) => System.IO.Path.Combine(AppContext.BaseDirectory, "Data", name);

    public static string Text(string name) => File.ReadAllText(Path(name));

    public static string[] Lines(string name) =>
        File.ReadAllLines(Path(name)).Where(l => l.Trim().Length > 0).ToArray();

    public static StatusSnapshot Parse(string json)
    {
        DashboardLog.Disabled = true;
        Xunit.Assert.True(StatusSnapshotParser.TryParse(json, out var s, out var err), err);
        return s!;
    }

    /// <summary>The design §3 example, minus its comments: the schema every renderer reads.</summary>
    public const string DesignExample = """
    {
      "schema": 1,
      "source": "service",
      "generation": { "topology": 17, "status": 2412 },
      "runtime": { "version": "2.32.0", "git_tag": "v2.32.0", "plugin_abi": 5,
                   "active_openxr_runtime": "C:\\Program Files\\DisplayXR\\Runtime\\DisplayXR_win64.json" },
      "plugins": [
        { "id": "leia-sr", "name": "DisplayXR Leia SR", "vendor": "Leia Inc.", "version": "2.13.0",
          "load": "ACTIVE", "platform_state": "READY", "hint": "", "fallback": false, "probe_order": 50 },
        { "id": "sim-display", "name": "DisplayXR Sim Display", "version": "2.32.0",
          "load": "NOT_ATTEMPTED", "platform_state": "UNKNOWN", "fallback": true, "probe_order": 200 }
      ],
      "screens": [
        { "id": "0x8c413a2f61152ce7", "index": 0,
          "device_name": "\\\\.\\DISPLAY1", "friendly_name": "AUO B194",
          "edid": { "manufacturer": "AUO", "product": "B194", "serial": 0 },
          "desktop": { "left": 0, "top": 0, "width": 3840, "height": 2160, "scale": 2.5 },
          "native": { "width": 3840, "height": 2160, "refresh_mhz": 60000, "is_native": true },
          "physical_mm": { "width": 344, "height": 194, "source": "plugin" },
          "roles": { "os_main": true, "runtime_default": true, "vendor_primary": true },
          "claim": { "plugin_id": "leia-sr", "confidence": "VERIFIED", "confidence_value": 100,
                     "serial": "QALA2137AL0011", "apis": ["d3d11", "d3d12", "vk", "gl"] },
          "layout": { "width_m": 0.3442, "height_m": 0.1936,
                      "nominal_viewer_m": { "x": 0.0, "y": 0.10, "z": 0.60 }, "source": "plugin" },
          "eye_tracking": { "supported": ["MANAGED"], "default": "MANAGED", "state": "TRACKING" },
          "mode": { "index": 1, "name": "LeiaSR", "views": 2, "is_3d": true },
          "dps": [ { "client_id": 3, "api": "d3d11", "kind": "primary", "backend": "OK" } ],
          "vendor": { "present": true, "ready": true, "verified": true, "calibrated": true,
                      "tracker": "RUNNING", "lens": "3D", "model": "AL", "serial": "QALA2137AL0011",
                      "worst_warning": null, "dashboard_command": "vendor.exe --page displays --display {serial}" },
          "warnings": [] },
        { "id": "0xb72ea4c616544d01", "index": 1,
          "device_name": "\\\\.\\DISPLAY5", "friendly_name": "Acer SpatialLabs DS1",
          "desktop": { "left": 3840, "top": 0, "width": 3840, "height": 2160, "scale": 3.0 },
          "roles": { "os_main": false, "runtime_default": false, "vendor_primary": false },
          "claim": { "plugin_id": "leia-sr", "confidence": "VERIFIED", "confidence_value": 100,
                     "serial": "QI012321D10117", "apis": ["d3d11", "d3d12", "vk", "gl"] },
          "eye_tracking": { "supported": ["MANAGED"], "default": "MANAGED", "state": "TRACKING" },
          "dps": [ { "client_id": 3, "api": "d3d11", "kind": "segment", "backend": "OK" } ],
          "vendor": { "present": true, "ready": true, "verified": true, "calibrated": true,
                      "tracker": "RUNNING", "lens": "3D", "model": "D1", "serial": "QI012321D10117",
                      "worst_warning": null, "dashboard_command": "x" },
          "warnings": [ { "code": "SEGMENT_FLAT_2D", "level": "warn",
                          "text": "A window spans this screen but its segment has no display processor; that half is flat 2D." } ] }
      ],
      "clients": [
        { "id": 3, "pid": 24416, "class": "APP", "name": "cube_handle_d3d11_win.exe",
          "flags": { "active": true, "visible": true, "focused": true, "overlay": false },
          "presenter": "APP_HWND",
          "lease": "slot",
          "window": { "left": 3018, "top": 285, "width": 1664, "height": 2093 },
          "owner_screen": "0xb72ea4c616544d01",
          "segments": { "generation": 41, "split": true, "items": [
            { "screen": "0x8c413a2f61152ce7", "canvas": { "x": 0, "y": 0, "w": 822, "h": 1875 },
              "has_dp": true, "woven": true, "eye_source": "DP" },
            { "screen": "0xb72ea4c616544d01", "canvas": { "x": 822, "y": 0, "w": 842, "h": 1875 },
              "has_dp": true, "woven": true, "eye_source": "DP" } ] },
          "views": { "capacity": 2, "active": 2, "reported": 4 },
          "integrity": { "paint": 1811, "present": 1809, "skip": 2, "weave_placement": "scanout" } }
      ],
      "workspace": { "enabled": false, "controller": null },
      "warnings": []
    }
    """;
}
