#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
"""Writes the Catalog tab's JSON fixtures:  python gen_catalog_fixtures.py ..   (the src/dashboard directory)
The shapes are written by hand from the emitter, not captured from an engine run. Field names and order follow the engine's emitter
(src/xrt/targets/get/get_core.c, dxr_list_catalog / dxr_list_receipts / report_offline)."""
import json, sys, os

root = sys.argv[1]  # src/dashboard


def item(id, name, cls, *, registered_id=None, publisher="displayxr", tier="listed", installed=None, available=None,
         state="not_installed", installable=True, receipt=False, listing=None, summary=None, aliases=(),
         recommended=False, detected="none", evidence=None, size=0, download=None, compatible=True,
         incompatible_reason=None, prerequisites=(), can_uninstall=None, **extra):
    x = {"id": id, "name": name, "class": cls}
    if registered_id:
        x["registered_id"] = registered_id
    x["publisher"] = publisher
    x["tier"] = tier
    x["installed"] = installed
    x["available"] = available
    x["state"] = state
    x["installable"] = installable
    x["receipt"] = receipt
    # The receipt recorded an uninstall command (false for an install from an entry that names none).
    x["can_uninstall"] = receipt if can_uninstall is None else can_uninstall
    if listing:
        x["listing"] = listing
    if summary:
        x["summary"] = summary
    x["aliases"] = list(aliases)
    x["recommended"] = recommended
    x["detected"] = detected
    if evidence:
        x["evidence"] = evidence
    x["size"] = size
    if download is None:
        download = size if state in ("not_installed", "outdated", "rebuilt") and installable and compatible else 0
    x["download"] = download
    x["compatible"] = compatible
    if not compatible:
        x["incompatible_reason"] = incompatible_reason
    x["prerequisites"] = list(prerequisites)
    x.update(extra)
    return x


def write(path, doc):
    p = os.path.join(root, path)
    with open(p, "w") as f:
        json.dump(doc, f, indent=1, ensure_ascii=False)
        f.write("\n")


EDID = "display EDID 44048/16711 DELL_8K32 (high confidence)"
CONV_RUNTIME = {"name": "Leia conversion runtime", "met": True, "installable": False}
LEAP = {"name": "Ultraleap tracking software", "met": False, "installable": False,
        "info_url": "https://www.ultraleap.com/downloads/"}

# --- schema 2: the generated catalog's six items (scripts/gen_catalog.py V2_ITEMS), on a Leia machine
v2 = {"schema": 2, "items": [
    item("runtime", "DisplayXR Runtime", "runtime", installed="2.70.0", available="2.70.0", state="current",
         size=50034808),
    item("leia-sr", "Leia SR display processor", "display_processor", registered_id="leia-sr", installed="2.22.0",
         available="2.23.0", state="outdated", receipt=True, aliases=["leia_plugin"], detected="high", evidence=EDID,
         size=31457280),
    item("browser", "DisplayXR Browser", "app", installed="1.10.5", available="1.10.5", state="current",
         size=213909504),
    item("open-conversion", "Open conversion provider", "conversion_provider", registered_id="open-conversion",
         available="0.3.0", summary="2D-to-3D conversion with open models. Needs no vendor software.",
         aliases=["conversion_open"], recommended=True, size=188743680),
    item("leia-conversion", "Leia conversion", "conversion_provider", registered_id="leia-conversion",
         installed="0.5.0", available="0.5.0", state="current", receipt=True,
         summary="2D-to-3D conversion from the display maker.", aliases=["leia_conversion"], size=9437184,
         prerequisites=[CONV_RUNTIME]),
    item("ultraleap", "Ultraleap hands", "input_provider", registered_id="ultraleap", available="1.2.0",
         summary="Hand tracking from an Ultraleap camera, as controllers and hand joints.", size=4194304,
         prerequisites=[LEAP]),
]}
write("DisplayXR.Dashboard.Tests/Data/catalog-list-v2.json", v2)

# --- schema-1 fallback: the three schema-1 components, converted by the engine. No summary, no
# publisher, no recommended flag, no prerequisites; the Leia plug-in found by detection.
v1 = {"schema": 1, "items": [
    item("runtime", "DisplayXR runtime", "runtime", publisher="", installed="2.64.0", available="2.70.0",
         state="outdated", size=50034808),
    item("leia-sr", "Leia display plug-in", "display_processor", registered_id="leia-sr", publisher="",
         available="2.23.0", aliases=["leia_plugin"], detected="high", evidence=EDID, size=31457280),
    item("browser", "DisplayXR Browser", "app", publisher="", installed="1.10.5", available="1.10.5", state="current",
         size=213909504),
]}
write("DisplayXR.Dashboard.Tests/Data/catalog-list-v1.json", v1)

write("DisplayXR.Dashboard.Tests/Data/catalog-offline.json",
      {"offline": True, "reason": "catalog unreachable or expired", "skipped": ["list"], "retry": "dxr-get list"})

