#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
"""Generate the dashboard's development / test fixtures (ADR-051 design §3 shape).

Run from this directory:  python gen_fixtures.py
  two-panels.ndjson     two keyed (phase-7) claimed panels, a split client, a DIAG client; every
                        other line drops the second segment's DP (SEGMENT_FLAT_2D),
                        and one line is cut short (the "last read failed" path)
  stress-16x32.ndjson   16 screens in a 4x4 grid, 32 clients, NOT_NATIVE /
                        CLAIM_FALLBACK / unclaimed rows
  empty-headless.ndjson zero screens, zero clients, source headless
"""
import json

APIS = ["d3d11", "d3d12", "vk", "gl"]
CONF_VALUE = {"VERIFIED": 100, "EDID": 50, "FALLBACK": 10, "NONE": 0}


def screen(i, sid, dev, name, left, top, w, h, scale, os_main=False, rt_default=False, vprimary=False,
           conf="VERIFIED", plugin="leia-sr", serial="", tracking="TRACKING", dps=None, warnings=None,
           vendor=True, mm=(344, 194), key=None):
    out = {
        "id": sid, "index": i, "device_name": dev, "friendly_name": name,
        "edid": {"manufacturer": name.split()[0][:3].upper(), "product": name.split()[-1], "serial": 0},
        "desktop": {"left": left, "top": top, "width": w, "height": h, "scale": scale},
        "native": {"width": w, "height": h, "refresh_mhz": 60000, "is_native": True},
        "physical_mm": {"width": mm[0], "height": mm[1], "source": "plugin" if mm[0] else "none"},
        "roles": {"os_main": os_main, "runtime_default": rt_default, "vendor_primary": vprimary},
        "claim": {"plugin_id": plugin, "confidence": conf, "confidence_value": CONF_VALUE[conf],
                  "serial": serial, "apis": APIS if plugin else []},
        "layout": {"width_m": mm[0] / 1000, "height_m": mm[1] / 1000,
                   "nominal_viewer_m": {"x": 0.0, "y": 0.1, "z": 0.6}, "source": "plugin"},
        "eye_tracking": {"supported": ["MANAGED"], "default": "MANAGED", "state": tracking},
        "mode": {"index": 1, "name": "LeiaSR", "views": 2, "is_3d": True} if dps else None,
        "dps": dps or [],
        "vendor": ({"present": True, "ready": True, "verified": conf == "VERIFIED", "calibrated": True,
                    "tracker": "RUNNING" if tracking == "TRACKING" else "DOWN", "lens": "3D", "model": "AL",
                    "serial": serial, "worst_warning": None,
                    "dashboard_command": "vendor-dashboard.exe --page displays --display {serial}"}
                   if vendor else
                   {"present": False, "ready": False, "verified": False, "calibrated": False, "tracker": "UNKNOWN",
                    "lens": "UNKNOWN", "model": "", "serial": "", "worst_warning": None, "dashboard_command": None}),
        "warnings": warnings or [],
    }
    if key is not None:  # phase 7: a stable per-screen key (absent from older CLIs)
        out["key"] = key
    return out


def client(cid, pid, name, cls="APP", window=None, owner=None, segs=None, split=False, paint=1811, flags=None,
           presenter="APP_HWND", lease="slot"):
    app = cls == "APP"
    return {
        "id": cid, "pid": pid, "class": cls, "name": name,
        "flags": flags or {"active": app, "visible": app, "focused": app, "overlay": False},
        "presenter": presenter if app else "NONE", "lease": lease if app else "none",
        "window": window, "owner_screen": owner,
        "segments": {"generation": 41 if app else 0, "split": split, "items": segs or []},
        "views": {"capacity": 2, "active": 2, "reported": 4} if app else {"capacity": 0, "active": 0, "reported": 0},
        "integrity": {"paint": paint if app else 0, "present": max(0, paint - 2) if app else 0,
                      "skip": 2 if app else 0, "weave_placement": "scanout" if app else None},
    }


def seg(screen_id, x, w, h, has_dp=True):
    return {"screen": screen_id, "canvas": {"x": x, "y": 0, "w": w, "h": h}, "has_dp": has_dp, "woven": has_dp,
            "eye_source": "DP" if has_dp else "NONE"}


