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
