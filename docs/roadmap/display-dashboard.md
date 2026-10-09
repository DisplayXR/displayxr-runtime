---
status: Design
owner: David Fattal
updated: 2026-10-09
adr: ADR-051
issues: []
code-paths: [src/xrt/targets/cli/, src/xrt/targets/control_panel/, src/xrt/ipc/, src/xrt/include/xrt/xrt_plugin.h, src/xrt/drivers/sim_display/]
---

# Display dashboard — multi-screen status for the CLI, the Control Panel and agents

The decision record is [ADR-051](../adr/ADR-051-display-status-model-and-dashboard.md).
This document is the design: the snapshot schema, where each field comes from, the feed,
the CLI verbs, the panel pages with mockups, the plug-in slot, phasing, and the open
items that need the vendor side. Vendor names appear here where a concrete product is
meant; the ADR and the code stay neutral.

## 1. Why now, in one paragraph

Multi-screen M6 is on `main`: per-screen weave and look-around for in-process D3D11 /
D3D12 and for the service's direct clients, HWND following the majority screen, the SR
monitor join in the plug-in. The box this was verified on has two 3D panels from one
vendor, each with its own camera, and the only way to know which plug-in claimed which
monitor, whether the second panel's tracker is up, or where a window's seam sits, is to
read `%LOCALAPPDATA%\DisplayXR\*.log` with a grep. `displayxr-cli info` still prints one
"Display processor" block. The Control Panel renders one display. Meanwhile the vendor's
own dashboard shipped a Displays page this week whose design — monitor list backbone,
2 s passive page-scoped feed, warnings as data — is exactly what the runtime needs, with
the runtime's facts instead of the vendor's.

## 2. What exists and where it lives today

| Fact | Where it is | Live? | Reachable from outside the service? |
|---|---|---|---|
| Screen registry: id, flags, confidence, desktop rect, native mode, scale, mm, plug-in, device name, metres, viewer, eye-tracking caps | `xrt_screen_list` (`target_screens_build`), served by `system_enumerate_displays` | re-probe on hotplug, no counter | yes (DIAG) |
| EDID identity, mm, connector, join diagnostics | `os_display_edid_enumerate` | on demand | in-process only |
| Claims: plug-in, confidence, APIs, serial | `target_plugin_resolve_displays` | static per probe | `displays --claims` (loads plug-ins) |
| Plug-in load result, platform state + hint, fallback, ProbeOrder | `target_plugin_get_status` | live in the service | **no** (tray tooltip only) |
| Rendering mode, `is_tracking` of the one service DP | `system_get_display_status` | live | yes, one DP only |
| Clients: id, pid, class, name, active / visible / focused, io, primary, z-order | `system_get_clients`, `get_client_info` | live | yes |
| A client's window rect | `system_get_client_window_metrics` | live | yes |
| A client's segment table + per-segment eyes, `generation` | `compositor_get_segment_metrics` | live | **own session only** |
| Window-handle owner, hand-offs | `comp_segments_owner_*` | live | **log only** |
| Panel lease holder, device REMOVED, DP backend state, swapchain / space counts | `[HEALTH]` every 10 s | live | **log only** |
| Present / skip / split decision and reason | `[RENDER]` every 10 s | live | **log only** |
| GPU topology, settings with provenance, ActiveRuntime | `cli_query` headless | next-launch truth | in-process |
| Vendor per-screen facts: verified, calibrated, tracker alive, lens state, camera paired | vendor process tables / files | live | **vendor dashboard only** |

The first row is the backbone. The rows marked log-only or own-session-only are what the
service RPCs in §5 expose. The last row is what the plug-in slot in §6 carries.

## 3. The snapshot schema

`schema: 1`. Keys are only ever added. Every renderer reads this and nothing else.

