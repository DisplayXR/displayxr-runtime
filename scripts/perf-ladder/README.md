# DXR Perf Ladder (`DXRPerfLadder`)

Standard, exportable perf-decomposition test for 3D-display boxes — tells the
cost components apart (idle floor, present machinery, weave unit, live-blend
tax, shaping tax, motion/WGC tax, render slope, repaint tax) by running a fixed
ladder of arms where **each arm differs from its parent by exactly one knob**.
Design + tracking: displayxr-runtime issue **#1113**.

## Running (target box)

1. Prereqs: DisplayXR runtime installed (version with `DXR_FRAME_WITNESS`,
   i.e. > v2.8.0), vendor display plug-in installed, panel at 60 Hz, **AC
   power**, nobody in front of the display (untracked is the standard
   condition), no other 3D apps running.
2. Double-click `RUN-LADDER.cmd`. Do not touch mouse/keyboard while it runs
   (~20 min: 13 arms x 3 reps x 20 s + warmups; the script drives the cursor
   itself on the motion arms).
3. Send back the `results\ladder-results-<host>-<date>.zip` it names at the end.

Dev switches (`run_ladder.ps1`): `-Smoke` (1 rep, 8 s windows),
`-Arms IDLE-P,SHIP` (subset), `-AppExeOverride <exe>`, `-RepsOverride N`.

## Methodology (why the numbers are trustable)

- **GPU cost = `\GPU Engine(*)\Running Time` deltas** per (pid, LUID) — never
  Utilization Percentage (gauge; 2x run-to-run spread).
- **True medians** over n=3 **interleaved** reps (full pass over all arms,
  repeated — dwm bistability lands on all arms equally, not on one block).
- **Warmup excluded** (default 6 s) before every sample window.
- **Scripted cursor**: parked top-right, or a deterministic 200 px 0.5 Hz orbit
  — a human hand is not a reproducible load, and cursor motion is the WGC
  delivery driver. A parked cursor with region-on-hover is *effectively
  unshaped*, so cursor state is a first-class axis, not lab hygiene.
- **Witness counters, not inference**: `DXR_FRAME_WITNESS` logs
  presents/weaves/repaints per second + actual 2d/3d mode from inside the
  runtime; the harness verifies each arm's mode closed-loop and records the
  rates into `ladder.csv`.
- **Gates self-report**: battery, elevation, missing witness, unsettled mode
  land as flags in the CSV and as PASS/FLAG rows in `SUMMARY.md` — a flagged
  run comes back labeled instead of silently poisoning the comparison.

## Files

| file | role |
|---|---|
| `RUN-LADDER.cmd` | entry point (non-elevated) |
| `config.json` | the arm table — arms are data, not code |
| `probe_caps.ps1` | Block 0: `capabilities.json` (adapters, scanout LUID via QueryDisplayConfig, present_wait, runtime/plugin/SR versions, power) |
| `lib_sample.ps1` | GPU/CPU delta sampling, cursor orbit, window placement, witness parsing |
| `run_ladder.ps1` | orchestrator: interleaved reps -> `ladder.csv` -> log harvest -> summarize -> results zip |
| `summarize.ps1` | per-arm medians + derived components + gates -> `SUMMARY.md` |

Results are only comparable **within one kit version** and within one session's
own anchors (dwm is bistable across sessions — never compare raw columns across
boxes; compare the derived deltas).

## Native-vs-hub A/B (`run_hub_ab.ps1`, #2317 P1d)

A second harness in this kit, for the single-hub decision: it measures each
per-API in-process compositor (`native`) against the D3D11 weave/present hub
(`hub`) on `cube_handle_{d3d11,d3d12,vk,gl}_win`. An arm is a **name + an env
map** (`hub-ab.json`), plus the path its own log must prove.

Arms are per app **and per topology** (`topologies` on an arm; `native` = the
path `main` takes by default on that topology - VK's only non-hub path is the
kill switch):

