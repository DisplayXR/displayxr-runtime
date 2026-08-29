# Runtime performance settings in the Control Panel — census + design

> **Status: Phases 0 and 1 SHIPPED** (#1252); Phases 2-3 are still design. This doc answers "can the
> runtime's performance levers be exposed as toggles in the DisplayXR Control Panel",
> censuses every lever, and proposes the mechanism. Related: #378 (panel
> architecture — *the GUI is dumb, `displayxr-cli` is the brain*), #793 (make the
> panel's override state loud), #918 /
> [ADR-037](../adr/ADR-037-adapter-placement-policy-hybrid-devices.md),
> [`motion-to-photon-levers.md`](../reference/motion-to-photon-levers.md),
> [`adapter-selection.md`](../reference/adapter-selection.md).

## The problem, stated precisely

Every runtime performance lever is an **environment variable**. The Control Panel is a
thin, non-elevated SDL+cimgui front-end that shells out to `displayxr-cli` and links
zero runtime code (`src/xrt/targets/control_panel/CMakeLists.txt:5-11`). The runtime
DLL loads into the **app's** process.

Three consequences kill the obvious design:

1. **The panel cannot set an env var for a running app.** Different process; the value
   was latched at first read anyway.
2. **The panel cannot set one for an app it never launches** — and it never launches
   any. The service orchestrator spawns only the workspace controller
   (`service_orchestrator.c`); an app started from Explorer, Steam or the Unity editor
   inherits nothing from the panel.
3. **Even in-process, `getenv` is not what you think.** On Windows it reads the CRT's
   cached environment table; `SetEnvironmentVariableW` does not refresh it. This is
   already documented for `DXR_D3D_FORCE_GPU` (`adapter-selection.md` §*The `getenv()`
   caveat*) and cost displayxr-unity#243 a native shim.

**Therefore: the only channel that reaches an arbitrary app process is a persisted store
that the runtime DLL reads from inside that process.** Any design phrased as "the panel
sets an env var" is wrong on Windows unless it also says who launches the app.

## How options are read today — three patterns, one seam

| Pattern | Example | Seam? |
|---|---|---|
| `DEBUG_GET_ONCE_*_OPTION` (`u_debug.h:95-166`) → `debug_get_*_option` → **`get_option_raw()`** (`u_debug.c:39-97`) | `DEBUG_GET_ONCE_BOOL_OPTION(weave_on_scanout, "DXR_WEAVE_ON_SCANOUT", true)` | **Yes** — one function, three platform bodies, covers every macro-based option in every process linking `aux_util` |
| Hand-rolled `static int x = -1; if (x < 0) x = parse(getenv("DXR_…"));` at the call site | `comp_d3d11_target.cpp:43-55`, `comp_split_gate.c:47-55`, `comp_d3d11_service.cpp:19741` | No — bypasses `get_option_raw` entirely. **~2/3 of the perf levers.** |
| A name-taking helper doing property-then-env | `bg2d_int_knob(prop, env, dflt)` (`comp_bg2d.c:190-205`) | Prior art for exactly the shape we want |

Three facts that constrain any redesign:

- **`get_option_raw` already has an out-of-process channel on Android**: it reads the
  system property `debug.xrt.<NAME>` rather than the environment (`u_debug.c:66-78`).
  Android is the platform where this problem is already solved.
- **The parsers are deliberately not unified.** `comp_split_gate.c:20-44` carries an
  explicit note — the leading-character test is *"deliberately NOT
  `debug_string_to_bool`"*. A design that re-parses centrally silently changes
  purpose-built semantics. **The store must supply strings and let each site parse as it
  does today.**
- **`DEBUG_GET_ONCE_*` caches are per translation unit.** `DXR_PRESENT_OPAQUE` has six
  independent caches, `DXR_LATE_WEAVE` five. Harmless for next-launch semantics; fatal
  for any naive "live update" of those names.

## The four tiers

The decisive question is **not** "is the read site on the per-frame path" — most of them
are. It is **what the value is consumed into.**

### Tier 1 — next-launch (the large majority)

The value is baked into an object at creation: an adapter is picked, a device is made, a
thread is spawned, a swapchain `AlphaMode` is set, a `VkInstance` layer list is finalized,
a client cap is sized. Changing these requires a new session, and often a new process.

`DXR_WEAVE_ON_SCANOUT` (Stage A stands up a second D3D device,
`comp_d3d11_compositor.cpp:4011`) · `DXR_WEAVE_REPAINT` (spawns, or does not spawn, the
repaint thread — `:4450-4463`) · `DXR_VK_QUEUE_MODE` (layer injection at `VkInstance`
create, `oxr_vulkan.c:353`) · `DXR_PRESENT_OPAQUE` (swapchain `AlphaMode`) ·
`DXR_D3D_FORCE_GPU` / `DXR_VK_FORCE_GPU` (adapter selection) · `DXR_MAX_CLIENTS`
(service start only, `ipc_server_process.c:541-551`) · `DXR_SPLIT_INGRESS` ·
`DXR_APP_HWND_LATENCY` (uncached `getenv`, but consumed at `SetMaximumFrameLatency`) ·
`DXR_VK_DEPOSIT` · `DXR_CMD_QUEUE` · `DXR_COMPOSE_FROM_COPY` · `DXR_LATE_WEAVE`.

### Tier 2 — service-scoped, could be live

Consumed fresh on the service's own render/commit tick; the *only* thing latching them is
a `static … = -1` cache in a process that already runs continuously. Making them live
means deleting the static and reading a service-held snapshot — mechanically cheap.

`DXR_COMMIT_PACE` · `DXR_FENCE_WAIT_MS` · `DXR_EVICT_IDLE_MS` · `DXR_EVICT_ENDED_MS` ·
`DXR_DP_GRAVEYARD_MS` · `DXR_DEVICE_REMOVED_EXIT_MS` · `DXR_HEALTH_MS`.