def snap(screens, clients, warnings=None, gen=(17, 2412), source="service"):
    return {
        "schema": 1, "source": source, "generation": {"topology": gen[0], "status": gen[1]},
        "runtime": {"version": "2.32.0", "git_tag": "v2.32.0", "plugin_abi": 5,
                    "active_openxr_runtime": "C:\\Program Files\\DisplayXR\\Runtime\\DisplayXR_win64.json"},
        "plugins": [
            {"id": "leia-sr", "name": "DisplayXR Leia SR", "vendor": "Leia Inc.", "version": "2.13.0",
             "load": "ACTIVE", "platform_state": "READY", "hint": "", "fallback": False, "probe_order": 50},
            {"id": "sim-display", "name": "DisplayXR Sim Display", "vendor": "DisplayXR", "version": "2.32.0",
             "load": "NOT_ATTEMPTED", "platform_state": "UNKNOWN", "hint": "", "fallback": True, "probe_order": 200}],
        "screens": screens, "clients": clients,
        "workspace": {"enabled": False, "controller": None}, "warnings": warnings or [],
    }


A, B = "0x8c413a2f61152ce7", "0xb72ea4c616544d01"
FLAT = {"code": "SEGMENT_FLAT_2D", "level": "warn",
        "text": "A window spans this screen but its segment has no display processor; that half is flat 2D."}
DEV = "\\\\.\\DISPLAY"


def two_panels(paint, flat):
    screens = [
        screen(0, A, DEV + "1", "AUO B194", 0, 0, 3840, 2160, 2.5, True, True, True, serial="QALA2137AL0011", key="edid:AUO-B194-QALA2137AL0011",
               dps=[{"client_id": 3, "api": "d3d11", "kind": "primary", "backend": "OK"}]),
        screen(1, B, DEV + "5", "Acer SpatialLabs DS1", 3840, 0, 3840, 2160, 3.0, serial="QI012321D10117", key="edid:ACR-0001-QI012321D10117",
               dps=[] if flat else [{"client_id": 3, "api": "d3d11", "kind": "segment", "backend": "OK"}],
               warnings=[FLAT] if flat else [], mm=(340, 190)),
    ]
    clients = [
        client(3, 24416, "cube_handle_d3d11_win.exe", window={"left": 3018, "top": 285, "width": 1664, "height": 1400},
               owner=B, split=True, paint=paint,
               segs=[seg(A, 0, 822, 1340), seg(B, 822, 842, 1340, has_dp=not flat)]),
        client(5, 31120, "displayxr-cli.exe", cls="DIAG"),
    ]
    return snap(screens, clients, gen=(17, 2412 + paint))


def dump(o):
    return json.dumps(o, separators=(",", ":"))


def main():
    with open("two-panels.ndjson", "w", newline="\n") as f:
        for k in range(4):
            line = dump(two_panels(1811 + 120 * k, flat=(k % 2 == 1)))
            f.write(line + "\n")
            if k == 1:  # a line cut short mid-write: the feed must keep the last good snapshot
                f.write(line[:700] + "\n")

    screens, clients = [], []
    for i in range(16):
        r, c = divmod(i, 4)
        sid = "0x%016x" % (0x1000 + i)
        warn, conf, plugin = [], "VERIFIED", "leia-sr"
        if i == 5:
            warn = [{"code": "NOT_NATIVE", "level": "critical",
                     "text": "The desktop mode is not the panel's native 1920x1080: set it in Display settings."}]
        if i == 9:
            conf, plugin = "FALLBACK", "sim-display"
            warn = [{"code": "CLAIM_FALLBACK", "level": "warn", "text": "Claimed only by the simulation fallback."}]
        if i == 12:
            conf, plugin = "NONE", None
        screens.append(screen(i, sid, DEV + str(i + 1), "Panel %02d" % i, c * 1920, r * 1080, 1920, 1080, 1.5,
                              os_main=(i == 0), rt_default=(i == 0), conf=conf, plugin=plugin,
                              tracking="TRACKING" if i % 3 else "NOT_TRACKING", vendor=(i % 2 == 0),
                              warnings=warn, serial="SN%04d" % i if plugin else ""))
    for k in range(32):
        i = k % 16
        r, c = divmod(i, 4)
        owner = "0x%016x" % (0x1000 + i)
        clients.append(client(100 + k, 4000 + k, "app_%02d.exe" % k,
                              window={"left": c * 1920 + 200 + (k // 16) * 300, "top": r * 1080 + 150,
                                      "width": 800, "height": 500},
                              owner=owner, segs=[seg(owner, 0, 800, 500)], paint=1000 + k))
    with open("stress-16x32.ndjson", "w", newline="\n") as f:
        f.write(dump(snap(screens, clients, gen=(3, 99))) + "\n")

    with open("empty-headless.ndjson", "w", newline="\n") as f:
        f.write(dump(snap([], [], gen=(0, 0), source="headless", warnings=[
            {"code": "SERVICE_HEADLESS", "level": "info",
             "text": "No service reached: this is what a process starting now would get; live rows are absent."}]))
                + "\n")


if __name__ == "__main__":
    main()
