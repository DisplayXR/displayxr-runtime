# XR_DXR_stereo_comfort

| Property | Value |
|----------|-------|
| Extension Name | `XR_DXR_stereo_comfort` |
| Spec Version | 1 |
| Type Values | `XR_TYPE_STEREO_COMFORT_DXR` (1004999440) · `XR_TYPE_STEREO_COMFORT_REQUEST_DXR` (1004999441) |
| Author | The DisplayXR Project |
| Platform | All. Works on every graphics API, in-process and over IPC; filled in the state tracker. |

---

## 1. Overview

How much disparity a viewer tolerates depends on how far they sit from the glass, not on the app.
The eyes converge on a point at distance `z` while they focus on the glass at distance `V`, and
comfort is a bound on the vergence change

    ΔD = 1/V − 1/z        (diopters; positive behind the glass, negative in front)

which lands on the glass as the disparity `p = e · V · ΔD` (`e` = 0.063 m). A percentage of the
canvas width measured on one display is wrong on another size or at another distance; a diopter
bound is not.

`XR_DXR_stereo_comfort` reports that bound for the current viewer. An app that sizes its eye
separation from it is right on every display, and follows the user's depth preference without
having a setting of its own.

> The **runtime** owns the zone: which published bound, the user's depth preference, and the
> viewer distance it tracks. The **app** owns its scene (its nearest and farthest points, its eye
> separation and convergence) and fits it inside the zone. Nothing here changes the views
> `xrLocateViews` returns.

The first user is the stereo mode of the D3D11/D3D12 translation layers used for existing games:
they size their separation gain and disparity ceiling from it instead of fixed numbers.

### Opt-in, zero cost otherwise

The runtime fills `XrStereoComfortDXR` only when it is chained on `XrViewState::next`, and then on
every such call.

## 2. Zones

The zones are the ones the DisplayXR web SDK's `comfortZone()` states (`js/inline3d-depth-budget.js`
in `displayxr-web`), with the same arithmetic.

| Zone | Front bound | Behind bound | At V = 0.6 m |
|---|---|---|---|
| `SHIBATA` (default) | `Dv/1.035 + 0.626 − Dv` | `Dv − (Dv/1.129 − 0.442)`, or +∞ when that far limit is ≤ 0 | 0.57 D / 0.63 D, 21.5 / 24.0 mm |
| `PERCIVAL` | 1/3 D | 1/3 D | 12.6 / 12.6 mm |
| `CUSTOM` | `customDiopters` | `customDiopters` | |

`Dv = 1/V`. Shibata et al., "The zone of comfort", J. Vision 11(8):11, 2011. These zones are
necessary, not sufficient: crosstalk can make a bright subject ghost well inside them, and moving
content tolerates less than still content (see the web SDK's authoring guide).

## 3. New structures

```c
typedef enum XrStereoComfortZoneDXR {
    XR_STEREO_COMFORT_ZONE_DEFAULT_DXR = 0,   // request only
    XR_STEREO_COMFORT_ZONE_SHIBATA_DXR = 1,
    XR_STEREO_COMFORT_ZONE_PERCIVAL_DXR = 2,
    XR_STEREO_COMFORT_ZONE_CUSTOM_DXR = 3,
} XrStereoComfortZoneDXR;

// Optional input, chained on XrViewLocateInfo::next.
typedef struct XrStereoComfortRequestDXR {
    XrStructureType          type;   // XR_TYPE_STEREO_COMFORT_REQUEST_DXR
    const void*              next;
    XrStereoComfortZoneDXR   zone;
    float                    customDiopters;   // CUSTOM only
} XrStereoComfortRequestDXR;

// Output, chained on XrViewState::next; the runtime fills it.
typedef struct XrStereoComfortDXR {
    XrStructureType          type;   // XR_TYPE_STEREO_COMFORT_DXR
    void*                    next;
    XrStereoComfortZoneDXR   zone;                   // applied; never DEFAULT
    float                    viewerDistanceMeters;   // V
    XrBool32                 viewerTracked;          // V from tracked eyes, else nominal
    float                    frontDiopters;
    float                    behindDiopters;         // may be +inf
    float                    frontDisparityMeters;   // e * V * frontDiopters
    float                    behindDisparityMeters;  // may be +inf
    float                    userDepthScale;         // (0, 1], already applied
} XrStereoComfortDXR;
```

## 4. Behaviour

- **Viewer distance.** The mean distance of the tracked eyes from the display plane: from the
  app's `XrViewDisplayRawDXR` block when it chains one and it is tracking (this covers IPC), else
  from the in-process locate, else the display's nominal viewer (`viewerTracked = XR_FALSE`).
  Values are per locate and unsmoothed.
- **Zone.** The app's request, else the runtime setting, else `SHIBATA`. A `CUSTOM` request with a
  bound ≤ 0, or an unknown zone, falls back to the runtime setting.
- **User preference.** A factor in (0, 1] that scales both bounds (1 = the published zone). It
  applies to every app, requested zone included, so a user who wants less depth gets it
  everywhere.
- **Settings (v1).** `DXR_COMFORT_ZONE` = `shibata` | `percival` | a positive number of diopters;
  `DXR_COMFORT_DEPTH_SCALE` = the user preference. Both are read in the process that runs the
  state tracker (the app's).

## 5. Using it

Disparity as a fraction of the canvas width is `p / canvasWidthMeters` (the canvas size is in
`XrViewDisplayRawDXR::canvasSizeMeters`). For a camera that shifts each eye's image by a fraction
`s` of the half-width per unit of inverse depth (a translation-layer stereo mode), content at
infinity lands at `s · W` on the glass, where `W` is the canvas half-width in metres; keeping it
inside the zone means `s · W ≤ behindDisparityMeters`. For a camera rig, the web SDK's
`cameraRigComfortFactor()` gives the eye-separation multiplier that fits a scene's nearest and
farthest points.