Semantic caveat: `DXR_COMMIT_PACE` changes backpressure discipline; flipping it under a
live client is a behaviour change mid-flight, not just a number change.

### Tier 3 — genuinely live

The honest members are `DXR_LATE_WEAVE_MAX_LATENCY` / `_AUTOBACKOFF`: the governor
(`comp_weave_latency_win.h:167-229`) **already** mutates its `effective` depth at runtime
— only `base` is latched. Plus the pure observers (`DXR_FRAME_WITNESS`,
`DXR_FRAME_STAGE_TIMING`, `DXR_WEAVE_LATENCY_CSV`), which change nothing but logging.

**But an in-process app has no IPC back to the panel.** "Live" in an app process could
only mean polling the store on a cadence. Not recommended for v1. Read Tier 3 as *live in
the service, next-launch in-app*.

### Tier 4 — must never be a toggle

All `DXR_TEST_*` · every `_DIAG` / `_DUMP` / `_PROBE` / `_JIGGLE` / `_REFLATTEN` /
`_DRAIN` / `_NO2D` bisect probe · `DXR_WEAVE_REPAINT_FORCE` (documented as *"it **will**
cost frame rate"*) · `DXR_SIM_INPUT` · `DXR_LEGACY_STANDALONE` · `DXR_IPC_FD` /
`DXR_IPC_HANDLE` · `DXR_ALLOW_UNVERIFIED_CONTROLLER`.

> **Security carve-out — `DXR_ALLOW_DEV_PLUGIN_PATHS`.** It is read via
> `GetEnvironmentVariableW` in `target_plugin_path_guard.c:91`, deliberately outside the
> `u_debug` machinery, and it **disables the #943 plug-in-path guard** — i.e. it decides
> whether the runtime will load a DLL out of a build tree. A per-user settings file that
> could turn off a code-loading guard is a privilege-escalation vector. It stays
> environment-only; the resolution chain below must never be plumbed into it.

## Design

### 1. A resolution chain with provenance

New `src/xrt/auxiliary/util/u_setting.{h,c}` in `aux_util` (which already links cJSON
publicly — `auxiliary/util/CMakeLists.txt:172-176`):

```
u_setting_get_raw(name, buf, cap, &provenance)
  1. environment          getenv_s / getenv / debug.xrt.<NAME>      <-- ALWAYS WINS
  2. per-user file        %LOCALAPPDATA%\DisplayXR\settings.json
                          $XDG_CONFIG_HOME/displayxr/settings.json
  3. machine default      HKLM\Software\DisplayXR\Settings  (REG_SZ per name)
  4. not found            -> the call site's own compiled default, unchanged
```

**Env outranks everything, deliberately.** `scripts/perf-ladder/config.json` sets
`DXR_WEAVE_REPAINT`, `DXR_VK_FORCE_GPU` and friends per A/B arm in the launcher
environment. If a stale panel setting could outrank that, every measurement the perf
ladder produces would silently lie. The same rule defuses half the "a stale panel setting
pins someone to a bad configuration" trap: a launcher, a `.bat` file or a harness can
always take control back without touching the panel.

**Who writes what:**

| Store | Writer | Elevation |
|---|---|---|
| per-user `settings.json` | the Control Panel, via a new CLI verb | none — this is why it exists |
| `HKLM\Software\DisplayXR\Settings` | an elevated `displayxr-cli`, or an installer | admin, same story as `PreferredPlugin` |

The panel is `asInvoker` — there is no `requestedExecutionLevel` anywhere in the repo, and
the only manifest it embeds is `common/dpi_aware.manifest`. Today that is why
`displayxr-cli dp use` reports *"run from an elevated terminal (HKLM needs admin)"*
(`cli_cmd_dp.c:124-129`). The per-user file gives the panel a store it can actually write.

**Only allow-listed names resolve from the stores — a safety property, not
tidiness.** `u_setting_is_managed()` gates steps 2 and 3; an unmanaged name
resolves from the environment alone, exactly as before. This exists because
several `DXR_*` variables gate authorization rather than performance
(`DXR_ALLOW_UNVERIFIED_CONTROLLER`), and wiring `get_option_raw` to a per-user
file would otherwise have made every one of them settable from a GUI-writable
file. `DXR_ALLOW_DEV_PLUGIN_PATHS` is doubly safe: it reads via
`GetEnvironmentVariableW`, outside this machinery entirely. The list starts as
exactly the six levers the three controls drive; extending it is a deliberate
act, and the Tier-4 names below must never appear on it.

**Robustness.** Parsed lazily, exactly once per process, behind `InitOnceExecuteOnce` /
`pthread_once`. Every failure — file absent, unreadable, malformed, or a low-integrity /
AppContainer process denied `%LOCALAPPDATA%` — returns "not found" silently and falls
through to the next source. It must never fail an app, and must never log per option.
Model it on `service_config_load()` (`targets/service/service_config.c:162-200`), which is
documented as *"Returns defaults if the file is absent or contains errors — never fails."*

### 2. Wiring it in — two steps, in this order

1. **`u_debug.c`'s `get_option_raw()` calls `u_setting_get_raw` instead of reading the
   environment directly.** ~15 lines. Every `DEBUG_GET_ONCE_*` option on all three
   platforms inherits the chain, in every process that links `aux_util` — the runtime DLL
   inside the app, `displayxr-service.exe`, `displayxr-cli.exe`.
2. **For the chosen subset only**, convert hand-rolled `getenv("DXR_…")` call sites to
   `u_setting_get_raw`, handing the string to **each site's existing parser, unchanged**.
   Do not convert Tier-4 sites, and never `target_plugin_path_guard.c`.

### 3. What the panel shows

**"Performance" — always visible, three controls.**

- **Target GPU** — `Auto (recommended)` / `Panel's adapter (scanout)` / `Discrete` /
  `Integrated`, writing `DXR_D3D_FORCE_GPU` + `DXR_VK_FORCE_GPU`. This is the one
  genuinely user-meaningful lever: it is a **documented supported contract** (#845,
  `adapter-selection.md`), the correct answer differs per machine, and `displayxr-unity`
  already ships a user-facing *Target GPU* setting over it.
- **Mode** — `Balanced (default)` / `Compatibility`. Compatibility is a named bundle of
  the kill switches (`DXR_WEAVE_ON_SCANOUT=0`, `DXR_WEAVE_REPAINT=0`, `DXR_LATE_WEAVE=0`)
  for the "3D looks wrong — is it the pipeline?" triage. **Presets, not forty checkboxes.**
- **Effective state readout** — read-only, and the most valuable half of this feature.

**"Developer settings" — collapsed, off by default, does not persist across panel
launches.** Lists Tier 1 + Tier 2 levers with name, current value, **provenance**
(env / user / machine / default), compiled default, and a per-row Reset, plus one
"Reset all performance settings". Tier 4 never appears here.

### 4. Effective state — and why the CLI is misleading today

`displayxr-cli` reads `DXR_WEAVE_ON_SCANOUT` and `DXR_SPLIT_INGRESS` from **its own
process environment** (`cli_query.c:194,356`) and re-derives the split verdict itself
(deliberately — it is headless and links no compositor). Launched from the panel as a
child process, that reports the **panel's** environment: not the app's, not the service's.

Introducing the store is what makes that report truthful for the first time, because every
process resolves the same chain. Three surfaces, in order of value:

1. **Headless — `displayxr-cli info --json`.** Extend the existing `GPU topology` block
   with a `performance` block: per lever, the resolved value **and its provenance**. That
   is "what a process starting right now would get". The panel renders it.
2. **Live service.** `displayxr-cli clients` already connects to the running service over
   IPC as class `DIAG`, verified by exe path (`cli_cmd_clients.c`). Return the service's
   *resolved* lever values over that same path. ADR-035 D7 already plans a per-client
   health RPC — fold into it rather than adding a second channel.
3. **A running in-process app — not reachable, and the panel must say so.** Label
   app-side levers *"applies on next launch"* and point at the authoritative artifact: the
   single `weave placement:` WARN in `%LOCALAPPDATA%\DisplayXR\DisplayXR_<exe>.*.log`.

### 5. Anti-stale, four mechanisms

1. **Env always wins** (above).
2. **Panel-written overrides are session-scoped by default**, with an explicit "keep
   across restarts" checkbox. This is exactly what #793 Phase 2 asked for on the DP
   override after that one turned out to be a footgun ("persists across reboots until
   reset").
3. **A loud persistent banner whenever any lever is non-default**, styled on the existing
   amber DP-override banner (`control_panel_main.c:550-567`), with one-click reset-all.
4. **`displayxr-cli info` / `selftest` print a "non-default performance settings" line**,
   so every bug report carries it. `selftest` reports it; it must not fail on it.

## Risks

1. **The #1248 class: broken output reads as a performance win.** A NULL-swapchain guard
   made every Windows Vulkan app on the DComp bridge present nothing, and it measured as a
   ~12-point GPU *improvement* (app GPU 8.91 → 4.12, with `dwm ≈ 0.00` as the only tell).
   It shipped in five releases. **A surface that lets users change these levers must never
   show a performance number without an output-integrity number beside it** — present
   count, weave count, and the `weave placement:` verdict. Safest v1: the panel shows
   configuration and integrity, and makes no "faster" claim at all.
2. **The levers interact, and the defaults are computed rather than fixed.** Repaint ×
   late-weave × pacing depth × the scanout split are not independent. The render adapter
   is ranked by dedicated VRAM (`d3d_render_adapter.cpp:189-199`), the scanout adapter is
   resolved from real display topology, and the split engages *automatically* when they
   differ. A checkbox per lever invites combinations nobody has ever measured — which is
   the argument for presets.
3. **Doc drift already exists.** 32 of the 71 `DXR_*` names appear nowhere in `docs/`, and
   `motion-to-photon-levers.md` documents `DXR_DEFER_PRESENT` in four places although it
   exists nowhere in `src/`. Exposing a name in a GUI is a commitment to it; document
   first.
4. **Low-integrity / AppContainer app processes** (the browser's GPU process) may be
   denied `%LOCALAPPDATA%`. The chain must degrade to HKLM, then to compiled defaults,
   without a word.
5. **A settings file must never gate code loading** — see the `DXR_ALLOW_DEV_PLUGIN_PATHS`
   carve-out above.
6. **Per-TU caching** means a "live" toggle on a `DEBUG_GET_ONCE_*` name would update some
   read sites and not others. Tier 1 must present in the UI as *next launch*, never as a
   checkbox that looks instant.

## Phasing

| Phase | Content | Status |
|---|---|---|
| **0** | GPU topology + provenance in `displayxr-cli info --json`, rendered by the panel. No behaviour change. | **SHIPPED** |
| **1** | `u_setting` chain + allow-list; the three user controls; `displayxr-cli perf`; the anti-stale banner. | **SHIPPED** |
| **2** | Gated "Developer settings" list (Tier 1 + Tier 2, with provenance). Wants the panel on **tabs** first — it is nine sections in one scroll column now, and at 250 % DPI you see about a third of it. | design |
| **3** | Tier-2 live service settings over the existing `DIAG` IPC path. | design |

### The three controls, as shipped

Deliberately three, not one dropdown: they are unrelated axes, and folding them
together produces a combinatorial menu that is both larger and less clear. There
is **no quality-versus-performance dial** in this runtime — the defaults *are*
the tuned configuration — so a Quality/Balanced/Performance menu would be
fiction.

| Control | Levers | Notes |
|---|---|---|
| **Target GPU** — Auto / Panel's display adapter / High performance / Power saving | `DXR_D3D_FORCE_GPU` + `DXR_VK_FORCE_GPU` | Hidden entirely on single-adapter boxes; there is nothing to choose. A documented supported contract (#845) |
| **Mode** — Balanced / Compatibility | `DXR_WEAVE_ON_SCANOUT=0` + `DXR_WEAVE_REPAINT=0` | Both change **what the display processor is asked to do**, which is where compatibility problems actually live |
| **Diagnostics** — Off / On | `DXR_FRAME_WITNESS=5` + `DXR_FRAME_STAGE_TIMING=1` | Pure observers; they change no behaviour, which is what makes them safe to hand a user |

**`DXR_LATE_WEAVE` is deliberately *not* in the Compatibility bundle.** It only
changes *when* we present on our own swapchain, it already self-disables where
the platform gives no present-timing feedback ("dormant rather than wrong"), and
it is the single largest latency win we have (96 → 17 ms on VK). Bundling it
would charge every Compatibility click a ~5× latency regression on the lever
least likely to be the culprit. It belongs in the Phase-2 developer list, for
bisecting.

Compatibility mode is **derived from the resolved lever values**, not stored as
its own key: a preset that stored its own name would drift from what the levers
say the moment anything else wrote one of them. Deriving it means the UI can
never claim a mode the runtime is not in, and "Custom" falls out for free.

---

## Appendix A — census

Every `DXR_*` name read at runtime under `src/xrt`, with its read site, mechanism, default
and tier. **79 distinct names**; the two `DXR_BG2D_*` knobs reach the environment through
`bg2d_int_knob()` rather than a literal `getenv` at the listed line.

Process column: **App** = the runtime DLL, loaded into the OpenXR app's process ·
**Svc** = `displayxr-service.exe` · **Both** = a shared `auxiliary/` or `targets/common/`
library linked into both · **CLI** = `displayxr-cli.exe` (reporting only, controls nothing).

### Weave / present pipeline

| Var | Read site | Mechanism | Default | Proc | Tier | What it does |
|---|---|---|---|---|---|---|
| `DXR_WEAVE_ON_SCANOUT` | `compositor/d3d11/comp_d3d11_compositor.cpp:128`; `compositor/xbridge/comp_split_gate.c:52`; (CLI report `targets/cli/cli_query.c:356`) | `DEBUG_GET_ONCE_BOOL` + hand-rolled `static` | **on** (kill switch) | App | 1 | Kill switch for the ADR-037 output-device split. Stage A creates a second D3D device on the scanout adapter |
| `DXR_WEAVE_ON_SCANOUT_DEPTH` | `compositor/xbridge/comp_xbridge.cpp:1986` | `getenv`, per-session | off | App | 4 | Forces the deterministic seq−1 slot instead of newest-ready. Diagnostic |
| `DXR_WEAVE_REPAINT` | `compositor/d3d11/comp_d3d11_compositor.cpp:4450`; `d3d12/…:5014`; `gl/…:6039`; `vk_native/…:6151` | `getenv` at compositor create | **on** | App | 1 | Master switch for #868 repaint. Decides whether the repaint thread is spawned |
| `DXR_WEAVE_REPAINT_FORCE` | same four files, next line (`:4452`, `:5016`, `:6041`, `:6274`) | `getenv` at create | off | App | 4 | Repaint every refresh regardless of app rate. Correctness probe; **will** cost frame rate |
| `DXR_WEAVE_REPAINT_GATE` | `auxiliary/util/u_repaint_gate.h` (`u_repaint_gate_open`, consumed by all four repaint loops) | `getenv`, cached in gate state | legacy | App | 4 | `adaptive` opts in to the EXPERIMENTAL N=2 window ("app presents every N vblanks" estimator; governor-guarded; N≥3 always legacy — five schedule variants lost on hardware). Not default: its intermittent engagement failed the hz30 visual check — panel cadence oscillating 28-50 reads as judder where a steady 33 does not (#1257) |
| `DXR_WEAVE_REPAINT_TRACE` | `auxiliary/util/u_repaint_gate.h` (`u_repaint_trace_*`, wired into the vk_native + d3d11 repaint loops) | `getenv`, cached | off | App | 4 | #1257 verification instrumentation: one WARN row per ~5 s per loop with real tick cadence, replay/pace durations, and per-gate bail counts. Says where missing repaints went; the witness only says how many landed |
| `DXR_APP_FRAME_DIVISOR` | `auxiliary/util/u_app_partition.h` (`u_app_partition_throttle` in each in-process wait_frame; engagement read by the repaint gate + loop tick) | `getenv`, cached | off (1) | App | 2 | #1257 slot partition: xrWaitFrame releases the app every Dth vblank (D=2..8) and the repaint loop fills the other slots at panel rate with a KNOWN N=D schedule — steady output cadence, no fire/commit collision by construction. **Supported tier: the #918 split (d3d11 bridge / hybrid) only** — verified steady 60 with eyeball sign-off; in-process vk_native/d3d12/gl tiers REFUSE cleanly (fill-loop tick starvation, 17-19 ms vs 1.8-2.3 ms on the bridge; follow-up). The natural Control Panel "smooth motion, lower app rate" control and the hook for face-tracking-driven rate later. Not wired into Metal or the IPC/service path |
| `DXR_APP_FRAME_DIVISOR_ANY_TIER` | `auxiliary/util/u_app_partition.h` (`u_app_partition_throttle` tier gate) | `getenv`, cached | off | App | 4 | Bring-up override: lets the partition throttle engage on an UNSUPPORTED tier (which collapses the panel there today). For the in-process-tier follow-up work only |
| `DXR_DP_FORWARD_HORIZON` | `compositor/util/comp_weave_latency_win.h` (`predict_weave_to_scanout_ns`, feeds the DP `set_predicted_scanout` slot from the split + d3d11 + d3d12 weave paths) | `getenv`, cached | **on** (kill switch) | App | 1 | #206: per-weave FORWARD horizon for the vendor eye predictor, computed from the vsync-locked vblank grid (DXGI frame statistics — measured period, not nominal). `0` disables → the DP falls back to its retrospective smoothed heuristic, the pre-#206 behavior |
| `DXR_FILL_FENCE_PARK` | `compositor/vk_native/comp_vk_native_compositor.c` (fill path of `vk_dp_weave_and_present`) | `getenv`, cached | **on** (kill switch) | App | 1 | #1264 S1: a fill's GPU fence wait releases the compositor lock (re-validated on retake; superseded fills are dropped). `0` restores the lock-held synchronous wait |
| `DXR_FILL_MMCSS` | `auxiliary/util/u_fill_thread_win.h` (`u_fill_thread_join_mmcss`, all four fill threads) | `getenv` at thread start | **off** (opt-in) | App | 4 | #1264 S2: fill threads join MMCSS "Pro Audio" on `1`. Default off: measured NEGATIVE interaction with the S1 fence-park (combined, the tier collapsed below strikes-off — a park/wake-heavy thread is what MMCSS deprioritizes); neutral alone |
| `DXR_SPLIT_SAME_ADAPTER` | `compositor/xbridge/comp_split_gate.c` (`comp_split_gate_env_same_adapter`; consulted by the VK tier — ADR-039 Phase A accepted) | `getenv`, latched | **on** (`=0` kills) | App | 2 | ADR-039: engages the #918 split when render == scanout — one fill engine for every tier. Default flipped on the #1264 Phase A acceptance record (2026-08-29); `=0` restores the same-adapter decline |
| `DXR_SPLIT_SAME_ADAPTER_D3D12` | `compositor/xbridge/comp_split_gate.c` (`comp_split_gate_env_same_adapter_d3d12`; consulted by the D3D12 tier's Stage A) | `getenv`, latched | off (bring-up) | App | 2 | ADR-039 Phase B bring-up: `=1` lets the D3D12 leg engage the same-adapter split. Default off until that tier passes its own acceptance matrix (repeated ≥5-min legs); then it collapses into `DXR_SPLIT_SAME_ADAPTER` and this env retires |
| `DXR_SPLIT_QUEUE_HIGH` | `compositor/d3d12/comp_d3d12_compositor.cpp` (Stage A out-queue creation; same-adapter engage only) | `getenv`, per-create | on under same-adapter (`=0` forces NORMAL) | App | 2 | #1264 Phase B: the same-adapter out queue is created at `D3D12_COMMAND_QUEUE_PRIORITY_HIGH` so weave submissions preempt the app's NORMAL direct queue (measured: NORMAL-vs-NORMAL balloons fire= 3.5–5× under a real app). Hybrid always NORMAL. A/B lever; refusal falls back to NORMAL with one WARN |
| `DXR_WEAVE_REPAINT_HASH` | `compositor/d3d11/comp_d3d11_compositor.cpp:4454` | `getenv` at create | off | App | 4 | Content-identity hash probe on repainted frames |
| `DXR_WEAVE_REPAINT_APPTHREAD` | `compositor/gl/comp_gl_compositor.cpp:4788` | `getenv`, cached | off | App | 4 | #885 bisect: replay the repaint path on the app thread to separate "replay broken" from "DP is thread-affine" |
| `DXR_WEAVE_REPAINT_DIAG` | `compositor/gl/comp_gl_compositor.cpp:3117, 3324, 3434` | `getenv`, cached per site | off | App | 4 | #885 per-thread FBO / blit-error / texel probe |
| `DXR_WEAVE_REPAINT_NO2D` | `compositor/vk_native/comp_vk_native_compositor.c:3849` | `getenv`, cached | off | App | 4 | Skip the zones/Local2D composite on a repaint (#868 bisect) |
| `DXR_WEAVE_REPAINT_REFLATTEN` | `compositor/gl/comp_gl_compositor.cpp:3388` | `getenv`, cached | off | App | 4 | Force a repaint to re-run deposit/flatten. Source comment: "must never be a default" |
| `DXR_WEAVE_REPAINT_DRAIN` | `compositor/vk_native/comp_vk_native_compositor.c:4219` | `getenv`, cached | off | App | 4 | Force `vkDeviceWaitIdle` around a repaint (cross-queue race probe) |
| `DXR_WEAVE_SINGLE_THREAD` | `compositor/vk_native/comp_vk_native_compositor.c:6169` | `getenv` (+ `debug.dxr.weave_single_thread`) | on (Android), off elsewhere | App | 1 | #1196 pins the DP weave to one thread where the DP is proven thread-affine |
| `DXR_LATE_WEAVE` | `compositor/d3d11/comp_d3d11_target.cpp:51`; `d3d11_service/…:187`; `d3d12/comp_d3d12_target.cpp:68`; `gl/…:2423`; `vk_native/comp_vk_native_target.cpp:229` | `getenv`, cached per site | **on** | App + Svc | 1 | Weave as late as possible before scanout. The largest single latency win |
| `DXR_LATE_WEAVE_MAX_LATENCY` | `compositor/util/comp_weave_latency_win.h:221`; `vk_native/comp_vk_native_target.cpp:245` | `getenv` into a file-scope governor | 1 | App + Svc | 3 | Frame-latency depth, clamped `1..LATE_WEAVE_MAX_DEPTH`. Only `base` is latched; `effective` already moves at runtime |
| `DXR_LATE_WEAVE_AUTOBACKOFF` | `compositor/util/comp_weave_latency_win.h:224` | `getenv` into the same governor | on | App + Svc | 3 | Backs the depth off on saturation; 30 s dwell, doubling, capped at 5 min |
| `DXR_PREDICT_LEGACY` | `compositor/util/comp_late_weave_lookahead.h:56` | `getenv` | off | App | 4 | Restores the pre-#867 `period x 2` lookahead constant, for A/B |
| `DXR_VK_BRIDGE_PACING` | `compositor/vk_native/comp_vk_native_target.cpp:99` | `getenv`, cached | governor (auto) | App | 1 | `0..3` pins the VK bridge queue depth and disables governor transitions (#912) |
| `DXR_VK_QUEUE_MODE` | `compositor/vk_native/comp_vk_native_compositor.c:6200`; `state_trackers/oxr/oxr_vulkan.c:353` | `getenv`, uncached | `auto` | App | 1 | #902 repaint queue tier. The `oxr_vulkan` read decides layer injection at `VkInstance` create — irreversible for that instance |
| `DXR_VK_DEPOSIT` | `compositor/vk_native/comp_vk_native_deposit.cpp:42` | `DEBUG_GET_ONCE_BOOL` | off | App | 1 | VK-0 same-adapter D3D11 deposit for the atlas (#1178) |
| `DXR_VK_DEPOSIT_PROBE` | `compositor/vk_native/comp_vk_native_deposit.cpp:43` | `DEBUG_GET_ONCE_BOOL` | off | App | 4 | One-shot readback proof of the deposit. Diagnostic |
| `DXR_PRESENT_OPAQUE` | `d3d11/comp_d3d11_compositor.cpp:104`; `d3d11/comp_d3d11_target.cpp:32`; `d3d12/comp_d3d12_compositor.cpp:94`; `d3d12/comp_d3d12_target.cpp:33`; `vk_native/comp_vk_native_compositor.c:138`; `vk_native/comp_vk_native_target.cpp:75` | `DEBUG_GET_ONCE_BOOL` ×6 **independent TU caches** | off | App | 1 | Opaque flip chain instead of the composed chain. Sets swapchain `AlphaMode` |
| `DXR_APP_HWND_LATENCY` | `compositor/d3d11_service/comp_d3d11_service.cpp:263` | `getenv`, **uncached** | 2 | Svc | 1 | Frame-latency depth on the app-HWND present path. Consumed at `SetMaximumFrameLatency` |
| `DXR_COMMIT_PACE` | `compositor/d3d11_service/comp_d3d11_service.cpp:19741` | `getenv`, `static` cached | on (1; 0/1/2) | Svc | 2 | Commit backpressure. `=0` lets a client run free; `=2` is the legacy tick-align shape |
| `DXR_FENCE_WAIT_MS` | `compositor/d3d11_service/comp_d3d11_service.cpp:15911` | `getenv`, `static` cached | 4 (clamped 0..16) | Svc | 2 | Budget for the workspace-sync fence wait on a client's IPC thread |
| `DXR_CMD_QUEUE` | `compositor/d3d11_service/comp_d3d11_service.cpp:2341` | `getenv`, `static` cached | on | Svc | 1 | Command-queue submission path vs. the fair-lock path (A/B) |
| `DXR_COMPOSE_FROM_COPY` | `compositor/d3d11_service/comp_d3d11_service.cpp:2328` | `getenv`, `static` cached | **off** ("until soaked") | Svc | 1 | Compose from a service-owned `CopyResource` instead of sampling the shared handle |
| `DXR_SPLIT_INGRESS` | `compositor/xbridge/comp_split_gate.c:71`; (CLI report `cli_query.c:194`) | `getenv`, **uncached** | `adaptive` | App | 1 | `adaptive` reads a still source in place; `staged` pins the PR 3-5 per-frame copy. A/B control, not tuning |
| `DXR_BG2D` | `compositor/util/comp_bg2d.c:102` | `getenv` (+ `debug.dxr.bg2d`) | disabled | App | 1 | De-occlusion backdrop: `grad`, `solid:RRGGBB`, `capture[:socket]`, … |
| `DXR_BG2D_SYNC` | `comp_bg2d.c:219` via `bg2d_int_knob` | `getenv` (+ sysprop), cached | **on** | App | 4 | #1120 slot-16 lifetime sync. `=0` restores the racy behaviour for a one-binary A/B |
| `DXR_BG2D_JIGGLE` | `comp_bg2d.c:850` via `bg2d_int_knob` | `getenv` (+ sysprop), cached | 0 (off) | App | 4 | Deliberate canvas churn at N Hz to force the #1120 race. Test-only |
| `DXR_L2D_CLIP` | `compositor/vk_native/comp_vk_native_compositor.c:143` | `DEBUG_GET_ONCE_BOOL` | **on** | App | 1 | #862 clips the Local2D composite to non-identity pixels |
| `DXR_LINUX_DIRECT_SCANOUT` | `compositor/vk_native/comp_vk_native_compositor.c:5845` | `getenv` | off | App | 1 | Opt-in RandR direct-scanout present path on Linux |
| `DXR_WINDOW_POS` | `compositor/vk_native/comp_vk_native_compositor.c:5248` | `getenv` | plug-in reported | App | 1 | `"x,y"` override for the self-owned window position (#715) |
| `DXR_WINDOW_FULLSCREEN` | `compositor/vk_native/comp_vk_native_window_xcb.c:273` | `getenv` | on | App | 1 | `=0` opts out of EWMH fullscreen placement (Linux XCB) |

### GPU placement

| Var | Read site | Mechanism | Default | Proc | Tier | What it does |
|---|---|---|---|---|---|---|
| `DXR_D3D_FORCE_GPU` | `auxiliary/d3d/d3d_render_adapter.cpp:303` (`env_forced_adapter`); (CLI report `cli_query.c:272`) | `getenv`, uncached; call site runs once per app | unset | Both | 1 | **Supported contract** (#845). `scanout` / `igpu` / `dgpu` / index. Read at `xrGetD3D11GraphicsRequirementsKHR` / `…D3D12…` time |
| `DXR_VK_FORCE_GPU` | `auxiliary/vk/vk_bundle_init.c:625` (`env_forced_gpu_index`); LUID resolved in `targets/common/target_instance.c:191` | `getenv`, uncached | unset | Both | 1 | Same contract for the Vulkan physical device |

### Android

All five also accept `debug.xrt.<NAME>` as a system property, for free, via
`get_option_raw`'s Android branch.

| Var | Read site | Mechanism | Default | Proc | Tier | What it does |
|---|---|---|---|---|---|---|
| `DXR_ANDROID_ADPF` | `compositor/multi/comp_multi_system.c:107` | `DEBUG_GET_ONCE_BOOL` | **on** | App | 1 | ADPF hint session on the render thread; avoids the ~13 ms core-park stall (#646) |
| `DXR_ANDROID_PIPELINE_WEAVE` | `compositor/multi/comp_multi_system.c:126` | `DEBUG_GET_ONCE_BOOL` | **on** | App | 1 | Pipelines the weave by one frame so it overlaps the pacing sleep (#663) |
| `DXR_ANDROID_PRESENT_TS` | `compositor/multi/comp_multi_system.c:106` | `DEBUG_GET_ONCE_BOOL` | **on** | App | 1 | Phase-split present loop with per-phase timestamps |
| `DXR_ANDROID_VSYNC_PACING` | `compositor/main/comp_target_swapchain.c:116` | `DEBUG_GET_ONCE_BOOL` | **on** | App | 1 | Feeds AChoreographer vsync timestamps into the pacer (closed loop) |
| `DXR_ANDROID_WEAVE_SPLIT` | `compositor/multi/comp_multi_weave_android.c:75` | `DEBUG_GET_ONCE_BOOL` | **on** | App | 1 | Kill switch for the self-submitting-DP split submission path |

### Diagnostics / observers

| Var | Read site | Mechanism | Default | Proc | Tier | What it does |
|---|---|---|---|---|---|---|
| `DXR_FRAME_STAGE_TIMING` | `compositor/vk_native/comp_vk_native_compositor.c:148` | `DEBUG_GET_ONCE_BOOL` | off | App | 3 | Per-stage CPU timing of the windowed commit; `composite=` is the GPU wait |
| `DXR_FRAME_WITNESS` | `compositor/util/comp_frame_witness.h:62` | `getenv`, atomic cached | 0 (secs) | App | 3 | Windowed count of app weaves vs. repaints vs. presents vs. 3D weaves |
| `DXR_WEAVE_LATENCY_CSV` | `compositor/util/comp_weave_latency_win.h:82`; `vk_native/comp_vk_native_target.cpp:1860, 1878` | `getenv` | unset | App | 3 | Filename prefix for latency CSV output |
| `DXR_WEAVE_PROBE` | `compositor/d3d11/comp_d3d11_compositor.cpp:2383` | `getenv`, cached | off | App | 4 | One-shot dump of the back buffer as the DP left it, pre-composite |
| `DXR_WEAVE_TAP` | `compositor/d3d12/comp_d3d12_compositor.cpp:4122` | `getenv`, cached | 0 | App | 4 | #727 dual-tap PNG dump around the composite. GPU flush + readback; costly |
| `DXR_PHASE_DEBUG` | `compositor/d3d12/comp_d3d12_compositor.cpp:767` | `getenv`, magic static | off | App | 4 | #740 corner-vs-centre phase seed dump |
| `DXR_XBRIDGE_DIAG` | `d3d11/comp_d3d11_compositor.cpp:4264`; `d3d11/comp_d3d11_outcomp.cpp:395`; `d3d12/comp_d3d12_outcomp.cpp:540` | `getenv` per site | off | App | 4 | #918 split diagnostics (e.g. weave-scratch grow events) |
| `DXR_SPLIT_CONTENT_PROBE` | `compositor/xbridge/comp_xbridge.cpp:1994` | `getenv`, per-session | off | App | 4 | #1178 content probe on the split ingress path |
| `DXR_SPLIT_COVER_DIAG` | `compositor/d3d11_service/comp_d3d11_service.cpp:10802` | `getenv`, cached | 0 (1=observe, 2=+sentinel) | Svc | 4 | Split-cover (post-black-frame) diagnostic mode |
| `DXR_SPLIT_COVER_DUMP` | `compositor/d3d11_service/comp_d3d11_service.cpp:12725` | `getenv`, one-shot | unset | Svc | 4 | One-shot dump of back buffer + source atlas on the first black frame |
| `DXR_CAPTURE_KEEP_ALPHA` | `compositor/d3d12/comp_d3d12_compositor.cpp:2830, 2957` | `getenv`, uncached | opaque | App | 4 | #672 preserve real atlas alpha in a debug PNG capture |
| `DXR_QTRACE` | `drivers/qwerty/qwerty_device.c:36`; `drivers/qwerty/qwerty_win32.c:33`; `state_trackers/oxr/oxr_space.c:36` | `DEBUG_GET_ONCE_BOOL` ×3 | off | Both / App | 4 | Qwerty device + space tracing |
| `DXR_KEY_DEBUG` | `ipc/server/ipc_server_macos_appkit.m:215` | `getenv`, cached | off | Svc | 4 | macOS AppKit key-forwarding diagnostic |

### Lifecycle

| Var | Read site | Mechanism | Default | Proc | Tier | What it does |
|---|---|---|---|---|---|---|
| `DXR_EVICT_IDLE_MS` | `compositor/d3d11_service/comp_d3d11_service.cpp:13554` | `getenv`, `static` cached | 0 | Svc | 2 | Grace before an idle client slot is evicted; stretches to 4× a capped client's own cadence |
| `DXR_EVICT_ENDED_MS` | `compositor/d3d11_service/comp_d3d11_service.cpp:13505` | `getenv`, `static` cached | 2000 | Svc | 2 | Grace before an ended-session slot is evicted (0 disables) |
| `DXR_DP_GRAVEYARD_MS` | `compositor/d3d11_service/comp_d3d11_service.cpp:10086` | `getenv`, `static` cached | 2000 | Svc | 2 | How long a torn-down DP is kept alive to outlive an in-flight swap |
| `DXR_DEVICE_REMOVED_EXIT_MS` | `compositor/d3d11_service/comp_d3d11_service.cpp:10279` | `getenv`, `static` cached | 2000 | Svc | 2 | Delay before the service exits on an unrecoverable `DEVICE_REMOVED` |
| `DXR_HEALTH_MS` | `ipc/server/ipc_server_process.c:82` | `DEBUG_GET_ONCE_NUM` | 10000 | Svc | 2 | #951 `[HEALTH]` telemetry cadence; 0 disables |
| `DXR_MAX_CLIENTS` | `ipc/server/ipc_server_process.c:87` | `DEBUG_GET_ONCE_NUM`, consumed `:541-551` | 0 = auto | Svc | 1 | #959 admitted-client cap. Read once at service start |
| `DXR_ULTRALEAP_SETTLE_MS` | `drivers/ultraleap/ultraleap_provider.cpp:1023` | `getenv` at provider init | `UL_SETTLE_DEFAULT_MS` | Both | 1 | How long to wait for a definitive Leap "attached" answer before role arbitration |

### Test / dev — never exposed

| Var | Read site | Default | Proc | Why not |
|---|---|---|---|---|
| `DXR_TEST_SPLIT_FAIL_STAGEA` | `compositor/xbridge/comp_split_gate.c:62` | off | App | Forces Stage A to fail |
| `DXR_TEST_FAKE_DP_REFUSE` | `d3d11/comp_d3d11_compositor.cpp:3684`; `d3d12/…:1136`; `vk_native/comp_vk_native_split.cpp:269` | off | App | Forces a DP refusal on the scanout adapter |
| `DXR_TEST_FORCE_WEAVE_INGEST_DP` | `compositor/d3d11_service/comp_d3d11_service.cpp:4269` | 0 | Svc | Forces the #1172 weave-on-ingest arm |
| `DXR_TEST_FAKE_DEVICE_REMOVED` | `compositor/d3d11_service/comp_d3d11_service.cpp:3633` | 0 | Svc | Simulates device loss |
| `DXR_TEST_EXIT_ON_DISCONNECT` | `ipc/server/ipc_server_per_client_thread.c:19` | 0 | Svc | #950 fault injection |
| `DXR_ALLOW_DEV_PLUGIN_PATHS` | `targets/common/target_plugin_path_guard.c:91` (`GetEnvironmentVariableW`) | unset | Both | **Disables the #943 plug-in-path guard.** Env-only, permanently — see the carve-out above |
| `DXR_ALLOW_UNVERIFIED_CONTROLLER` | `ipc/server/ipc_server_handler.c:167` | off | Svc | Accepts an unverified `CONTROLLER` claim. "Never set on a production box" |
| `DXR_SIM_INPUT` | `drivers/sim_input/sim_input_plugin.c:59` | off | Both | Simulated-input opt-in |
| `DXR_LEGACY_STANDALONE` | `compositor/d3d11_service/comp_d3d11_service.cpp:286` | off | Svc | Reverts the service to the pre-hybrid standalone path (ADR-035 D3, slated for deletion) |
| `DXR_IPC_FD` | `auxiliary/util/u_sandbox.c:181`; `ipc/client/ipc_client_connection.c:211, 234` | unset | Both / App | #1056 adopts an embedder-supplied service socket. Not a setting |
| `DXR_IPC_HANDLE` | `ipc/client/ipc_client_connection.c:437, 456` | unset | App | Windows analogue of the above |

### Adjacent, out of scope but worth knowing

`XRT_FORCE_MODE` (`auxiliary/util/u_sandbox.c:145`, with a Windows-only second read via
`GetEnvironmentVariableA` for exactly the split-CRT reason described in *The problem*) ·
`XRT_PRINT_OPTIONS` (logs every `debug_get_*_option` read — the closest thing to a
provenance tool that exists today) · `XRT_PREFERRED_PLUGIN_ID` / `XRT_PLUGIN_SEARCH_PATH` ·
`XRT_LOG` / `XRT_JSON_LOG` · the `OXR_*`, `IPC_*`, `SIM_DISPLAY_*` and `DISPLAYXR_*`
families.

## Appendix B — keeping this table honest

The table above is the artifact most likely to rot. Re-derive the name list with:

```bash
grep -rhoE '"DXR_[A-Z0-9_]+"' src/xrt \
  --include=*.c --include=*.cpp --include=*.h --include=*.hpp --include=*.m --include=*.mm \
  | sort -u
```

As of this writing that yields **79** names, all present above. Read sites and mechanisms
were machine-derived by intersecting that list with lines containing `getenv`,
`GetEnvironmentVariable` or `DEBUG_GET_ONCE`, then spot-verified line by line. If the
count changes, the table is stale.
