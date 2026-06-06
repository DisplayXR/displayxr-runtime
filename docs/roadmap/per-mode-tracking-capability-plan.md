# Per-Mode Tracking Capability + Tracking-State Event — Implementation Plan

**Status:** ✅ COMPLETE (2026-06-06) — all 5 phases shipped as runtime **v1.13.0** + vendor plug-in ABI-v3 rebuild + bundle **v0.13.0**; #441 closed. Architectural decisions promoted to **ADR-022** (flags-word vendor ABI + frozen-at-v13 app struct / chaining policy).
**Scope:** runtime, sim_display plug-in (in-tree), vendor plug-ins (own repos), extension header v14, IPC.
**Breaking:** plug-in ABI v2 → v3 (coordinated release). Zero app-binary breakage by design.

## Goal

1. Every display rendering mode carries its own **`has_tracking`** capability — e.g. Leia can
   expose a "2D tracked" mode alongside the default 3D mode; sim_display honestly reports
   no tracking on all modes.
2. **`is_tracking`** (per-frame, DP-reported) reaches apps truthfully on every path —
   including IPC, where it is currently faked to TRUE.
3. New **`XrEventDataEyeTrackingStateChangedEXT`** event so MANUAL-mode apps get edge-triggered
   tracking-loss/recovery notification instead of polling `XrViewEyeTrackingStateEXT`.
4. MANAGED/MANUAL contract unchanged (already specified in
   `docs/specs/vendor/eye-tracking-modes.md`); this plan implements the missing plumbing.

## Design decisions (locked)

