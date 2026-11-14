# Per-application legacy camera profiles

Issue [#1381](https://github.com/DisplayXR/displayxr-runtime/issues/1381).

A legacy OpenXR instance can receive camera-rig tuning from a JSON profile when
it first creates a native, in-process camera-mode session with a runtime-owned
window. The profile seeds that instance's shared qwerty head/tuning. Live
keyboard adjustments remain available; Space restores the resolved profile.
Navigation stays with the existing shared head device.

The existing combined IPD/parallax keyboard control still sets both factors to
the same adjusted value. Space restores the profile's separate values.

The profile has **instance lifetime**, not session lifetime. All sessions reading
that instance's qwerty fallback share its tuning, including headless siblings.
An instance that remains headless-only never loads a profile. A headless instance
that later creates an eligible native session becomes profiled at that point,
and closing the native session does not remove its seed from surviving siblings.
Destroying and recreating the instance permits a new lookup.

Display-aware instances (`XR_DXR_display_info`) and instances that create only
external-window/readback/shared-texture, workspace/service, or display-mode
sessions remain unseeded. An explicit camera or display rig chained on a locate
keeps its existing precedence over the keyboard fallback. External/raw view
paths and service/workspace rig policy keep their existing semantics. Independent
per-window input/head devices are outside this feature's scope.

V1 does not seed IPC or service sessions. Their existing synthesis and workspace
override policy remain unchanged.

## Location and selection

Windows reads `%ProgramData%\DisplayXR\app-profiles\<executable-basename>.json`.
For example, `SampleGame.exe` uses `SampleGame.exe.json`. The actual executable
basename is used, not `XrApplicationInfo::applicationName` or a directory path.
POSIX uses `app-profiles/` under the existing runtime configuration root returned
by `u_file_get_config_dir`.

The process environment variable `DXR_LEGACY_CAMERA_RIG` takes precedence over
that file. It accepts either a JSON object (leading whitespace allowed) or a
UTF-8 path to a JSON file. On Windows the loader reads the process environment
directly, so changes made with `SetEnvironmentVariableW` before the first eligible session creation
are visible even if another CRT has already cached its environment.

Selection, parsing and canvas-dependent alias resolution happen once per instance,
lazily at its first eligible native session creation. This supplies the canvas
geometry that is unavailable at instance creation. Editing a file or changing
the override does not reseed existing instances, even if another session is
created. Concurrent session creation claims this initialization once.

A missing per-application file uses the normal defaults without a warning.
Invalid data or an unreadable explicit override is reported and ignored as a
whole; an invalid override does not fall through to a different per-application
file. A missing or invalid profile also completes that instance's lookup attempt.
The loader never creates directories or writes configuration. A new instance
starts with its own defaults and may read a different profile.

## Record

The record uses `XrCameraRigDXR` tuning vocabulary and excludes `pose`:

```json
{
  "ipdFactor": 1,
  "parallaxFactor": 1,
  "convergenceDiopters": 0.5,
  "verticalFov": 1.0,
  "metersToVirtual": 0.7
}
```

| Field | Meaning | Missing-field default |
| --- | --- | --- |
| `ipdFactor` | Absolute eye-separation scale | `1` |
| `parallaxFactor` | Eye-centroid motion scale | `1` |
| `convergenceDiopters` | Inverse convergence distance; zero means infinity | `0.5` |
| `verticalFov` | Full vertical field of view, radians | Existing qwerty `tan(vFOV/2) = 0.3249`, approximately 36 degrees |
| `metersToVirtual` | Constant metres-to-virtual-world scale | `1` |
| `pitchDegrees` | Initial camera pitch, degrees, negative = looking down; also the Space-reset look | `0` |
| `pullbackMeters` | Initial distance behind the default camera position, along the starting look (so with `pitchDegrees` the camera also rises), metres; also part of the Space-reset pose | `0` |

`pitchDegrees` and `pullbackMeters` are the only non-tuning fields: they frame
where the camera *starts*, for titles whose work area sits at arm's length and
hand height (Substance 3D Modeler's clay block starts a few tens of centimetres
below and in front of a camera that starts level at eye height). Everything the
user drives afterwards stays theirs.

`pitchDegrees` must lie in `[-89, 89]` and `pullbackMeters` in `[0, 10]`; outside
those the record is rejected.
The same scalar bounds as a chained camera rig apply: eye factors `[0, 10000]`,
convergence `[0, 20]`, vertical FOV `[0.01, 3.13]`, and positive
metres-to-virtual scale `[0.0001, 100000]`. A nonpositive `metersToVirtual` maps
to `1`, matching the chained descriptor's zero/unset convention. Convergence
then passes through `dxr_rig_clamp_for_comfort` using the nominal viewer distance.
The resolved seed is logged once at WARN when it is applied.

Three optional aliases are converted at session creation:

| Alias | Conversion |
| --- | --- |
| `horizontalFovDeg` | Horizontal degrees in `(0, 180)` converted to vertical radians using the active canvas width/height ratio, then clamped to the canonical vertical bounds; invalid/missing dimensions reject this alias |
| `renderedBaselineMeters` | Positive baseline divided by the fixed 0.063 m reference IPD becomes `metersToVirtual` |
| `convergenceMeters` | Positive distance becomes `convergenceDiopters = 1 / distance` |

Use either a canonical field or its alias, not both. Duplicate keys, conflicting
aliases, nonnumeric/nonfinite values of a known field, out-of-domain alias values
(`convergenceMeters`/`renderedBaselineMeters` ≤ 0, `horizontalFovDeg` outside
`(0, 180)`) and malformed JSON reject the complete record. JSON input is bounded
to 64 KiB. File paths support UTF-8, including non-ASCII executable basenames on
Windows.

**Unknown fields are skipped, not fatal** ([#2450](https://github.com/DisplayXR/displayxr-runtime/issues/2450)).
A field this runtime does not recognise is ignored whatever its value, and the
known fields in the same record still apply, so a profile written for a newer
runtime degrades gracefully on an older one. The runtime logs one WARN naming
the skipped fields when the profile is resolved, for example:

```
Legacy camera profile: unknown camera profile key(s) ignored: pitchDegrees, pullbackMeters
```

Field names are case-sensitive, so a misspelt field (`ipdfactor`) is also
skipped with that WARN rather than rejecting the record; check the log when a
value appears to have no effect.

Baseline conversion uses a fixed reference rather than a per-frame division by
tracked eye separation. The selected world scale therefore stays constant when
the viewer changes. No tracked-IPD normalization switch is provided.

## Automatic 2D and automatic re-convergence

Issues [#2639](https://github.com/DisplayXR/displayxr-runtime/issues/2639) and
[#2640](https://github.com/DisplayXR/displayxr-runtime/issues/2640). The same record
carries two switches that are not camera tuning: they act on the PICTURES a legacy
session submits, so they need no cooperation from the app. Both are **off** unless
enabled, and only a LEGACY session (no `XR_DXR_display_info`) is ever touched: a
display-aware app owns its own display mode and rig.

```json
{
  "auto2d": true,
  "autoConvergence": true,
  "maxShiftFraction": 0.03,
  "maxBehindFraction": 0.03,
  "maxFrontFraction": 0.02
}
```

| Field | Meaning | Default |
| --- | --- | --- |
| `auto2d` | Hardware 2D (left eye alone) while both eyes show one flat picture | `false` |
| `autoConvergence` | Horizontal image shift that keeps the scene's subject near the screen plane | `false` |
| `maxShiftFraction` | Largest applied shift, either sign, fraction of the eye image width, `[0, 0.2]` | `0.03` |
| `maxBehindFraction` | Comfort: the far content (p95 of the measured disparities) at most this far behind the screen, `[0, 0.2]` | `0.03` |
| `maxFrontFraction` | Comfort: the near content (p5) at most this far in front, `[0, 0.2]` | `0.02` |

`auto2d` / `autoConvergence` take `true`, `false`, `0` or `1`; anything else, or a
fraction outside its range, rejects the record like any other invalid value. The
process environment variables `DXR_LEGACY_AUTO_2D` and `DXR_LEGACY_AUTO_CONVERGENCE`
(`1`/`on`/`true` or `0`/`off`/`false`) win over the record's switches when set; the
bounds always come from the record. Unlike the camera tuning, these are read for every
eligible session (at its creation), not once per instance.

**Where it runs.** On the D3D11 hub (`compositor/hub_d3d11/`): the in-process D3D11
tier and the D3D12 tier's reroute, which is the route a legacy hosted D3D12 app that
adopts its own window takes. The policy (`auxiliary/util/u_stereo_auto.h`) is pure and
host-tested (`tests_aux_stereo_auto`) so other tiers can adopt it; the tiers' own
weave legs, Vulkan, OpenGL, Metal, IPC/service sessions and client-presents texture
sessions are not wired yet and stay as they were.

**The probe.** At most ten times a second, on app frames only, the hub copies the
stereo atlas into a mip-chained scratch, generates its mips and copies one small level
(each eye 256–511 px wide) into a staging texture, which it reads on a later frame with
`D3D11_MAP_FLAG_DO_NOT_WAIT`. Nothing on the frame path waits for the GPU. A worker
thread reduces each eye to a 256×24 luma grid and runs the policy. Only a 2×1 stereo
atlas is probed; any other layout (a 2D rendering mode, more views) idles.

**Automatic 2D.** The eyes are flat when they are nearly identical, or when a single
horizontal shift explains their difference (a menu drawn at one depth). 2D after five
consecutive flat probes (about half a second), 3D again on the second consecutive
probe that sees depth, so a cut never flickers the lens. In 2D the hub asks the
display processor for hardware 2D through its normal display-mode request and hands
it tile 0 alone, as a 2D rendering mode would; the stereo atlas keeps arriving, so the
probe sees when the eyes differ again. While 2D is held, the session's own requests
(begin/end session, the V key) are recorded, not forwarded, and the session's last
choice is restored when the hold clears — never a forced 3D. Leaving stereo (a 2D
rendering mode) releases the hold at once.

**Automatic re-convergence.** Every disparity `d` (positive behind the screen) becomes
`d + Δ`: the left view moves by `−Δ/2`, the right by `+Δ/2`, and the uncovered edge
strip is black. `Δ` puts the scene's screen-plane estimate at zero — the centre-weighted
rule of the lift auto-convergence (the blend of the ~p80 of nearness and the dominant
clump with the far background excluded), on block disparities matched between the two
grids — then keeps the far and near content inside `maxBehindFraction` /
`maxFrontFraction` (when both cannot hold, the middle of the two) and `|Δ|` within
`maxShiftFraction`. The first estimate, a scene cut and a return from automatic 2D
snap; otherwise a one-second follower with a dead band; flat or ambiguous probes hold.
Per frame the applied shift moves a few pixels at most. The shift is applied in the
hub's crop stage, before the display processor; it is vendor-neutral and costs one
atlas-sized copy per frame while non-zero.

Why a picture shift rather than the rig's `convergenceDiopters`: on a camera rig the
convergence is the asymmetric frustum the app reads from `xrLocateViews`, and an app
that caches its field of view at startup ignores a changed frustum. Expressing it
through the eye poses would mean toe-in (vertical disparity). The shift needs nothing
from the app.

Log lines (WARN unless noted): `legacy auto-stereo: ON for this legacy session …` once
per session; `legacy auto-stereo: probing the …` at the first stereo frame;
`legacy auto-2D: 2D — both eyes show one flat picture …` and
`legacy auto-2D: back to the session's 3D — the eyes differ again …` per edge;
`legacy auto-convergence: engaged — screen plane moved by …` once; INFO per scene cut
and every 5 s while engaged.

## Disparity does not depend on the display

The profile seeds a **camera** rig, computed by the shared core's camera math
like a chained `XrCameraRigDXR` ([`XR_DXR_view_rig` §3](../extensions/XR_DXR_view_rig.md)).
Neither the display's physical size nor the display rig's virtual height enters
it. For a point `Z` world units in front of the camera, the disparity as a
fraction of the canvas width is

```
ipdFactor × metersToVirtual × (convergenceDiopters − 1/Z) × eyeSeparation
    / (2 × tan(verticalFov / 2) × canvasAspect)
```

(`Z → ∞` for the far limit; positive is behind the glass). The only
display-dependent inputs are the eye separation the display processor reports
(`sim_display`: synthetic 60 mm; a tracked display: the viewer's real eyes) and
the canvas aspect. A profile therefore gives the same canvas-relative disparity
on `sim_display` and on a vendor panel of the same aspect, within the few
percent those two inputs differ. On the glass it is that fraction times the
canvas width in metres. The display's nominal viewer distance enters only the
comfort clamp on `convergenceDiopters`, applied once at seed time.

The `vH` the qwerty diagnostics print alongside a camera rig (default 1.3 m) is
the dormant **display** rig's virtual height, kept for the P toggle. It does
not scale the camera rig's eyes. `tests_camera_rig_disparity` pins the
relation above across panel sizes ([#2451](https://github.com/DisplayXR/displayxr-runtime/issues/2451)).

What `sim_display` cannot reproduce is the panel's optics: how much disparity
a given lenticular panel shows comfortably, before crosstalk and ghosting,
has to be judged on that panel.

## Validation

`tests_aux_stereo_auto` covers the automatic-2D detector (flat, flat at one depth,
two depths, uncorrelated eyes), the hysteresis, the disparity estimate, the comfort
clamp and cap, the temporal policy (first estimate, slow follow, cut snap, flat hold)
and the profile keys above.
`tests_camera_profile` covers parsing, defaults, scalar bounds, alias conversions
for 16:9 and 16:10 canvases, file/override precedence, invalid data, skipped
unknown fields and their notice, size limits and Unicode paths.
`tests_camera_rig_disparity` checks the camera rig's disparity against the
closed form above on `sim_display`'s and two vendor panels' geometry. `tests_qwerty_camera_profile` exercises actual qwerty devices:
profile seeding, live adjustments, Space reset, independent-system defaults and
display-mode exclusion.

`tests_oxr_session_window_binding` covers profile-only exclusion for common
window/readback/shared-texture bindings, late-decoded Xlib/Wayland bindings,
and Android runtime-created hosted windows. The profile check does not rewrite
the shared rendering/input classification. The test requires no graphics surface
and runs on the host; it is not a Linux or Android GPU integration test.

On Windows, explicitly build and run `tests_oxr_camera_profile --native` for the
GPU/native-session probe. It loads the candidate runtime DLL directly and opens
runtime-owned windows using the registered display processor; it changes no
system OpenXR registration. It tests native legacy seeding, per-locate rig
precedence, mixed native/headless sharing, persistence across session recreation,
reloading after instance recreation, and display-aware/headless-only exclusion.
This probe is excluded from automatic CTest and is not a visual stereo-comfort
or controller/navigation acceptance test.
