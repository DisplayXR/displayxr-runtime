# Shell Phase 8 Status: 3D Capture MVP

**Branch:** `feature/shell-phase8`
**Status:** MVP complete — live-verified 2026-04-15. Hotkey is Ctrl+Shift+C; SBS-only output. 8.5 deferred. Compositor stretch bug (#158) blocks adding the active-region crop back.
**Date:** 2026-04-13, updated 2026-04-15

## Scope

Deliver the MVP of the 3D capture pipeline: `Ctrl+Shift+3` in the shell captures the pre-weave L/R stereo pair to disk with a metadata sidecar. Promotes the Phase 7 file-trigger screenshot to a shell-owned, IPC-driven feature with L/R separation and metadata.

**Full plan:** [shell-phase8-plan.md](shell-phase8-plan.md)
**Full spec:** [3d-capture.md](3d-capture.md)

## Tasks

| Status | Task | Description |
|--------|------|-------------|
| [x] | 8.1 | IPC protocol: capture flags, `ipc_capture_request`/`ipc_capture_result` structs, `shell_capture_frame` call |
| [x] | 8.2 | Service: `comp_d3d11_service_capture_frame` with L/R/SBS outputs (new function, not a refactor) |
| [x] | 8.3 | L/R sub-image extraction from combined atlas staging texture |
| [x] | 8.4 | Shell: `Ctrl+Shift+C` hotkey, filename policy, IPC call, cJSON sidecar |
| [ ] | 8.5 | Capture flash indicator — DEFERRED (per agent prompt: skip if it slows down MVP) |
| [x] | 8.6 | Phase 7 file-trigger now routes through `capture_frame` (output renamed to `shell_screenshot_sbs.png`) |

## Commits

- `6f3eed00b` Shell 8.1: IPC protocol for shell_capture_frame
- `62158f3ad` Shell 8.2/8.3/8.6: capture_frame impl + L/R split + file-trigger refactor
- `fa65a2021` Shell 8.4: Ctrl+Shift+3 capture hotkey + JSON sidecar (later renamed to Ctrl+Shift+C)
- `be9e6c409` Docs: initial Phase 8 status update
- `0773e5388` Shell 8: SBS-only capture, Ctrl+Shift+C hotkey, uncropped atlas dump

## Design Decisions

- **`char[256]` not supported by IPC schema.** The proto schema only accepts scalar / struct / enum / handle types, so the path prefix is wrapped in `struct ipc_capture_request` (prefix + flags) rather than passed as a raw `char[N]` parameter.
- **Output param `result` → `capture_result`.** The generated reply struct already has `xrt_result_t result`; the Phase 8 output struct must use a different name to avoid a C struct-member collision.
- **Hotkey is Ctrl+Shift+C, not Ctrl+Shift+3.** Ctrl+Shift+3 passed through to the compositor's layout-preset handler (which only checks Ctrl+digit and ignores Shift), so both fired.
- **SBS-only, no L/R PNGs.** Per-eye PNGs were redundant with SBS — removed. The IPC flag plumbing remains (LEFT/RIGHT/SBS) for future use.
- **Uncropped atlas dump.** The capture dumps the full atlas (`atlas_w × atlas_h`) without cropping to an "active region". The previous crop assumed the compositor rendered each tile at `view_w × view_h`; in practice the compositor is stretching each view to fill `atlas_w/tile_columns × atlas_h/tile_rows`. Honest uncropped output surfaces that bug. Re-add the crop once #158 lands.
- **File-trigger output renamed** `%TEMP%\shell_screenshot.png` → `%TEMP%\shell_screenshot_sbs.png` so there is a single capture code path.

## Known Issues

- **#158 — Compositor stretches each view vertically in stereo mode.** `view_height = display_height / tile_rows` (= display_height for 2×1 stereo) instead of using `rendering_mode.view_height_pixels`. Visible as tall/narrow content in each eye tile of the capture. Blocks re-adding the crop in capture; doesn't block the Phase 8 MVP itself.
- **8.5 capture flash indicator — deferred.** Not required for MVP per the agent prompt; can be added as a follow-up if desired.
- **Color format unverified.** Atlas may be BGRA8 rather than RGBA8 — Phase 7 didn't normalize, and Phase 8 follows the same convention. Live captures look correct so far.
- **`shell_capture_frame` while shell is inactive.** The service handler returns `XRT_ERROR_IPC_FAILURE` if `combined_atlas` doesn't exist; the shell guards on `g_shell_active` before issuing the call.