| app | topology | `native` (expected path) | hub arm(s) (expected line) |
|---|---|---|---|
| d3d11 | default | `DXR_HUB=0` (own #918 split; the kill switch since #2317 step 2) | `hub`: default env (`hub placement: hub=1 ... ingress=xbridge`) |
| d3d11 | same-adapter | `DXR_HUB=0` (own same-adapter engage) | `hub-direct`: `DXR_HUB=1` (`ingress=direct`); `hub-xbridge`: + `DXR_HUB_INGRESS=xbridge`; optional `default-same`: default env, expected NATIVE (`reason=same_adapter_opt_in` - the default-on scope is cross-adapter until the desktop run) |
| d3d12 | default | default env (own-legs split) | `hub` (optional): `DXR_SPLIT_D3D12_HYBRID_REROUTE=1` (`ingress=xbridge`) - the code gate also needs `DXR_APP_FRAME_DIVISOR>=2`, so it is not like-for-like |
| d3d12 | same-adapter | `DXR_WEAVE_ON_SCANOUT=0` | `hub`: default env, the #1264 reroute (`hub placement: hub=1 ... ingress=xbridge ingress_reason=default_xbridge`); `hub-direct`: + `DXR_HUB_INGRESS=direct` (P2a, `ingress=direct`) |
| vk | default | `DXR_WEAVE_ON_SCANOUT=0` | `hub`: default env (`ingress=xbridge ingress_reason=cross_adapter`) |
| vk | same-adapter | `DXR_WEAVE_ON_SCANOUT=0` | `hub`: default env (`ingress=xbridge ingress_reason=default_xbridge`); `hub-direct`: + `DXR_HUB_INGRESS=direct` (P2a) |
| gl | both | default env | - (reference row) |

```powershell
.\run_hub_ab.ps1 -AppDir <repo>\test_apps\build\bin                      # all apps, n=3 x 20 s
.\run_hub_ab.ps1 -AppDir ... -Apps d3d12,vk -Topology same-adapter     # hybrid box: render on the scanout GPU
.\run_hub_ab.ps1 -AppDir ... -Windows 10 -Reps 1                        # validation pass
.\run_hub_ab.ps1 ... -RuntimeJson <dev json> -RuntimePath <_package\bin> # a from-source runtime
.\run_hub_ab.ps1 ... -PresentMon <PresentMon.exe>                       # optional present-mode columns
```

- **Topology matters on a hybrid laptop.** The d3d12 reroute onto the fill arm
  is accepted **same-adapter only**: with the app on the dGPU and the panel on
  the iGPU, default d3d12 engages its *own-legs* split (path `own-split`), and
  the `hub` arm is then REFUSED, correctly. `-Topology same-adapter` adds
  `DXR_D3D_FORCE_GPU=scanout` / `DXR_VK_FORCE_GPU=scanout` so render, weave and
  present share the scanout adapter (what a single-GPU desktop sees by default).
  `-Arms own-split` runs d3d12's own split as a third arm.
- **Verification, not assumption.** Each run's path is classified from the app
  log (`weave placement:` / `hub placement:` verbatim + the split/reroute ACTIVE
  lines) and cross-checked against the `[WITNESS]` site (`d3d11` for the fill
  arm/hub, the API's own site for native). A mismatch, a lost foreground, a
  missing witness, a missing `expectLine`, a peer XR app mid-window, or a runtime/DP different from the pinned one
  (`-ExpectRuntime`, default = the first run's) marks the run REFUSED: kept in
  `runs.csv`, never counted. Environmental refusals (focus, peer app, app exit) are retried up to
  `maxRetries` so the interleaved pair survives; a path mismatch is not.
- **R / D.** `DXR_WEAVE_LATENCY_CSV=<run dir>\wl` writes `wl.<site>.csv`; R is
  joined F->S exactly as `weave_latency.py` does (app frames only, window
  limited by QPC). D rows exist only where the site is handed
  `predicted_display_time_ns`: the native `d3d11`/`d3d12` sites and, since P1c,
  the hub's `d3d11` site do; `vknative` does not. Since #2327 the hub pairs
  each weave with the promise of the frame it actually weaves (by seq), and the
  d3d12/vk callers predict from the hub target's measured #867 lookahead.
  `vknative` needs `VK_KHR_present_wait` - absent there, no CSV at all.
- **Hygiene.** Apps launch de-elevated via `explorer.exe` -> `.bat` (env is
  process-level, so the runtime's `getenv` sees it), centred, forced to the
  foreground and polled every 500 ms (losing focus moved R p50 from 16.6 to
  66-133 ms in validation). Before every launch the harness waits up to
  `-WaitForPanelMin` while any other XR client runs; it only ever stops the pid
  it started and never touches `displayxr-service`.
- Output: `results\hub-ab-<stamp>[-label]\` - `runs.csv`, `summary.md`
  (arm x app medians, verdict per app from the interleaved pairs, placement
  evidence, every run), `environment.json`, `capabilities.json`,
  `displayxr-cli-info.txt`, and per run `launch.bat`, `app.log`, `wl.*.csv`,
  `presentmon.csv`, `run.json`.