| Decision | Choice |
|---|---|
| Capability layering | Per-mode `has_tracking` = "does this rendering mode consume live tracking". System-level `supported_eye_tracking_modes` (MANAGED/MANUAL bits) = "which control contracts the vendor offers". Consistency rule: `supportedModes != 0` ⇔ at least one mode has `has_tracking = true`. |
| `isTracking` computation | `active_mode.has_tracking && eye_pos.valid && eye_pos.is_tracking`. Replaces the contradictory fallback heuristic at `oxr_session.c:1747-1764`. |
| sim_display | All 5 modes `has_tracking = false`; `supported_eye_tracking_modes = 0`. Dev-only env toggle `SIM_DISPLAY_FAKE_TRACKING=1` re-enables MANUAL_BIT + `has_tracking=true` (+ optional `SIM_DISPLAY_FAKE_TRACKING_PERIOD_MS=N` to square-wave `is_tracking` for event testing). Env var, not registry — this is a dev override, not a product feature. |
| `xrRequestEyeTrackingModeEXT` | Stays a **session preference** validated against system `supportedModes` only. Latent (no error) while the active rendering mode is untracked. No ordering race with workspace mode switches. |
| `activeMode` in untracked modes | Keeps reporting the session's MANAGED/MANUAL preference. No new "NONE" enum value. |
| App-facing per-mode capability | **Chained struct**, NOT a field append. `oxr_xrEnumerateDisplayRenderingModesEXT` writes with the runtime's compiled stride (`oxr_api_session.c:1497`), so appending to `XrDisplayRenderingModeInfoEXT` (the v12/v13 precedent) corrupts memory in app binaries compiled against older headers (Unity plug-in, shipped demos) — and unlike the plug-in side there is no version gate to reject them cleanly. Chaining is zero-break AND the canonical Khronos pattern (cf. `XrSystemProperties` capability chaining, which `XrEyeTrackingModeCapabilitiesEXT` already uses). **Policy: `XrDisplayRenderingModeInfoEXT` is frozen at its v13 layout; all future per-mode fields chain.** |
| Plug-in ABI | `uint32_t mode_flags` (bit 0 = `XRT_RENDERING_MODE_FLAG_HAS_TRACKING`) + `uint32_t reserved[3]` added to the vendor-provided section of `struct xrt_rendering_mode` (`xrt_device.h:249`). The array is embedded in `xrt_device`, which the plug-in creates (`plugin->create_device`), so element stride changes → `XRT_PLUGIN_API_VERSION_CURRENT` 2 → 3. Loader rejects mismatched plug-ins; versions.json ABI gate coordinates the release. A flags word + reserved padding (zero-init = all off = untracked = safe) means future per-mode capabilities are new bits, not new fields — **this is intended to be the last rendering-mode ABI break**. |
| MANAGED auto-switch × workspace authority | Vendor-initiated MANAGED transitions are **hardware display state** changes: they fire `XrEventDataHardwareDisplayStateChangedEXT` (and the new tracking event), and flow through the existing DP→runtime event path. They are NOT rendering-mode requests, so the workspace-controller authority gate (#233/#234) is untouched. Under a workspace, the vendor SHOULD still honor the 2D/3D weave decision globally (it owns the panel); the shell observes the event like any session. |

## New API surface (extension header v14)

```c
// Chained (input) to each XrDisplayRenderingModeInfoEXT element's next by apps that opt in.
#define XR_TYPE_DISPLAY_RENDERING_MODE_TRACKING_INFO_EXT ((XrStructureType)1000999012)
typedef struct XrDisplayRenderingModeTrackingInfoEXT {
    XrStructureType    type;
    void* XR_MAY_ALIAS next;
    XrBool32           hasTracking;  //!< Mode consumes live eye tracking
} XrDisplayRenderingModeTrackingInfoEXT;

// Queued on every is_tracking edge (and on transitions into/out of untracked modes).
#define XR_TYPE_EVENT_DATA_EYE_TRACKING_STATE_CHANGED_EXT ((XrStructureType)1000999013)
typedef struct XrEventDataEyeTrackingStateChangedEXT {
    XrStructureType       type;
    const void* XR_MAY_ALIAS next;
    XrSession             session;
    XrBool32              isTracking;  //!< New state
    XrEyeTrackingModeEXT  activeMode;  //!< Session's MANAGED/MANUAL preference at edge time
} XrEventDataEyeTrackingStateChangedEXT;
```

`XrEyeTrackingModeCapabilitiesEXT`, `XrViewEyeTrackingStateEXT`, and
`xrRequestEyeTrackingModeEXT` are unchanged.

## Phases

### Phase 0 — Spec & docs (doc-only, direct to main)
- `docs/specs/vendor/eye-tracking-modes.md`: add per-mode `has_tracking` section, layering +
  consistency rules, the new event, the workspace-authority resolution above, and the
  vendor decision table row for "2D tracked" modes.
- `docs/specs/extensions/XR_EXT_display_info.md`: v14 additions (chained struct + event),
  explicit note on why chaining (binary compat) — sets precedent for future per-mode fields.
- `docs/guides/vendor-plugin-onboarding.md` + `docs/reference/xrt_plugin_iface.md`:
  `has_tracking` is in the "driver MUST set" block (no `struct_size` clamp on
  `xrt_rendering_mode`; zero-init = untracked = safe default).

### Phase 1 — Runtime core + ABI v3 (PR 1)
1. `xrt_device.h`: `uint32_t mode_flags` (`XRT_RENDERING_MODE_FLAG_HAS_TRACKING = 1u << 0`) +
   `uint32_t reserved[3]` in the vendor-provided section of `xrt_rendering_mode`.
2. `xrt_plugin.h`: `XRT_PLUGIN_API_VERSION_3`, bump `XRT_PLUGIN_API_VERSION_CURRENT`.
3. `XR_EXT_display_info.h` → v14: the two structs above (auto-syncs to
   `displayxr-extensions` via `publish-extensions.yml`).
4. `sim_display_device.c`: `has_tracking = false` on all modes; `sim_display_plugin.c`:
   `supported_eye_tracking_modes = 0` by default; `SIM_DISPLAY_FAKE_TRACKING` env toggle
   (advertise MANUAL_BIT, `has_tracking = true` on 3D modes, square-wave `is_tracking` in
   `sim_dp_get_predicted_eye_positions` when `..._PERIOD_MS` set).
5. `oxr_api_session.c` enumerate: walk each element's input `next` chain
   (`OXR_GET_OUTPUT_FROM_CHAIN` pattern) and fill `hasTracking`. Stop nulling `next`
   unconditionally.
6. `oxr_session.c`: replace the isTracking fallback block (`:1747-1764`) with
   `active_mode.has_tracking && eye_pos.valid && eye_pos.is_tracking`. Requires plumbing the
   head device's active mode's `has_tracking` to the session (read via
   `GET_XDEV_BY_ROLE(... head)->rendering_modes[active_rendering_mode_index]`).
7. Event: `sess->last_is_tracking` edge detection at the end of the `xrLocateViews` path;
   enqueue via the `oxr_event.c` pattern (`oxr_event.c:398`). Also fires on mode switches
   that flip effective tracking (handled naturally since isTracking recomputes per locate).
8. `cli_query.c` (`displayxr-cli info`): print `has_tracking` per rendering mode.
9. Consistency warning at init (`target_instance.c`): `supportedModes != 0` but no tracked
   mode (or inverse) → one-shot `U_LOG_W`.

Verify: `ctest`, `./scripts/build-mingw-check.sh`, `displayxr-cli selftest` (CI gate
exercises plug-in discovery + new ABI), cube_handle_d3d11 with `SIM_DISPLAY_FAKE_TRACKING=1`
observing the event + isTracking edges.

### Phase 2 — IPC transport (PR 2)
- Carry `xrt_eye_positions` (positions + `valid` + `is_tracking` + timestamp) to IPC clients:
  extend the IPC view-pose sync (proto.json — renames cascade per the proto codegen rules)
  or the shared-memory frame state, whichever carries view poses today.
- Wire `client_*_compositor` `get_predicted_eye_positions` so
  `oxr_session_get_predicted_eye_positions` (`oxr_session.c:158`) succeeds for IPC sessions;
  delete the IPC fake-TRUE fallback.
- Verify: `displayxr-service.exe` + `XRT_FORCE_MODE=ipc` + cube_handle_d3d11 (NOT cube_ipc),
  with `SIM_DISPLAY_FAKE_TRACKING=1` — IPC app must see the same isTracking edges + events
  as in-process.

### Phase 3 — Leia plug-in (`displayxr-leia-plugin`)
- Sync new runtime headers; report ABI 3.
- Set `has_tracking = true` on the 3D mode(s); decide whether to add the "2D tracked" mode
  now or later (the capability plumbing supports it either way).
- `is_tracking` from the SR SDK is already wired through `get_predicted_eye_positions`;
  audit it against the MANAGED grace-period recommendation in the spec (SHOULD stay true
  during collapse animation, flip false on actual 2D fallback).

### Phase 4 — Apps, linter, polish (PR 3, runtime)
- cube test apps: HUD line for `isTracking`/`activeMode` + log the new event (gives every
  manual test session free visibility).
- `docs/guides/displayxr-app-rules.md` + `scripts/check_displayxr_app.py`: advisory rule —
  apps requesting MANUAL mode SHOULD handle `XrEventDataEyeTrackingStateChangedEXT`.
- WebXR bridge + Unity plug-in: no changes required (chained struct is opt-in); file
  tracking issues to adopt the new event when convenient.

### Phase 5 — Coordinated release
1. Land PRs 1-3 on runtime `main`; land Leia plug-in PR (built against the v3 headers).
2. `/release minor` (runtime) — versions.json self-bump runs; the **leia ABI gate**
   (`scripts/check_plugin_abi.py`) will detect the old plug-in's ABI 2 ≠ 3, skip its bump,
   and auto-open the tracking issue on `displayxr-leia-plugin`. Expected, by design.
3. `/dxr-release leia-plugin vX.Y.Z` — built with ABI 3; versions-bump now passes the gate.
4. `/installer-release` — ship the matched runtime + plug-in pair in one bundle so end
   users can't land in the mixed state.

Mixed-state behavior (by construction): new runtime + old Leia plug-in → loader rejects ABI 2
→ sim_display fallback (priority -20) — degraded to sim, no crash, `selftest` flags it.
Old runtime + new plug-in → equally rejected. The bundle release closes the window.

## App-compat matrix

| App | Impact |
|---|---|
| Existing binaries (cube, gauss, Unity, WebXR) | Zero change in struct layout; on Leia, isTracking behavior identical. On sim, isTracking becomes honestly FALSE (was inconsistent TRUE/FALSE) — only test code asserting TRUE would notice. |
| IPC/service-mode apps | isTracking becomes truthful (was always TRUE). Behavior *improvement*; apps that ignored it see no difference. |
| New apps | Opt into per-mode `hasTracking` via chaining; subscribe to the tracking event for MANUAL mode. |

## Out of scope
- Per-mode MANAGED/MANUAL capability (system-level stays sufficient).
- "NONE" eye-tracking-mode enum value.
- Leia "2D tracked" product mode itself (enabled by, not part of, this work).
