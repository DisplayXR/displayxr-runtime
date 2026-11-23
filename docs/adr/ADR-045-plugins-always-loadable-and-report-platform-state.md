# ADR-045: Plug-ins are always loadable and report their platform state

**Status:** Proposed (2026-10-02) · extends [ADR-019](ADR-019-vendor-plugin-aux-boundary.md) and
[ADR-020](ADR-020-plugin-abi-compatibility-policy.md) (append-only slot, ABI unchanged) · epic
[#1803](https://github.com/DisplayXR/displayxr-runtime/issues/1803) (#1804, #1805) · spec:
[plugin-discovery.md §4.1–4.2](../specs/runtime/plugin-discovery.md)

## In one paragraph

Three things are installed independently on a DisplayXR machine: the **runtime**, a **vendor
plug-in**, and the **vendor platform runtime** that plug-in drives (plus, optionally, a vendor
conversion runtime). Users install them in any order, uninstall them in any order, and plug the 3D
display in or out at any time. The runtime must reach the right state in every case **without
knowing which vendor it is dealing with**. It does so with three rules. A registered plug-in is a
fact. A plug-in is always loadable, and it tells the runtime, in generic terms, why it cannot
serve the display right now. The long-lived service re-evaluates its choice when the world changes,
but it never takes the display away from a vendor plug-in that is already serving it.

## Context

Before this decision, every one of these orders had a failure mode, and each failed silently:

| Situation | Before |
|---|---|
| Runtime + plug-in installed, vendor platform installed **later** | The plug-in imported the platform's libraries statically, so the OS loader refused it. The runtime silently fell back to the simulation display. A service started before the platform keeps its logon-time `PATH`, so it kept failing until it was restarted. |
| Vendor platform installed, its service **not yet running** at service start | The plug-in declined. A re-probe happened only on the next **client** connect. When it did adopt the plug-in, the adoption was partial: the weaving DP and the geometry switched, but the head device (mode table, eye tracking) stayed the fallback's. |
| Display **not connected** at service start | Probe could block for many seconds waiting for the platform, and it was re-tried only on client events. Plugging the display in later did nothing until an app launched. |
| Display **disconnected** while running | Nothing noticed. |
| Any degraded state | Nothing in the tray or the Control Panel. Only `displayxr-cli selftest` and the logs showed it. |

There were three root causes. Load-time coupling of plug-in and platform. Selection that was
re-evaluated only on client events, with a probe that could block. No vocabulary for a plug-in to
say "I am fine, my platform is not".

## Decisions

### D1. A registered plug-in is a fact, not a decision

The `DisplayProcessors` registration (registry key / manifest) records that a plug-in is installed.
It is not a claim that the hardware is present. The runtime never deletes or rewrites a vendor
registration because the plug-in cannot be used right now. An **orphan** registration (its binary
is gone) is skipped with one WARN per process. The runtime installer owns only its own fallback
registration (installer work item, same epic).

### D2. Plug-ins are loadable without their platform, and report a generic platform state

Every plug-in binary must load when its vendor platform is absent. It resolves vendor libraries
lazily, by a path it derives itself, and never relies on the host process's `PATH`. It also reports
its state through a new optional slot, `xrt_plugin_iface::get_platform_state`. The slot is appended
per ADR-020, at an unchanged ABI, and carries `XRT_PLUGIN_HAS_PLATFORM_STATE`:

| State | Meaning |
|---|---|
| `READY` | Platform installed, running, and its display attached. |
| `PLATFORM_ABSENT` | The vendor platform runtime is not installed. |
| `PLATFORM_NOT_RUNNING` | It is installed, but its service is not running. |
| `NO_DISPLAY` | The platform is up, but none of its displays is attached. |
| `INCOMPATIBLE` | The platform is present but unusable (version, OS, GPU). |
| `UNKNOWN` | Not reported: an older plug-in, or the call returned false. |

Each state comes with a short **hint** that the vendor writes ("install the … runtime", "connect
the display"). The runtime shows the hint verbatim and never parses it. A `FALLBACK` flag marks a
plug-in that claims any system; today that is only the in-tree simulation display.

The slot takes no instance, so it can be called **before `probe()`**. The loader sequence is
load → negotiate → `get_platform_state` → `probe`, which lets a plug-in that is about to decline
say why. Both calls are presence checks only, with a budget of about 100 ms. They must never wait
for the platform: readiness waits belong in device or DP creation, or on a thread the plug-in owns.
The state is advisory. It never gates loading, and the runtime acts on it only generically.

### D3. Re-select on world events, but never swap a live vendor plug-in

The service re-evaluates selection on four triggers:
- display topology changes (`WM_DISPLAYCHANGE`);
- device-node changes (`WM_DEVICECHANGE` / `DBT_DEVNODES_CHANGED`);
- changes under the registration root (`RegNotifyChangeKeyValue`);
- a 10 s timer, only while the active plug-in is the fallback.

Re-probes are debounced (one refresh at least 1 s after the last event of a burst) and run on a
worker thread, never on the window thread or the IPC main loop. In-process apps do not re-probe on
world events: each app process selects once.

A refresh adopts a better-ranked plug-in **only while the active one is the fallback**. While a
vendor plug-in is active, the refresh changes nothing, even when that plug-in reports `NO_DISPLAY`.
The runtime surfaces the state instead, and the plug-in's DP passes pixels through unwoven. We
rejected a live swap between two DPs mid-session (for example, falling back to the simulation
display when the panel is unplugged). It would tear down live sessions' weavers and their
mode-table snapshots for a state that is usually transient (a cable, sleep, a monitor input switch).

### D4. Adoption is complete, through a restart when idle

When a refresh adopts a vendor plug-in, the weaving DP and the display info follow it immediately,
as before. The **head device** cannot follow it live. That device is the fallback's: its rendering
modes, its eye tracking, its pose binding. The system compositor holds it, it sized the worst-case
atlas, and every connected client holds a shared-memory snapshot of its mode table. Instead, the
system marks the head as stale. Once no client has been connected for 2 s, the service ends its
main loop and starts a successor of itself. The successor waits for the old process to exit, then
builds its whole system on the new plug-in. Clients that were connected keep running on the
partial adoption until they reconnect. A successor never restarts itself for the same reason again,
so a plug-in whose probe flaps cannot cause a restart loop.

### D5. Surface the state everywhere a user looks

- `displayxr-cli info` and `selftest` print one line per registered plug-in: the registration's own
  name and version, its platform state and hint, and the loader outcome. The outcomes are `ACTIVE`,
  `LOADED`, `DECLINED`, `BINARY_MISSING`, `DEPENDENCY_MISSING`, `ABI_MISMATCH`, and so on. The
  `vendor_dp` self-test note carries the rejected plug-in's state. Exit codes do not change.
- The service tray tooltip names the active display processor. When that is the fallback, it adds
  the reason the better-ranked plug-in gave. When the active plug-in reports `NO_DISPLAY`, it says
  so.
- The Control Panel lists the same per-plug-in lines and highlights the degraded ones.

## Consequences

- A vendor plug-in built against older headers keeps working unchanged. It reports `UNKNOWN`, and
  its load failures are still classified (binary missing vs dependency missing vs other).
- The fallback decision is made from the plug-in's own flag. For a simulation plug-in too old to
  report state, the decision falls back to the runtime's own simulation id. It is never made from a
  vendor id.
- Repeated identical load failures during re-probes log at INFO after the first WARN, so a box
  whose vendor platform is absent costs one WARN per process, not one every 10 s.
- The service can now restart itself, once, to complete an adoption. That is a new lifecycle event.
  It is logged ("plug-in adoption: no client connected — restarting the service …") and happens
  only after the fallback was active and a vendor plug-in was adopted.

## Amendment 1 (2026-11-22): the platform remedy — "update recommended" / "update required"

**What changed.** A plug-in can now say that its vendor platform has an update, and where to get
it, through the same `get_platform_state` call. Two flags join `FALLBACK` in
`xrt_plugin_platform_status::flags`, and four strings are appended to the struct:

| Addition | Meaning |
|---|---|
| `XRT_PLUGIN_PLATFORM_FLAG_UPDATE_RECOMMENDED` (`1u << 1`) | The platform works; an update is available and recommended. |
| `XRT_PLUGIN_PLATFORM_FLAG_UPDATE_REQUIRED` (`1u << 2`) | The platform needs an update. Wins when both bits are set. |
| `char installed_version[32]` | The platform version installed now. |
| `char required_version[32]` | The version the update brings, or the minimum the plug-in needs. |
| `char action_label[64]` | A short label for the action, e.g. "Download the update". |
| `char action_url[256]` | Where to get it. Absolute `https://` only. |

The strings are UTF-8, NUL-terminated and empty when absent. The vendor writes them, and the runtime
shows them verbatim, the same way it treats the hint. `XRT_PLUGIN_HAS_PLATFORM_REMEDY` marks a
header that has all of this.

**The rule: advisory only.** The runtime never gates loading, selection, adoption or re-probe on
either bit. `UPDATE_RECOMMENDED` changes nothing but the notice. `UPDATE_REQUIRED` is
informational too. If a platform is too old to run at all, the plug-in must also report a non-READY
state (normally `INCOMPATIBLE`) and decline its probe. The state is what the runtime acts on (D2);
the bit is what it tells the user. This keeps the vendor's judgment about versions inside the
vendor's binary, which is the whole idea of D2.

**Growth without an ABI bump.** The 64 bytes of `reserved[8]` cannot hold a URL, so the strings are
appended *after* `reserved`, and `reserved` and `reserved_0` stay as they are. The struct's
contract always allowed this: the runtime sets `struct_size` to its own size and zero-fills, and the
plug-in writes nothing past `struct_size`. A plug-in writes each new field only when
`XRT_PLUGIN_PLATFORM_STATUS_HAS(out_status, field)` is true. As a result:

- A newer runtime with an older plug-in gets zero-filled fields, which reads as no remedy.
- An older runtime with a newer plug-in passes the old `struct_size`, so the plug-in writes nothing
  new.

`XRT_PLUGIN_API_VERSION_CURRENT` does not change. `tests_plugin_platform_remedy` pins the offsets.

**What the loader does.** It forces every string to be NUL-terminated. It keeps only the two remedy
bits and drops the strings when neither bit is set. It drops an `action_url` that is not a plain
absolute `https://` URL (no spaces, quotes, backslashes, angle brackets or control characters) and
logs that at DEBUG; the notice still shows, without the link. The sanitised remedy goes in the
per-plug-in record and in the status snapshot's plug-in row
(`xrt_status_plugin::remedy`, a same-build service↔CLI struct). It is part of the topology change
key, so `status --watch`, the tray and the Dashboard refresh when it appears or changes. A changed
remedy logs one WARN on the load path, not one per re-probe.

**Where the user sees it.** Every surface uses vendor-neutral wording around the plug-in's strings:

- `displayxr-cli info` / `selftest` print an indented line under the plug-in, and `status` prints a
  `PLUG-IN <id>:` line under its header. The line is
  `Update recommended: <installed> -> <required>. <label>: <url>`, with empty parts left out. On
  each plug-in object in `--json`, the keys `update_recommended`, `update_required`,
  `installed_version`, `required_version`, `action_label` and `action_url` are always present.
- **`selftest` warns and never fails** on either bit. It prints `WARN: PLATFORM_UPDATE_RECOMMENDED`
  or `WARN: PLATFORM_UPDATE_REQUIRED` with the line, and the same code appears in `warnings[]`
  (level `warn`). The exit code is decided exactly as before. A plug-in that cannot run fails the
  existing checks through its state and its decline, not through this bit.
- The service tray adds the active plug-in's notice to its menu (greyed text) and, when there is an
  https URL, an item named after `action_label` that opens the page. The tooltip adds
  "(update recommended)" or "(update required)".
- On the Home page, the Dashboard shows an "Update available" or "Update required" chip, the
  versions, and a button that opens the page. The URL is checked again with the Dashboard's
  https-only rule. The Components row lists the same line.

`sim_display` reports no remedy.
