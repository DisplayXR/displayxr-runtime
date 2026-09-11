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
aliases, unknown fields, nonnumeric/nonfinite values and malformed JSON reject
the complete record. JSON input is bounded to 64 KiB. File paths support UTF-8,
including non-ASCII executable basenames on Windows.

Baseline conversion uses a fixed reference rather than a per-frame division by
tracked eye separation. The selected world scale therefore stays constant when
the viewer changes. No tracked-IPD normalization switch is provided.

## Validation

`tests_camera_profile` covers parsing, defaults, scalar bounds, alias conversions
for 16:9 and 16:10 canvases, file/override precedence, invalid data, size limits
and Unicode paths. `tests_qwerty_camera_profile` exercises actual qwerty devices:
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