```json
{
  "schema": 1,
  "source": "service",                       // "service" | "headless"
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
      "physical_mm": { "width": 344, "height": 194, "source": "plugin" },   // plugin | edid | none
      "roles": { "os_main": true, "runtime_default": true, "vendor_primary": true },
      "claim": { "plugin_id": "leia-sr", "confidence": "VERIFIED", "confidence_value": 100,
                 "serial": "QALA2137AL0011", "apis": ["d3d11", "d3d12", "vk", "gl"] },
      "layout": { "width_m": 0.3442, "height_m": 0.1936,
                  "nominal_viewer_m": { "x": 0.0, "y": 0.10, "z": 0.60 }, "source": "plugin" },
      "eye_tracking": { "supported": ["MANAGED"], "default": "MANAGED",
                        "state": "TRACKING" },                 // TRACKING | NOT_TRACKING | NO_DP | UNKNOWN
      "mode": { "index": 1, "name": "LeiaSR", "views": 2, "is_3d": true },
      "dps": [ { "client_id": 3, "api": "d3d11", "kind": "primary", "backend": "OK" } ],
      "vendor": { "present": true, "ready": true, "verified": true, "calibrated": true,
                  "tracker": "RUNNING", "lens": "3D", "model": "AL", "serial": "QALA2137AL0011",
                  "worst_warning": null, "dashboard_command": "LeiaSRDashboard.exe --page displays --display QALA2137AL0011" },
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
                  "worst_warning": null, "dashboard_command": "…" },
      "warnings": [ { "code": "SEGMENT_FLAT_2D", "level": "warn",
                      "text": "A window spans this screen but its segment has no display processor; that half is flat 2D." } ] }
  ],
  "clients": [
    { "id": 3, "pid": 24416, "class": "APP", "name": "cube_handle_d3d11_win.exe",
      "flags": { "active": true, "visible": true, "focused": true, "overlay": false },
      "presenter": "APP_HWND",                                   // APP_HWND | CLIENT_TEXTURE | NONE
      "lease": "slot",                                           // controller | slot | none
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
```

Field sources, by block:

| Block | Service source | Headless source |
|---|---|---|
| `runtime`, `plugins` | `target_plugin_get_status` in the service | same call in the CLI process |
| `screens[*]` identity, desktop, native, claim, layout, eye-tracking caps | `xrt_screen_list` + `target_plugin_get_monitor_record` | same |
| `screens[*].eye_tracking.state`, `mode`, `dps[]` | the DPs bound to that screen: the service's primary DP and every client's segment DPs (`comp_*_segments_get_eyes`) | `NO_DP` |
| `screens[*].vendor` | plug-in slot `get_screen_status` (§6) | same slot, from the CLI's instance |
| `screens[*].warnings` | derived in `u_status_snapshot_build` (§4) | same |
| `clients[*]` | `ipc_app_state`, `system_get_client_window_metrics`, the new `system_get_client_segments`, the `[HEALTH]` / `[RENDER]` structs | empty |
| `generation` | service counters (§5) | 0 |

### 3.1 `displays --claims --json` is a contract

