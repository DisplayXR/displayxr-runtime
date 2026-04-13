# Shell Phase 8: Agent Prompt — 3D Capture MVP

Use this prompt to start a new Claude Code session for implementing Phase 8 on branch `feature/shell-phase8`.

---

## Prompt

```
I'm working on the DisplayXR spatial shell — a spatial window manager for 3D lenticular displays. Phase 8 delivers the MVP of 3D capture: a user-triggered spatial screenshot that saves pre-weave L/R stereo images plus a metadata sidecar.

## Context

Read these docs first (in order):
1. `CLAUDE.md` — project overview, build commands, architecture (Windows)
2. `docs/roadmap/shell-phase8-plan.md` — **the plan you're implementing**
3. `docs/roadmap/shell-phase8-status.md` — task checklist (update as you complete tasks)
4. `docs/roadmap/3d-capture.md` — full 3D capture design (MVP is just frame capture for now)
5. `docs/roadmap/shell-phase7-status.md` — what Phase 7 delivered (file-trigger screenshot you're building on)

## Branch

You are on `feature/shell-phase8`, branched from `main` after Phase 7 was merged. All work goes here. Commits reference #43 (Spatial OS tracking issue).

## Memory files you should rely on

- `reference_runtime_screenshot.md` — how to capture screenshots of the compositor for self-feedback loops (file trigger + PostMessage for launcher toggle)
- `feedback_dll_version_mismatch.md` — after IPC protocol changes, copy both `displayxr-service.exe` AND `DisplayXRClient.dll` to `C:\Program Files\DisplayXR\Runtime\` or apps fail OpenXR init
- `feedback_test_before_ci.md` — wait for user to test locally before pushing
- `feedback_shell_screenshot_reliability.md` — shell screenshots via PrintWindow miss UI during eye-tracking warmup; use the F12 / file-trigger atlas screenshot instead

## What already exists

The Phase 7 screenshot mechanism is 80% of the work:

- **File-trigger screenshot**: compositor checks for `%TEMP%\shell_screenshot_trigger` each frame, reads `combined_atlas` texture, writes full SBS PNG to `%TEMP%\shell_screenshot.png`
- **Capture point**: `multi_compositor_render()` in `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp`, just before `swap_chain->Present()`
- **PNG encoder**: `stb_image_write.h` with `STB_IMAGE_WRITE_IMPLEMENTATION` defined in `comp_d3d11_service.cpp`

What Phase 8 adds:
- IPC call `shell_capture_frame(path_prefix, flags)` so the shell drives capture
- L/R sub-image extraction from the SBS atlas (split into two PNGs)
- Metadata returned via IPC, written by the shell as a JSON sidecar
- Shell hotkey `Ctrl+Shift+3` (in addition to keeping the file-trigger for dev workflows)

## Tasks

### 8.1 — IPC protocol

Add to `src/xrt/ipc/shared/ipc_protocol.h`:
- `IPC_CAPTURE_FLAG_LEFT|RIGHT|SBS|ALL` bitmask defines
- `struct ipc_capture_result` with timestamp, atlas/eye dims, tile columns/rows, display dims (meters), eye poses, views_written bitmask

Add to `src/xrt/ipc/shared/proto.json`:
- `shell_capture_frame(char[256] path_prefix, uint32_t flags) -> struct ipc_capture_result`

Regenerate IPC bindings via the CMake build.

### 8.2 — Service: refactor into `capture_frame_impl`

In `src/xrt/compositor/d3d11_service/comp_d3d11_service.{cpp,h}`, add:

```cpp
bool
comp_d3d11_service_capture_frame(struct xrt_system_compositor *xsysc,
                                 const char *path_prefix,
                                 uint32_t flags,
                                 struct ipc_capture_result *out_result);
