---
status: Proposed
date: 2026-10-09
deciders: David Fattal
issues: []
related: [ADR-019, ADR-020, ADR-035, ADR-045, ADR-047]
---

# ADR-051: Display status — one vendor-generic model, the service owns the truth, the panel stays dumb

## Context

Multi-screen shipped (ADR-047 and its three amendments): a box can carry several 3D
screens from one or more vendor plug-ins, a window spanning two of them is woven per
screen, each half tracked by its own camera, and the service does the same for its direct
clients. None of that is visible anywhere a person can look at it.

What exists today:

- `displayxr-cli info` prints one "Display processor" block for the **active** plug-in's
  **one** device, then a per-screen list (`displays`) that is a flat EDID dump. Claims
  (`displays --claims`) are a third, separate listing. Nothing is live: every call loads
  every registered plug-in in the CLI's own process and reports *what that process would
  see*, which is not what the service or the running apps see (the Control Panel design
  already names this as "why the CLI is misleading today").
- The Control Panel (`targets/control_panel`, SDL2 + cimgui) is deliberately dumb: it
  spawns `displayxr-cli … --json` on a Refresh click and renders. It shows one display,
  does not render the per-screen array it already receives, and knows nothing about
  clients, windows or segments.
- The service holds the truth — the screen registry it serves to every client, the
  per-client segment tables with their `generation` counters, the window-handle owner,
  the presenter lease, the plug-in's platform state, the DP backend health — and
  publishes it only as `[HEALTH]` / `[RENDER]` / `segments:` log lines every 10 s. A
  DIAG client can enumerate displays and clients, but not another client's segments, not
  the plug-in state, not the lease.
- Per-screen vendor facts (verified / calibrated, a tracker alive for that screen, the
  lens state, a camera paired) exist only inside the vendor's own process tables and its
  own dashboard. The runtime gets the static claim (confidence, serial) and, per DP,
  `is_tracking`.

The vendor's own dashboard, which David pointed at as the model, solved the same problem
on its side this week: a monitor-list backbone with vendor facts joined on, a page-scoped
2 s passive feed that never wakes a camera, warnings as data with a nav badge, and a
one-line summary on its home page. That design is the right shape; the runtime must not
copy its *data* (vendor tables are not a contract), only its *model and discipline*.

Two constraints frame everything below:

- **ADR-019 / separation of concerns.** The runtime links no vendor code; a new vendor
  adds no file here. Vendor facts reach the runtime only through the plug-in interface,
  and reach the panel only as opaque strings, codes and capability bits.
- **ADR-020 append-only.** New vendor facts need a new appended slot, implemented by
  `sim_display` too, so a two-monitor box with no hardware exercises every row of the
  dashboard.

## Decision

### D1 — One status model, owned by the runtime

A single vendor-generic snapshot type, `xrt_status_snapshot`, defined in
`src/xrt/include/xrt/xrt_display_status.h` and assembled by one function,
`u_status_snapshot_build()`, from sources that already exist. It is the **only** thing
the CLI, the panel, the MCP surface and any future web page ever render.

```
xrt_status_snapshot
  schema            1                         bumped only on an incompatible change
  generation        topology_gen, status_gen  monotonic; see D3
  source            SERVICE | HEADLESS        where this snapshot was built
  runtime           version, git tag, ABI, active OpenXR runtime
  plugins[]         id, name, vendor, version, load result, platform state, hint, fallback
  screens[]         one per registry screen (D2 below for the vendor cell)
  clients[]         one per IPC client: id, pid, class, name, presenter kind,
                    window rect, lease holder, segments[], owner screen, eye source per segment
  workspace         on/off, controller
  warnings[]        system-level {code, level, text}
```

`screens[]` is the backbone. Rows are **OS monitors**, in registry order, with the system
default first; a vendor screen that is not on the desktop still gets a row. Each row joins
the facts the runtime already has: EDID identity and millimetres, desktop rect and scale,
native mode and refresh, the claim (plug-in id, confidence VERIFIED / EDID / FALLBACK,
serial), which DP factories the claim offers per graphics API, the nominal viewer and
metres the segment layout uses, eye-tracking capability, the live tracking state of any DP
currently bound to it, the rendering mode it is in, and per-screen warnings.

Three "primary" notions stay distinct and are shown as separate chips: the **OS main**
monitor, the **runtime default** screen (`SYSTEM_DEFAULT`, where a non-spanning window
lands), and the **vendor primary** if the vendor reports one.

