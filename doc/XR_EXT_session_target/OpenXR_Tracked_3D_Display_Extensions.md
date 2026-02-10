# OpenXR Tracked 3D Display Support — Implementation Plan (Updated)

This document defines the **finalized design and implementation guidance** for supporting
tracked 3D displays in OpenXR using **two neutral extensions**, with clear separation between:

- runtime-owned *render-ready* behavior (legacy apps, WebXR)
- app-owned *raw* behavior (native apps, Unity plugin)

This version incorporates the following updates:
- Explicit **canonical display pyramid / frustum** concept
- Alignment with existing code where the window extension already exists under the name
  **“session target”** and must be **renamed**
- Migration from **absolute recommended view sizes** to **(scaleX, scaleY)** with
  explicit support for **anisotropic quality scaling**

---

## 0) Final Extension Names (Authoritative)

### A) Window / HWND binding extension
**Final name:**
```
XR_EXT_win32_window_binding
```

Notes:
- Claude already implemented this under a *“session target”* name.
- This is a **rename only**; semantics stay the same.
- Purpose is explicit: app provides a Win32 `HWND` for OpenXR rendering.
- No camera, geometry, or policy logic belongs here.

---

### B) Display geometry & viewing model extension
**Final name:**
```
XR_EXT_display_info
```

Purpose:
- Expose **physical display geometry**
- Define the **canonical viewing pyramid**
- Provide **nominal viewer pose**
- Provide **recommended render resolution scaling**
- Enable **RAW vs RENDER_READY** view behavior

No vendor branding; suitable for future upstreaming.

---

## 1) Canonical Display Pyramid (Core Concept)

The physical display and nominal viewer pose together define a **canonical display frustum**:

- **Base**: physical display rectangle (real-world size)
- **Apex**: nominal viewer position
- **Edges**: rays connecting apex to display corners

Geometrically this is a **pyramid**, representing the *intended single-view camera* for the display.

This pyramid:
- defines the natural mono view of the display
- anchors zero-parallax and stereo comfort
- is the reference from which all stereo views are derived

Stereo rendering is then:
**sampling this same pyramid from two nearby eye positions** (raw tracked eyes).

---

## 2) Nominal Viewer Pose (Clarified Semantics)

`nominalViewerPoseInDisplaySpace` is:

- **not tracked**
- **static**
- a **design-time expectation** of where the viewer should be relative to the display

Interpretation:
- Actual tracked eyes are expected to **vary around** this pose
- At the nominal pose:
  - parallax is neutral
  - depth feels natural
  - the canonical pyramid is perfectly aligned

The nominal viewer pose:
- anchors stereo geometry
- anchors first-person camera alignment
- defines the apex of the canonical pyramid

---

## 3) View Modes and Ownership

### View Modes
| Mode | Meaning |
|---|---|
| `RENDER_READY` | Runtime returns converged, comfortable stereo |
| `RAW` | Runtime returns raw physical eye positions |

### Ownership Rules
| Condition | Default Mode | Camera Model Owned By |
|---|---|---|
| `XR_EXT_display_info` not enabled | RENDER_READY | Runtime |
| Extension enabled | RAW | App |
| Extension enabled + override | RENDER_READY | Runtime |

Debug / qwerty input:
- Applies **only** in `RENDER_READY`
- Never affects RAW output

---

## 4) Display Space

We **keep `XrSpace` for the display**.

### Display Space Definition
- Origin: center of physical display plane
- Axes:
  - +X right
  - +Y up
  - +Z toward viewer
- Rigidly attached to the display

---

## 5) `XR_EXT_display_info` API (Final Shape)

### 5.1 Display Space Creation
```c
xrCreateDisplaySpaceEXT(...)
```

---

### 5.2 Display Info Query (Single Call)

Replace **absolute recommended sizes** with **quality scales**.

Minimum fields:
- `XrExtent2Df displaySizeMeters`
- `XrPosef nominalViewerPoseInDisplaySpace`
- `float recommendedViewScaleX`
- `float recommendedViewScaleY`

---

## 6) Recommended View Scale Semantics

Per-eye render size is computed as:

```
(widthPx * scaleX, heightPx * scaleY)
```

These scales represent **quality scaling only**.
Aspect ratio is controlled by the window viewport and projection.

Anisotropic scaling is intentional and supported.

---

## 7) RAW Mode Semantics

In RAW mode:
- `XrView.pose.position` → physical eye center in display space
- `XrView.pose.orientation` → identity
- `XrView.fov` → advisory

No runtime policy applies.

---

## 8) Render-Ready Mode Semantics

Runtime returns:
- converged, calibrated stereo views
- derived from the canonical display pyramid

Used for:
- WebXR
- legacy OpenXR apps

---

## 9) Required Code Changes for Claude

- Rename “session target” extension → `XR_EXT_win32_window_binding`
- Update display info API to return scaleX / scaleY
- Update documentation to explain:
  - canonical display pyramid
  - nominal viewer pose
  - anisotropic quality scaling

---

End of instructions.
