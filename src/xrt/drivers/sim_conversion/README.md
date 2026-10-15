# DisplayXR-SimConversion

The hardware-free reference **conversion provider** (ADR-053;
`docs/specs/runtime/conversion-provider-discovery.md` §6.1). Id `sim-conversion`,
ProbeOrder **250**, `XRT_CONV_PROVIDER_FLAG_FALLBACK`: selection never picks it
over a real capability. Built as `DisplayXR-SimConversion.dll` / `.so` / `.dylib`;
POSIX discovery manifest `250-sim-conversion-conversion-provider.json`.

It is not a model. Every output is synthetic, cheap and **deterministic** (CPU
compute; same input ⇒ same bytes):

| cap_id = task | Lanes | Input | Output |
|---|---|---|---|
| 1 `DEPTH_MONO` | stream + job | mono | R32F vertical ramp, `AFFINE`, 0 at the top; job = `RAW_IMAGE` blob |
| 2 `DEPTH_VIDEO` | stream | mono | ramp × (1 + 0.1·sin(phase)), `TEMPORAL`; `FRAME_RESET` snaps the phase to 0 |
| 3 `DISPARITY_STEREO` | stream + job | stereo pair | constant `stereo.infinity_shift_px + 4` px, `STEREO_DISPARITY_PX` |
| 4 `VIEWS_MONO` | stream | mono | N ≤ 8 shifted copies (ROW or ARRAY) + aux metric depth 1 → 2 m |
| 5 `VIEWS_FROM_DEPTH` | stream | colour + depth | depth-scaled shift (nearer = more parallax) |
| 6 `SPLATS_MONO` | job | mono | two-layer 3DGS PLY, `metric = 0`, intrinsics `ASSUMED` |
| 7 `SPLATS_STEREO` | job | stereo pair | as 6; `metric = 1` only when `stereo.baseline_m > 0` |

**Viewpoints are honoured exactly.** View *i* samples the input shifted by
`0.1 · strength · x_i / rect_width_m` of the view width (`sim_conv_synth.h`):
each view follows its own viewpoint, so an asymmetric pair renders an
asymmetric parallax. Only without viewpoints (or with `rect_width_m == 0`)
does it fall back to the historical fan of 2 % of the view width per view.

**Transport.** Every capability imports `XRT_CONV_MEM_CPU`. On Windows it also
imports `XRT_CONV_MEM_WIN32_NT_HANDLE` with `SYNC_NONE` / `SYNC_FENCE_NT`, on a
D3D11 device of its own (`sim_conv_d3d11_io.cpp`): inputs are read back, the
output is synthesised on the CPU and uploaded, so the bytes match the CPU path.
A CPU `ARRAY` views target holds its layers back to back (layer stride =
`row_stride · height`).

**Shorter caller structs** are accepted everywhere: the provider reads and
writes only within each struct's `struct_size`.

The DP-side fake (`SIM_DISPLAY_FAKE_LIFT=1`, `sim_display_lift_d3d11.cpp`) is a
thin wrapper over the same synthesis (`sim_conv_synth.{h,c,cpp}`, the PLY in
`sim_conv_fake_ply.c`). It is deprecated and removed one release after the
lift thread runs on the conversion core.

## Settings