### D2 — Vendor facts come through one appended slot, as a status cell, never as tables

`xrt_plugin_iface` gains one append-only slot (ADR-020: `struct_size`-gated, no ABI bump,
NULL-safe, with a `XRT_PLUGIN_IFACE_HAS_GET_SCREEN_STATUS` macro):

```
xrt_result_t (*get_screen_status)(struct xrt_plugin_instance *inst,
                                  uint64_t monitor_id,
                                  struct xrt_plugin_screen_status *out);   // sized, versioned
```

`xrt_plugin_screen_status` is the **vendor summary cell** and nothing more: `ready`,
`verified`, `calibrated`, `tracker_state` (NONE / OFF / STARTING / RUNNING / DOWN /
UNSUPPORTED), `lens_state` (2D / 3D / UNKNOWN), `model` / `serial` strings, up to eight
`warnings[] {code, level, text}` shown verbatim, a `change_counter`, and a
`dashboard_command` template (`"<exe> --page displays --display {serial}"`) the panel
may launch and never parses. The runtime does not know what "calibrated" means for a
given vendor; it only knows how to show it.

Rules the slot carries in its doc comment, because the vendor dashboard learned them the
hard way: the call must be **passive** (no handle that wakes a camera or holds a lens; no
per-call vendor instance; sub-20 ms), it may be polled at 2 s, and it must bump
`change_counter` on the vendor's own device / topology events so the runtime can skip the
rebuild when nothing moved. `sim_display` implements it with synthetic values, and
`SIM_DISPLAY_FAKE_TRACKING_PERIOD_MS` drives its `tracker_state` edge so the live UI can be
tested with no hardware.

What stays vendor-side, by design: configuration keys and their provenance, pairing rules,
calibration files, tracker ports and restarts, camera previews, lens tests, and any action.
The runtime dashboard shows status; it offers one link into the vendor dashboard for the
rest.

### D3 — The service owns the truth; one session-free DIAG RPC serves it

