# Shell Phase 8 Status: 3D Capture MVP

**Branch:** `feature/shell-phase8`
**Status:** In progress — 8.1, 8.2, 8.3, 8.4, 8.6 implemented; awaiting live verification. 8.5 deferred.
**Date:** 2026-04-13

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
| [x] | 8.4 | Shell: `Ctrl+Shift+3` hotkey, filename policy, IPC call, cJSON sidecar |
| [ ] | 8.5 | Capture flash indicator — DEFERRED (per agent prompt: skip if it slows down MVP) |
| [x] | 8.6 | Phase 7 file-trigger now routes through `capture_frame` (output renamed to `shell_screenshot_sbs.png`) |

## Commits

_(none yet — pending live verification)_

## Design Decisions

- **`char[256]` not supported by IPC schema.** The original plan called for `path_prefix` as a raw `char[256]` parameter to `shell_capture_frame`, but the proto schema only accepts scalar/struct/enum/handle types. Wrapped the path + flags in a new `struct ipc_capture_request` instead.
- **File-trigger output renamed.** Previously `%TEMP%\shell_screenshot.png`; now `%TEMP%\shell_screenshot_sbs.png` to keep a single code path. Memory file `reference_runtime_screenshot.md` will need a one-line update once verified.
- **Mono fallback.** When `tile_columns == 1`, requesting `LEFT` and `RIGHT` both write the full-frame (eye_x = 0). SBS is identical to L/R in this case.

## Known Issues

- **Color format unverified.** Atlas may be BGRA8 rather than RGBA8 — Phase 7 didn't normalize, and Phase 8 follows the same convention. If captured PNGs show R/B-swapped colors, add a swizzle pass during the L/R copy loop.
- **`shell_capture_frame` while shell is *inactive*.** The service handler returns `XRT_ERROR_IPC_FAILURE` if `combined_atlas` doesn't exist (i.e., shell not active). The shell-side wrapper guards on `g_shell_active` before issuing the call.