Read through `host->get_setting` in the hosting process only (never `getenv`):
`Settings\<KEY>` under `HKLM\Software\DisplayXR\ConversionProviders\sim-conversion`
on Windows, the manifest's `settings` object on POSIX, overridden per user by
`displayxr-cli conv set sim-conversion <KEY> <value>` (HKCU `Settings\<KEY>` /
the POSIX `sim-conversion.settings.json`; the one options contract, ADR-054
decision 7 / N3c) and by the environment variable
**`DXR_CONV_SIM_CONVERSION_<KEY>`** (in the service's own environment, or the
conversion host's), which always wins. `displayxr-cli conv options
sim-conversion` lists the table below with each key's effective value and
source (the provider declares it through `describe_settings`).

| Key | Default | Effect |
|---|---|---|
| `ENABLE` | 0 | `probe()` declines unless 1 |
| `WARM_MS` | 0 | every capability: WARMING for this long (progress 0 → 1) on its first `warm`, then READY |
| `DOWNLOAD_MS` | 0 | every capability: DOWNLOADING for this long first; `warm` calls `host->model_request("sim/fake")` |
| `STATE_<cap>` | — | `<cap>` = the cap id (`STATE_4`) or the task name upper-cased (`STATE_VIEWS_MONO`). Value `absent`, `incompatible`, `unlicensed`, `disabled`, `failed:<reason>` (static, reported from probe on) or `installed` (cold until `warm`) |
| `LATENCY_MS` | 8 | sleep per `stream_convert` |
| `JOB_MS` | 2000 | `job_execute` duration, with progress; cancel polled every ≤ 50 ms |
| `JOB_FAIL_AT` | — | a job fails once its progress reaches this fraction (0 … 1) |
| `HANG_MS` | 0 | extra sleep per `stream_convert` (stall watchdog) |
| `CRASH_AFTER` | 0 | after N successful converts / jobs: in a satellite (`host_flags` bit 0) the process exits (`_Exit(70)`); in-service every capability reports `FAILED(CRASH_LOOP)` and work fails |
| `DEVICE_LOST_EVERY` | 0 | every Nth convert / job fails and its capability reports `FAILED(DEVICE_LOST)`, retryable; the next `warm` reloads |
| `LICENCE` | — | `research` ⇒ `RESEARCH_ONLY`, `commercial_ok = 0` (the `AllowResearchOnly` gate) |
| `SCRIPT` | — | the compact per-task script below; applied last, so it wins |

### `SCRIPT` grammar

```
SCRIPT    := directive { ";" directive }
directive := [ target "." ] key "=" value          (whitespace around a directive is ignored)
target    := task name (views-mono | VIEWS_MONO | depth-video | ...) | cap id 1..7
             omitted = every capability
key       := state      value = step { "," step }
           | download   value = ms        (DOWNLOADING before the rest)
           | warm       value = ms        (WARMING before the terminal state)
           | fail       value = reason    (the terminal state becomes FAILED(reason))
           | cold       value = 0 | 1     (INSTALLED until the first warm, even for one step)
           | latency_ms | job_ms | hang_ms | crash_after    (global; target ignored)
step      := state-name [ "(" reason ")" ] [ ":" ms ]
state-name:= installed | downloading | warming | ready | evicted | absent
           | incompatible | unlicensed | failed | disabled
reason    := licence | load | platform | network | version | resources | model
           | device_lost | crash_loop
```

Names are case-insensitive. A `state=` list replaces the capability's sequence;
every step but the last runs for its `ms`, the last is terminal (its `ms` is
ignored). `failed` without a reason means `failed(load)`.

**When the clock runs.** Before the first `warm` a capability reports its
script's only step when the script has one step (static: `ready`, `absent`,
`failed(...)` …), else `INSTALLED`. `warm` starts the script and blocks until
the terminal step, publishing each transition through `host->notify_changed`
and progress / `eta_ns` through `get_state`; it returns success only on READY.
A cancelled `warm` returns promptly and the load keeps its clock. `evict` turns
READY into `EVICTED`; the next `warm` reruns the script. A retryable failure
(`load`, `network`, `resources`, `device_lost`) reruns on the next `warm`; a
non-retryable one (`version`, `model`, `licence`, `platform`) stays.

Examples:

```
views-mono.state=warming:5000,ready                      WARMING 5 s → READY
splats-mono.state=downloading:3000,warming:1000,ready    DOWNLOADING with progress
depth-video.fail=version; depth-mono.fail=model          FAILED(VERSION) vs FAILED(MODEL)
crash_after=20                                           a crash after 20 conversions
download=2000; warm=500; latency_ms=30                   every task, slower
```

Set it as one string, e.g. `DXR_CONV_SIM_CONVERSION_SCRIPT=views-mono.state=warming:5000,ready`.

## Tests

`tests/tests_sim_conversion.cpp` links the provider statically
(`sim_conversion_static`) and calls `xrtConversionPluginNegotiate` directly.