The authoritative snapshot is assembled **in the service**, next to
`ipc_handle_system_enumerate_displays`, on the 20 Hz main loop: it is rebuilt when any
input generation moves (screen registry re-probe, a client's `xrt_segment_metrics.generation`,
the owner state machine, the plug-in's `change_counter`, lease or client-list changes) and
at least every 2 s otherwise. Two counters cross the wire so a consumer can poll cheaply:
`topology_gen` (screens, plug-ins) and `status_gen` (everything else).

New session-free RPCs on the DIAG path (appended to `proto.json`, under the 1 KB message
budget by returning the snapshot's fixed-size parts and the per-client segment tables by
index, exactly as `system_enumerate_displays` and `compositor_get_segment_metrics` already
cross): `system_get_status_generation`, `system_get_status_snapshot`,
`system_get_client_segments(client_id)`. The last one lifts the "own session only"
restriction of `compositor_get_segment_metrics` for DIAG callers; it is read-only.

The `[HEALTH]` fields that today exist only in the log (panel lease holder, device
REMOVED, DP backend state, per-client swapchain / space counts) move into a struct that
both the log line and the snapshot read, so the dashboard and the log can never disagree.
This closes ADR-035 D7's planned per-client health RPC and the Control Panel design's
Phase 3 "live service values over the existing DIAG path" in one place.

**Headless fallback.** When no service is reachable (none running, or the caller is
elevated and cannot connect), `u_status_snapshot_build()` runs in-process from the
headless instance, exactly as `cli_query` does today, and the snapshot says
`source = HEADLESS`. The panel renders that word prominently: a headless snapshot is "what
a process starting now would get", not what the apps see.

### D4 — The CLI is the brain; the panel, MCP and any web page are its renderers

`displayxr-cli` gains one verb, `status`, and the existing per-topic verbs keep their
shapes:

- `displayxr-cli status [--json]` — the snapshot, once. Text output replaces today's
  single-display "info" block with the per-screen table; `info` stays for the static
  bug-report dump and gains a `schema` field.
- `displayxr-cli status --watch [--interval ms]` — a long-lived process writing one JSON
  line per change (NDJSON), driven by the service's generation counters, 2 s floor, with
  the topology-change wake (D6). This is what the panel spawns once and reads from, instead
  of re-spawning the CLI and re-loading every plug-in per refresh.
- `displayxr-cli displays --claims --json` is **a published contract**: the vendor
  dashboard reads it to show "claimed by DisplayXR". It gets a `schema` field, keeps every
  existing key, and only ever adds keys. Its two known gaps — the claim `serial` is empty
  and `physical_*_mm` is 0 on Windows — are fixed as part of this work.

The Control Panel keeps the #378 rule ("the GUI is dumb, the CLI is the brain") and the
ADR-019 process boundary: it links no runtime or vendor code, it renders the snapshot. It
gets the tabs its own design asked for, and two new ones, *Displays* and *Windows*
(`docs/roadmap/display-dashboard.md`). The session-free MCP tool `get_runtime_status`
grows a sibling `get_status_snapshot` that returns the same JSON, so an agent sees the
same truth as the human.

### D5 — The feed discipline is part of the contract

Borrowed from the vendor dashboard and binding on every renderer:

1. **Passive only.** A status read never creates a tracker, lens, display or weaver
   handle anywhere: not in the service, not in the plug-in, not in the panel.
2. **Page-scoped.** A renderer holds the feed only while a page that shows it is visible;
   hidden or minimised releases it. The service rebuilds nothing for nobody.
3. **Generation-driven.** Consumers poll the two counters and fetch the body only when one
   moved; the service bumps them from events, with a 2 s floor so a missed event costs at
   most 2 s.
4. **Keep the last snapshot through a reconnect**, show "no service" only after a timeout,
   and drop it on release so a badge never shows stale data.
5. **Never a performance number without an integrity number beside it** (#1248): where a
   renderer shows present or weave counts it shows the paint / present / skip triple and
   the `weave placement` verdict on the same row.

### D6 — Topology changes are events on every platform

Windows: the service already re-probes on `WM_DISPLAYCHANGE` / `WM_DEVICECHANGE`; that
path now also bumps `topology_gen`. Linux: the existing DRM poll. macOS: the CoreGraphics
reconfiguration callback. The vendor plug-in may bump its `change_counter` from its own
events; the runtime does not depend on it and keeps the 2 s floor, because at least one
vendor runtime never raises its topology event on Windows today.

### D7 — Vendor neutrality of the link out

The "open in vendor dashboard" affordance is a command template handed over by the
plug-in (D2), with `{serial}` and `{monitor_id}` placeholders, launched by the panel
with `ShellExecute` / `xdg-open` semantics and never parsed. A plug-in that has no
dashboard leaves it empty and the button does not appear. The runtime repo contains no
vendor executable name.

## Consequences

- **One model, four renderers.** CLI text, CLI JSON, the panel and MCP all read
  `xrt_status_snapshot`; a fact added once appears everywhere. The `displays --claims`
  JSON becomes a cross-vendor contract and is versioned accordingly.
- **The service grows a small amount of status plumbing** (struct, three RPCs, generation
  bumps) and loses nothing: the log lines stay, now reading from the same struct.
- **One appended plug-in slot**, implemented by `sim_display` and documented with the
  passive-polling rules. Vendors implement it when they have something to say; a NULL
  slot renders as "no vendor status".
- **The panel stops loading plug-ins per refresh** and stops being single-display. It
  needs tabs; that was already its next step.
- **Not decided here:** per-screen `PreferredPlugin` (the roadmap's per-monitor override
  is still unimplemented and this dashboard shows, but does not change, claims);
  per-screen eye-tracking mode ownership (multi-screen roadmap §6.8); inter-display pose
  (display spatial model, #46) — the row model leaves room for a fidelity column.
- **Open items for the vendor side**, recorded for the design pass, not committed: a
  handle-free per-display status query in the vendor SDK that the plug-in would wrap for
  D2; a dashboard command line that forwards `--page` / `--display` to its running
  instance for D7; raising the vendor's topology event on Windows for D6.

## Alternatives considered

- **Keep extending `displayxr-cli info`.** Rejected: it is headless by construction and
  cannot become live without pretending to know what the service decided.
- **A service-side MCP server as the primary feed.** Rejected as primary: it would duplicate
  the DIAG data path; accepted as a thin wrapper over the same snapshot.
- **Read the vendor's own status tables / files from the runtime.** Rejected: their layouts
  are internal, differ per OS, and reading them would put vendor knowledge in the runtime
  (ADR-019).
- **A per-screen callback instead of a change counter.** Deferred: the 20 Hz main loop
  already polls cheap counters; a callback adds a thread boundary into the plug-in for no
  latency the UI can show.
- **Rendering the dashboard in the vendor's dashboard.** Rejected as the only surface:
  a mixed-vendor box has two of them, and the runtime view (claims, DPs, segments,
  clients) is the runtime's to show. Each side shows one cell of the other's data and
  links across.