write("DisplayXR.Dashboard.Tests/Data/catalog-installed.json", {"installed": [
    {"id": "leia-sr", "name": "Leia SR display processor", "class": "display_processor", "publisher": "displayxr",
     "version": "2.22.0",
     "source_url": "https://github.com/DisplayXR/displayxr-leia-plugin/releases/download/v2.22.0/DisplayXRLeiaSRSetup-2.22.0.exe",
     "sha256": "0f3a0c1d6b6a4d0e9b8f1c2d3e4f5a6b7c8d9e0f1a2b3c4d5e6f7a8b9c0d1e2f", "install_time": "2026-11-18T09:12:44Z",
     "can_uninstall": True,
     "uninstall_command": "\"C:\\Program Files\\DisplayXR\\Runtime\\Plugins\\LeiaSR\\Uninstall.exe\" /S"},
    {"id": "open-conversion", "name": "Open conversion provider", "class": "conversion_provider",
     "publisher": "displayxr", "version": "0.3.0",
     "source_url": "https://github.com/DisplayXR/displayxr-conversion-open/releases/download/v0.3.0/DisplayXRConversionOpenSetup-0.3.0.exe",
     "sha256": "9c1e5b7a3d2f4e6081a2b3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f701", "install_time": "2026-11-19T17:03:10Z",
     "can_uninstall": False, "uninstall_command": ""},
]})

# --- the layout fixture (--fixture, beside components.ndjson): every state the tab has to draw.
# Items with made-up publishers stand for third-party listings; "fixture_*" keys drive the simulated
# engine's unhappy paths (Feed/FixtureCatalogEngine.cs).
layout = {"schema": 2, "items": v2["items"][:2] + [
    item("acme-xr-panel", "Acme Spatial 27 display plug-in", "display_processor", registered_id="acme-xr-panel",
         publisher="acme", available="1.4.0", summary="The display plug-in for Acme Spatial monitors.",
         size=22020096, compatible=False,
         incompatible_reason="acme-xr-panel needs runtime >= 2.75.0, 2.70.0 is installed"),
    v2["items"][5],
    v2["items"][3],
    v2["items"][4],
    item("acme-depth", "Acme Depth Studio", "conversion_provider", registered_id="acme-depth", publisher="acme",
         installed="3.1.0", available="3.1.0", state="current", receipt=True,
         summary="Studio-grade 2D-to-3D conversion for film and photo work.", size=524288000),
    item("northlight-3d", "Northlight Live 3D", "conversion_provider", registered_id="northlight-3d",
         publisher="northlight", installed="1.0.2", available="1.0.2", state="current", receipt=True,
         can_uninstall=False, fixture_no_uninstall=True,
         summary="Real-time 3D for video calls and streams.", size=73400320),
    item("open-tracking", "Open camera tracking", "tracking_provider", registered_id="open-tracking",
         installed="0.2.0", available="0.2.0", state="current", receipt=True,
         summary="Look-around from an ordinary camera, for displays that have no eye tracker of their own.",
         size=41943040),
    item("shell", "DisplayXR Shell", "workspace_controller", registered_id="shell", available="2.4.3",
         summary="A spatial desktop: place, resize and switch your 3D apps side by side.", size=68157440),
    v2["items"][2],
    item("media-player", "DisplayXR Media Player", "app", available="1.17.0",
         summary="Plays 3D photos and video, and converts 2D ones as they play.", size=96468992,
         fixture_exit=20),
    item("model-viewer", "DisplayXR Model Viewer", "app", available="1.6.0",
         summary="Opens glTF and OpenPBR models in depth.", size=58720256, fixture_prompt="declined"),
    item("acme-anatomy", "Acme Anatomy Atlas", "app", publisher="acme", installable=False,
         listing="https://store.example.com/anatomy-atlas",
         summary="An interactive 3D atlas for teaching and study."),
    v2["items"][0],
]}
write("fixtures/catalog-list.json", layout)


def provider(id, name, vendor, licence=None, tasks=("depth",)):
    p = {"id": id, "name": name, "vendor": vendor, "version": "1.0.0", "load": "ACTIVE", "enabled": True,
         "enabled_source": "default", "provenance": "signed", "signer": vendor, "built_in": False}
    if licence:
        p["licence"] = licence
    return p


def licence(state, source=None, expires=None, message="", purchase_url="", key=False, deep_link=False):
    return {"state": state, "source": source, "expires_unix": None, "expires": expires, "message": message,
            "purchase_url": purchase_url, "activation": {"key": key, "deep_link": deep_link},
            "blocks": state in ("unlicensed", "expired")}


write("fixtures/conv-list.json", {
    "schema": 1, "source": "headless", "policy": {"policy_lock": False, "preferred": {}},
    "providers": [
        provider("leia-conversion", "Leia conversion", "Leia, Inc.",
                 licence("licensed", "hardware", message="Included with your Leia display")),
        provider("acme-depth", "Acme Depth Studio", "Acme",
                 licence("trial", "trial", "2026-12-21T00:00:00Z", "30-day trial, every feature unlocked",
                         "https://store.example.com/depth-studio", key=True, deep_link=True)),
        provider("northlight-3d", "Northlight Live 3D", "Northlight",
                 licence("unlicensed", None, None, "Activate to start converting",
                         "https://northlight.example.com/buy", key=True)),
    ],
    "tasks": [
        {"task": "depth", "provider": "leia-conversion", "selected": True, "verdict": "selected"},
        {"task": "depth", "provider": "acme-depth", "selected": False, "verdict": "ready"},
        {"task": "depth", "provider": "northlight-3d", "selected": False, "verdict": "unlicensed"},
    ]})

write("fixtures/tracking-list.json", {
    "preferred": None, "policy": {"policy_lock": False}, "disabled_by_env": False,
    "providers": [
        dict(provider("open-tracking", "Open camera tracking", "DisplayXR",
                      licence("expired", "key", "2026-10-31T00:00:00Z", "Your licence ended on 31 October",
                              "https://displayxr.example.org/tracking", key=True)),
             display_name="Open camera tracking", preferred=False, first=True),
    ]})
print("ok")