The vendor dashboard reads `displayxr-cli displays --claims --json` every 5 s while its
Displays page is visible to show "claimed by DisplayXR". That shape is frozen: `schema: 1`
is added at the top, every existing key stays, `serial` is filled from the claim (today it
is empty because the claim's serial is not copied into the JSON), `physical_*_mm` comes
from the EDID reader on every platform (#1877), and new keys are only ever added. The
`status` verb does not replace it.

## 4. Warnings as data

Each warning is `{code, level, text}` with `level ∈ info | warn | critical`. Only `warn`
and `critical` light the panel's tab badge; `info` shows in place. Runtime-derived codes
(vendor codes pass through verbatim under `screens[*].vendor`):

| Code | Level | Condition |
|---|---|---|
| `NOT_NATIVE` | critical | the desktop mode of a claimed screen is not its native mode (a lenticular panel at the wrong resolution never weaves right) |
| `CLAIM_FALLBACK` | warn | a 3D-capable monitor is claimed only at FALLBACK confidence (sim_display took it) |
| `CLAIM_EDID_ONLY` | info | claimed by EDID identity, not verified by the vendor |
| `NO_PHYSICAL_SIZE` | warn | no millimetres from plug-in or EDID: a segment on this screen gets one view set for the whole window (the 0 m trap) |
| `SEGMENT_FLAT_2D` | warn | a live window spans this screen but its segment has no DP |
| `TRACKER_DOWN` | warn | the screen's claim says tracking is supported and a DP is bound, but no DP on it is tracking for more than 5 s while a viewer is tracked elsewhere |
| `PLUGIN_NOT_READY` | critical | the claiming plug-in's platform state is not READY (text = the plug-in's hint, verbatim) |
| `SERVICE_HEADLESS` | info | system-level: snapshot built without a service; live rows are absent |
| `DP_DEGRADED` / `DP_STALE` | warn / critical | a bound DP's backend state |

Texts are one sentence each and say what to do, following the vendor dashboard's
"Title: fix" pattern.

## 5. Service plumbing

Three session-free RPCs on the DIAG class, appended to `proto.json`:

| RPC | Returns | Notes |
|---|---|---|
| `system_get_status_generation` | `{topology, status}` | the poll; two `uint64` |
| `system_get_status_snapshot` | the fixed-size head: runtime, plugins[], screens[] (vendor cell included), workspace, client ids | crosses like `xrt_screen_list` does today |
| `system_get_client_segments(client_id)` | that client's `xrt_segment_metrics` + owner screen + window rect + presenter + lease + integrity counters | read-only; lifts the own-session restriction of `compositor_get_segment_metrics` for DIAG only |

Generation bumps: `topology` on every screen-registry re-probe (the existing
`WM_DISPLAYCHANGE` / `WM_DEVICECHANGE` / registry-change path, the Linux DRM poll, the
macOS reconfigure callback) and on a plug-in state change; `status` on any client
connect / disconnect / focus change, any segment table `generation` move, any owner
hand-off, any lease change, any `is_tracking` edge, any plug-in `change_counter` move, and
on a 2 s timer so a missed event costs at most 2 s. The snapshot is rebuilt lazily on the
first RPC after a bump, not on every bump.

The `[HEALTH]` and `[RENDER]` emitters read from the same structs the snapshot reads, so
the log and the dashboard cannot drift (`emit_health_if_elapsed` becomes a formatter over
`struct service_health`).

Budget: one DIAG slot (quota 4) per live consumer; the panel and the CLI `--watch` are
one consumer each. An elevated consumer cannot connect and gets the headless fallback,
labelled as such.

## 6. The plug-in slot

```c
/*!
 * Vendor summary for ONE screen this plug-in claimed (ADR-051 D2). Appended per
 * ADR-020 (struct_size-gated, no ABI bump); announced by
 * XRT_PLUGIN_IFACE_HAS_GET_SCREEN_STATUS; NULL = "no vendor status".
 *
 * PASSIVE: this call must not create or touch a tracker, lens, display or
 * weaver handle, must not create a per-call vendor instance, and must return
 * in < 20 ms. It may be polled every 2 s by the service and by diagnostic
 * processes. Bump @ref change_counter on the vendor's own device / topology
 * events so a poller can skip an unchanged screen.
 */
xrt_result_t (*get_screen_status)(struct xrt_plugin_instance *inst,
                                  uint64_t monitor_id,
                                  struct xrt_plugin_screen_status *out);

struct xrt_plugin_screen_status {
	uint32_t struct_size;            // sizeof, set by the caller
	uint32_t version;                // 1
	uint64_t change_counter;
	bool ready, verified, calibrated;
	enum xrt_plugin_tracker_state tracker;   // NONE, OFF, STARTING, RUNNING, DOWN, UNSUPPORTED
	enum xrt_plugin_lens_state lens;         // L2D, L3D, UNKNOWN
	char model[32], serial[32];
	uint32_t warning_count;
	struct { char code[32]; uint8_t level; char text[96]; } warnings[8];
	char dashboard_command[160];     // "{serial}" / "{monitor_id}" placeholders; "" = none
};
```

`sim_display` fills `ready = verified = calibrated = true`, `tracker` from
`SIM_DISPLAY_FAKE_TRACKING` (RUNNING, square-waved to DOWN by `_PERIOD_MS`), `lens` from
the mode it is in, `model = "Sim"`, no dashboard command, and one `info` warning saying it
is the simulation. That gives every row of the UI a value on a two-monitor box with no
hardware.

The Leia plug-in would fill it from a handle-free vendor SDK query (§9, open) and bump
`change_counter` from the SDK's device-connected / disconnected / display-not-connected
events, keeping its one long-lived vendor instance.

## 7. The CLI

| Verb | Today | After |
|---|---|---|
| `info [--json]` | static bug-report dump, one display block | unchanged content plus `schema`; the display block is replaced by a pointer to `status` |
| `displays [--json] [--claims]` | EDID list, claims | unchanged shape, `schema: 1`, serial and mm filled |
| `clients [--json]` | client list over DIAG | unchanged; gains `presenter`, `lease`, `window`, `owner_screen` |
| **`status [--json]`** | — | the snapshot, once: service if reachable, else headless (labelled) |
| **`status --watch [--interval ms]`** | — | long-lived; one NDJSON line per generation change, 2 s floor, `--interval` raises it; text mode redraws a table in place |
| `selftest` | per-check PASS/FAIL | one added check: every VERIFIED claim has metres (the 0 m trap), warn not fail |

Text layout of `status` (the table the panel mirrors):

```
DisplayXR 2.32.0 (v2.32.0)  ·  source: service (gen 17/2412)  ·  plug-ins: leia-sr READY, sim-display fallback
SCREENS  2 monitors · 2 claimed (2 VERIFIED) · 2 tracking
 #  device        name                 desktop              mode          claim                 tracking   lens  vendor        warnings
 0  \\.\DISPLAY1  AUO B194             3840x2160 @0,0 x2.5  3840x2160@60  leia-sr VERIFIED      TRACKING   3D    AL  QALA…0011  —
    roles: OS main · runtime default · vendor primary · DPs: client 3 d3d11 primary (OK)
 1  \\.\DISPLAY5  Acer SpatialLabs DS1 3840x2160 @3840,0 x3  3840x2160@60  leia-sr VERIFIED      TRACKING   3D    D1  QI01…0117  —
    roles: — · DPs: client 3 d3d11 segment (OK)
WINDOWS  1 client
 3  cube_handle_d3d11_win.exe  APP  APP_HWND  lease slot  window 3018,285 1664x2093  owner DISPLAY5
    segments gen 41: [DISPLAY1 0,0 822x1875 woven eyes=DP] [DISPLAY5 822,0 842x1875 woven eyes=DP]
    views 2/2 (reported 4)   paint 1811 present 1809 skip 2   weave: scanout
```

## 8. The Control Panel

The panel keeps its rules (asInvoker, links no runtime code, spawns the CLI) and changes
how it spawns it: once, `displayxr-cli status --watch --json`, reading NDJSON from the
child's stdout on a thread, instead of re-running `info` on each click. `Refresh` stays as
a manual kick (`SIGUSR1`-style: the panel closes and respawns the child). The nine
sections become tabs, which the panel's own design already asked for:

```
┌ DisplayXR Control Panel ─────────────────────────────────────────────────────────────┐
│ [Overview] [Displays ●] [Windows] [Performance] [Developer]           source: service │
├──────────────────────────────────────────────────────────────────────────────────────┤
│ Displays   2 monitors · 2 claimed (2 verified) · 2 tracking                 Copy list │
│ ┌──────────────────────────────────────────────────────────────────────────────────┐ │
│ │ ● AUO B194                              [OS main] [runtime default] [vendor primary]│
│ │   \\.\DISPLAY1 · 3840×2160 @ 60 Hz · (0,0) · ×2.5 · 344×194 mm                   │ │
│ │   leia-sr · VERIFIED · QALA2137AL0011 · d3d11 d3d12 vk gl                        │ │
│ │   tracking TRACKING · lens 3D · DP: client 3 d3d11 primary OK                    │ │
│ │   vendor: ready · verified · calibrated · tracker RUNNING     [Open in vendor ↗] │ │
│ ├──────────────────────────────────────────────────────────────────────────────────┤ │
│ │ ▲ Acer SpatialLabs DS1                                                           │ │
│ │   \\.\DISPLAY5 · 3840×2160 @ 60 Hz · (3840,0) · ×3.0 · 340×190 mm                │ │
│ │   leia-sr · VERIFIED · QI012321D10117 · d3d11 d3d12 vk gl                        │ │
│ │   tracking TRACKING · lens 3D · DP: client 3 d3d11 segment OK                    │ │
│ │   vendor: ready · verified · calibrated · tracker RUNNING     [Open in vendor ↗] │ │
│ │   ▲ SEGMENT_FLAT_2D: A window spans this screen but its segment has no display … │ │
│ └──────────────────────────────────────────────────────────────────────────────────┘ │
│ Desktop                                                                               │
│ ┌────────────────────┐┌────────────────────┐   each monitor to scale; live windows  │
│ │ DISPLAY1      ┌────┼┼───┐                │   drawn as outlines with the seam and  │
│ │               │cube│┆   │                │   the owner screen highlighted          │
│ │               └────┼┼───┘                │                                         │
│ └────────────────────┘└────────────────────┘                                         │
└──────────────────────────────────────────────────────────────────────────────────────┘
```

**Overview** keeps today's runtime / DP / GPU-topology / self-test blocks, plus one line
per the vendor dashboard's Home rule: "2 screens · 2 tracking · 1 warning → Displays",
shown only with more than one claimed screen or a warn / critical warning.

**Displays** is the table above. Row order is registry order. The status dot is plain /
ok / warn / critical from the row's worst warning. Chips are the three roles. The badge is
the claim confidence. "Open in vendor" appears only when `dashboard_command` is set. The
desktop map is an ImGui draw-list: monitors to scale, client windows as outlines, the seam
as a dashed line, the owner screen tinted.

**Windows** lists `clients[]`: one card per client with the segment table, the owner, the
per-segment eye source, the view-set counts and, on the same row, the integrity triple
(paint / present / skip) and the weave-placement verdict, never a rate without them
(#1248). A card for a client whose segment has no DP shows the same `SEGMENT_FLAT_2D`
text as the screen row.

**Performance** and **Developer** are the shipped controls and the designed Phase-2 list,
unchanged.

Feed discipline in the panel: the child runs only while the window is visible and not
minimised (SDL window events), the last snapshot is kept through a child restart, "no
service — headless" is a banner, and the tab badge counts warn / critical only.

## 9. Open items on the vendor side (proposals, not commitments)

Agreed with the vendor's runtime session on 2026-10-09; each needs David's go on that
side and is tracked here so the runtime work does not block on it:

1. **A handle-free per-display status query in the vendor SDK**, e.g.
   `srGetDisplayStatus(displayId) → {verified, calibrated, trackerState, lensState,
   warnings[], changeCounter}`, wrapping the vendor's Windows tables and Linux `/run`
   files. The plug-in fills §6 from it. Its dispatch slot must be reserved on
   `#tmp-multi-monitor` first (next-free rule). Until it exists the plug-in fills what it
   already knows (claim, FPC verified, tracker-per-device capability) and leaves
   `tracker` / `lens` UNKNOWN.
2. **The vendor dashboard forwards arguments to its running instance**:
   `LeiaSRDashboard.exe --page displays --display <serial>`; today a second launch only
   raises the existing window. That string is what the plug-in hands over as
   `dashboard_command`.
3. **The vendor's `DISPLAY_TOPOLOGY_CHANGED` event fires on Windows** so the plug-in's
   `change_counter` moves on hotplug without polling. The runtime keeps its own
   `WM_DISPLAYCHANGE` path regardless.
4. **The reverse read**: the vendor dashboard's planned "claimed by DisplayXR" column
   reads `displays --claims --json` (§3.1); the shape is frozen for it.

Polling rules the plug-in must follow, in its own words from the vendor session: one
long-lived SR instance (every instance is a client of the vendor service); only the
passive enumerate / capabilities / config-resolve calls on the 2 s tick; never a tracker,
lens or display handle per tick (a tracker handle wakes that screen's camera; a lens
enable is a shared vote that `destroy` does not withdraw); config resolve at most every
10 s.

## 10. Phasing

| Phase | Content | Depends on |
|---|---|---|
| **0** landed | `displays --claims --json`: `schema`, claim serial, mm; `selftest` metres check | — |
| **1** | `xrt_display_status.h`, `u_status_snapshot_build` (headless), `displayxr-cli status [--json]`, warnings §4; `service_health` struct behind `[HEALTH]` / `[RENDER]` | — |
| **2** | the three DIAG RPCs, generation counters, `status --watch`; `clients` gains presenter / lease / window / owner | 1 |
| **3** | plug-in slot `get_screen_status` + `sim_display` implementation + iface doc | 1 |
| **4** | Control Panel: tabs, long-lived `--watch` child, Displays + Windows pages, desktop map, Overview summary line | 2 |
| **5** | MCP `get_status_snapshot` (session-free, wraps the CLI) | 2 |
| **6** | the vendor plug-in fills the cell from the SDK query; "Open in vendor" | 3 + §9.1–9.2 |

Phases 1–3 are runtime PRs with CI coverage (`tests_ipc_proto.py` for the appended
messages, a `tests_status_snapshot` unit test over a synthetic registry, the headless
`selftest` gate). Phase 4 is a panel PR verified on the two-panel rig and on a two-monitor
sim_display box. Nothing in 0–5 waits on the vendor.

## 11. Non-goals

- Changing claims from the dashboard (per-screen `PreferredPlugin` is a separate,
  unimplemented roadmap item; the global override stays on the Overview tab).
- Per-screen settings with provenance: that is the vendor dashboard's model and stays
  there; the runtime's own settings keep the Performance / Developer tabs.
- Inter-display pose or a space-graph view (display spatial model, #46).
- A web UI: the snapshot is the contract, so one can be added later without touching the
  service.
- Any action that wakes a camera or votes on a lens.