```

Implementation:
1. Acquire `sys->render_mutex`
2. Validate `combined_atlas` + device + context
3. Create staging texture (USAGE_STAGING, CPU_ACCESS_READ)
4. `CopyResource(staging, combined_atlas)`, `Map()` staging
5. If `flags & SBS`: `stbi_write_png("{prefix}_sbs.png", atlas_w, atlas_h, 4, mapped.pData, mapped.RowPitch)`
6. If `flags & LEFT`: extract left half → tightly-packed buffer → `{prefix}_L.png`
7. If `flags & RIGHT`: extract right half → tightly-packed buffer → `{prefix}_R.png`
8. Populate `out_result` with timestamp_ns, dims, eye poses (from the most recent tracker state — look at how the hit-test code accesses eye positions; same data), display_width_m/height_m
9. `Unmap()`, release staging

Implement the IPC handler (`ipc_server_handler.c`) that calls this function.

### 8.3 — L/R sub-image extraction

In the capture function, for each requested eye:

```cpp
uint32_t eye_w = atlas_w / sys->tile_columns;
uint32_t eye_h = atlas_h / sys->tile_rows;
uint32_t eye_x = eye_index * eye_w;  // 0 for left, eye_w for right
std::vector<uint8_t> buf(eye_w * eye_h * 4);
const uint8_t *src = (const uint8_t *)mapped.pData;
for (uint32_t y = 0; y < eye_h; y++) {
    const uint8_t *src_row = src + y * mapped.RowPitch + eye_x * 4;
    memcpy(buf.data() + y * eye_w * 4, src_row, eye_w * 4);
}
stbi_write_png(path, eye_w, eye_h, 4, buf.data(), eye_w * 4);
```

Make sure to handle the case `tile_columns == 1` (mono) — then LEFT == RIGHT == SBS (all the same image).

### 8.4 — Shell: hotkey + IPC + sidecar JSON

In `src/xrt/targets/shell/main.c`:

1. Define `HOTKEY_CAPTURE 3` next to `HOTKEY_TOGGLE`, `HOTKEY_LAUNCH`
2. `RegisterHotKey(g_msg_hwnd, HOTKEY_CAPTURE, MOD_CONTROL | MOD_SHIFT, '3')` alongside the others
3. Handle the message:
   ```c
   } else if (msg.message == WM_HOTKEY && msg.wParam == HOTKEY_CAPTURE) {
       capture_frame(ipc_c);
   }
   ```
4. Build the output path: `%USERPROFILE%\Pictures\DisplayXR\capture_YYYY-MM-DD_HH-MM-SS` (create dir if missing, use `GetLocalTime`)
5. Call `ipc_call_shell_capture_frame(ipc_c, prefix, IPC_CAPTURE_FLAG_ALL, &result)`
6. Write the sidecar JSON file at `{prefix}.json` with timestamp, dims, eye poses, views_written. Use cJSON (already linked into the shell target).

### 8.5 — Capture flash indicator (optional)

Set `sys->capture_flash_until_ns = now + 300ms` when capture runs. In the render path before Present, if `now < flash_until_ns`, draw a bright cyan border around the atlas edge using the glow shader. Skip if this slows down MVP; add as a follow-up.

### 8.6 — Route file-trigger through same code path

The Phase 7 file-trigger block should be refactored to call `comp_d3d11_service_capture_frame(xsysc, "%TEMP%\shell_screenshot", IPC_CAPTURE_FLAG_SBS, &dummy)` so there's one code path. Keep the trigger mechanism for dev workflows (useful when you need a quick atlas dump without shell IPC).

## Key code locations

- `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` — Phase 7 screenshot block is before the `swap_chain->Present()` call inside `multi_compositor_render()`
- `src/xrt/compositor/d3d11_service/comp_d3d11_service.h` — public API for the service
- `src/xrt/ipc/shared/ipc_protocol.h` — IPC struct definitions
- `src/xrt/ipc/shared/proto.json` — IPC call definitions (re-run build to regenerate bindings)
- `src/xrt/ipc/server/ipc_server_handler.c` — where to add the `shell_capture_frame` handler
- `src/xrt/targets/shell/main.c` — shell message loop, hotkey registration, IPC calls

## Commit style

- Commit per task (or small group)
- Reference #43 in every commit
- Use `Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>`

## Testing

Build locally:
```
scripts\build_windows.bat build
```

Important: after any IPC protocol change, copy BOTH binaries to the installed location or apps will fail OpenXR init:
```
powershell -Command "Copy-Item _package\bin\displayxr-service.exe 'C:\Program Files\DisplayXR\Runtime\displayxr-service.exe' -Force; Copy-Item _package\bin\DisplayXRClient.dll 'C:\Program Files\DisplayXR\Runtime\DisplayXRClient.dll' -Force"
```

If the DLL is locked, find the holder and kill it:
```
powershell -Command "Get-Process | Where-Object { $_.Modules.FileName -match 'DisplayXRClient' } | Select-Object Id, ProcessName"
```

Launch the shell:
```
_package\bin\displayxr-shell.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe
```

Press **Ctrl+Shift+3** → check `%USERPROFILE%\Pictures\DisplayXR\`:
- `capture_<ts>_L.png` — 1920×2160
- `capture_<ts>_R.png` — 1920×2160
- `capture_<ts>_sbs.png` — 3840×2160
- `capture_<ts>.json` — metadata

Verify parallax between L and R (the cube should appear horizontally shifted).

### Autonomous screenshot iteration

You can use the Phase 7 file-trigger mechanism to self-inspect the shell during development:
```bash
touch "/c/Users/SPARKS~1/AppData/Local/Temp/shell_screenshot_trigger"
sleep 3
# Read %TEMP%\shell_screenshot.png via the Read tool
```

## When to ask the user

- After completing each task — wait for live verification before committing
- Before adding the capture flash (design call: cyan border? full-screen flash? flicker?)
- If the IPC binding regeneration fails or produces unexpected output
- When you hit any GPU texture access issue (format mismatches are common with staging textures)
```
