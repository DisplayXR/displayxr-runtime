# Shell Phase 8 Plan: 3D Capture MVP

**Branch:** `feature/shell-phase8`
**Issue:** #43, #44 (tracking), new capture issue TBD
**Date:** 2026-04-13

## Goal

Deliver the MVP of the 3D capture pipeline: a user-triggered spatial screenshot that saves pre-weave L/R stereo images plus a metadata sidecar. Builds on the Phase 7 file-trigger screenshot by promoting it to an IPC-driven, shell-owned feature with proper L/R separation and metadata.

**User experience:** Press `Ctrl+Shift+3` in the spatial shell → a cyan flash on screen → a new `capture_<timestamp>.png` pair appears in `%USERPROFILE%\Pictures\DisplayXR\`.

**Full spec:** [3d-capture.md](3d-capture.md) — this phase delivers MVP (frame capture only; recording and dataset modes deferred).

## Architecture

Ownership split matches `3d-capture.md`:

| Runtime (service compositor) owns | Shell owns |
|-----------------------------------|------------|
| GPU texture read-back | Hotkey binding (`Ctrl+Shift+3`) |
| Capture point (post-composite, pre-weave) | Output path construction (timestamp, directory policy) |
| PNG encoding (stb_image_write) | Sidecar JSON metadata file |
| Metadata collection (eye pose, timestamps) | Toast / notification UX (optional) |
| L/R split from combined atlas | Privacy policy / opt-out (future) |

**IPC flow:**
```
Shell (Ctrl+Shift+3)
  └─ build path: %USERPROFILE%\Pictures\DisplayXR\capture_2026-04-13_14-52-10
  └─ ipc_call_shell_capture_frame(path_prefix, flags=SBS|L|R|ALL)
     └─ Service: copy combined_atlas → staging → map → stbi_write_png for each requested view
     └─ Return: struct ipc_capture_result { timestamp_ns, width, height, views_written }
  └─ Shell: write path_prefix.json with metadata
  └─ Shell: optional flash indicator
```

## Capture Point (unchanged from Phase 7)

The existing screenshot code in `comp_d3d11_service.cpp`, `multi_compositor_render()` just before `swap_chain->Present()`, is already the correct capture point:
- Post-composite (all windows composed)
- Pre-weave (L/R still separable; not display-specific)
- GPU-resident (`combined_atlas` texture)

Phase 8 refactors this to be IPC-triggered with L/R output instead of a monolithic file-trigger SBS dump.

## Tasks

### 8.1 — IPC protocol

**Files:**
- `src/xrt/ipc/shared/ipc_protocol.h` — add flags + result struct
- `src/xrt/ipc/shared/proto.json` — new call
- `src/xrt/ipc/server/ipc_server_handler.c` — handler stub

**Changes:**

Add to `ipc_protocol.h`:
```c
// Phase 8: 3D capture flags
#define IPC_CAPTURE_FLAG_LEFT   (1u << 0)
#define IPC_CAPTURE_FLAG_RIGHT  (1u << 1)
#define IPC_CAPTURE_FLAG_SBS    (1u << 2)
#define IPC_CAPTURE_FLAG_ALL    (IPC_CAPTURE_FLAG_LEFT | IPC_CAPTURE_FLAG_RIGHT | IPC_CAPTURE_FLAG_SBS)

struct ipc_capture_result
{
    uint64_t timestamp_ns;      // CLOCK_MONOTONIC-equivalent at capture
    uint32_t atlas_width;       // full atlas (SBS) dims
    uint32_t atlas_height;
    uint32_t eye_width;         // per-eye dims (atlas_width / tile_columns)
    uint32_t eye_height;
    uint32_t views_written;     // bitmask of which flags succeeded
    uint32_t tile_columns;      // stereo mode at capture (1 or 2)
    uint32_t tile_rows;
    float display_width_m;
    float display_height_m;
    float eye_left_m[3];        // left eye pose in meters
    float eye_right_m[3];
    char _pad[16];
};
```

Add to `proto.json`:
```json
{
    "name": "shell_capture_frame",
    "in": [
        {"name": "path_prefix", "type": "char[256]"},
        {"name": "flags", "type": "uint32_t"}
    ],
    "out": [
        {"name": "result", "type": "struct ipc_capture_result"}
    ]
}
```

The `path_prefix` is the full base path *without* extension — e.g. `C:\Users\...\Pictures\DisplayXR\capture_2026-04-13_14-52-10`. The runtime appends `_L.png`, `_R.png`, `_sbs.png` depending on flags. This keeps filename policy in the shell.

### 8.2 — Service: refactor screenshot into capture_frame_impl

**Files:**
- `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp`
- `src/xrt/compositor/d3d11_service/comp_d3d11_service.h`

**Changes:**

Extract the Phase 7 screenshot block into a reusable function:

```cpp
bool
comp_d3d11_service_capture_frame(struct xrt_system_compositor *xsysc,
                                 const char *path_prefix,
                                 uint32_t flags,
                                 struct ipc_capture_result *out_result);
```

Implementation steps:
1. Lock `render_mutex`
2. Validate `combined_atlas` exists
3. Create staging texture matching atlas format + dims, `USAGE_STAGING`, `CPU_ACCESS_READ`
4. `CopyResource(staging, combined_atlas)`
5. `Map()` staging, read RGBA pixels
6. If `flags & SBS`: write full atlas to `{prefix}_sbs.png`
7. If `flags & LEFT`: extract left tile `(0, 0, eye_w, eye_h)` into a new buffer (stride handling!), write to `{prefix}_L.png`
8. If `flags & RIGHT`: extract right tile `(eye_w, 0, eye_w, eye_h)`, write to `{prefix}_R.png`
9. Collect metadata:
   - `timestamp_ns` from `os_monotonic_get_ns()`
   - atlas dims from `combined_atlas->GetDesc()`
   - eye dims = atlas_w / `sys->tile_columns`, atlas_h / `sys->tile_rows`
   - eye poses from the most recent `eye_pos` state on the active compositor (see Phase 6 code that populates `data.eyes[]`)
   - display dims from `sys->base.info.display_*_m`
10. `Unmap()`, return

Keep the Phase 7 file-trigger path as a backward-compat fallback (small cost, useful for dev workflows without IPC): if `shell_screenshot_trigger` exists, call `capture_frame(SBS, default_path)`.

### 8.3 — Left/right sub-image extraction

**File:** `comp_d3d11_service.cpp` in the capture function

For L/R split, the mapped staging texture gives us the full SBS atlas with a row stride (`mapped.RowPitch`). Each eye half is `eye_w = atlas_w / tile_columns` pixels wide starting at `x = eye_index * eye_w`.

Approach: allocate a tightly-packed RGBA8 buffer for each eye (`eye_w * eye_h * 4` bytes), copy rows from the mapped staging memory:

```cpp
std::vector<uint8_t> buf(eye_w * eye_h * 4);
const uint8_t *src = (const uint8_t *)mapped.pData;
for (uint32_t y = 0; y < eye_h; y++) {
    const uint8_t *src_row = src + y * mapped.RowPitch + eye_x_offset * 4;
    uint8_t *dst_row = buf.data() + y * eye_w * 4;
    memcpy(dst_row, src_row, eye_w * 4);
}
stbi_write_png(path, eye_w, eye_h, 4, buf.data(), eye_w * 4);
```

For SBS write, `stbi_write_png` handles row stride via the last arg, so no copy needed.

### 8.4 — Shell: hotkey + IPC call + filename policy

**File:** `src/xrt/targets/shell/main.c`

**Changes:**

1. Register a new hotkey:
```c
#define HOTKEY_CAPTURE 3
RegisterHotKey(g_msg_hwnd, HOTKEY_CAPTURE, MOD_CONTROL | MOD_SHIFT, '3');
```

2. In the message loop, handle it:
```c
} else if (msg.message == WM_HOTKEY && msg.wParam == HOTKEY_CAPTURE) {
    capture_frame(ipc_c);
}
```

3. New `capture_frame()` function:
```c
static void capture_frame(struct ipc_connection *ipc_c) {
    // Build path: %USERPROFILE%\Pictures\DisplayXR\capture_YYYY-MM-DD_HH-MM-SS
    char dir[MAX_PATH], prefix[MAX_PATH];
    const char *home = getenv("USERPROFILE");
    snprintf(dir, sizeof(dir), "%s\\Pictures\\DisplayXR", home ? home : ".");
    CreateDirectoryA(dir, NULL); // ignore error if exists

    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(prefix, sizeof(prefix),
             "%s\\capture_%04d-%02d-%02d_%02d-%02d-%02d",
             dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    struct ipc_capture_result result = {};
    xrt_result_t r = ipc_call_shell_capture_frame(ipc_c, prefix,
                                                  IPC_CAPTURE_FLAG_ALL, &result);
    if (r != XRT_SUCCESS) {
        PE("capture_frame: IPC call failed: %d\n", r);
        return;
    }

    // Write sidecar JSON
    char json_path[MAX_PATH];
    snprintf(json_path, sizeof(json_path), "%s.json", prefix);
    write_capture_sidecar(json_path, &result);

    P("Capture saved: %s\n", prefix);
}
```

4. `write_capture_sidecar()` writes minimal JSON using cJSON (already linked):
```json
{
    "schema_version": 1,
    "timestamp_ns": 12345678901234,
    "atlas": {"width": 3840, "height": 2160},
    "eye": {"width": 1920, "height": 2160},
    "stereo": {"tile_columns": 2, "tile_rows": 1},
    "display_m": {"width": 0.700, "height": 0.394},
    "eye_left_m": [-0.032, 0.0, 0.6],
    "eye_right_m": [0.032, 0.0, 0.6],
    "views_written": ["sbs", "L", "R"]
}
```

### 8.5 — Capture indicator (optional but nice)

**File:** `comp_d3d11_service.cpp`

A brief cyan flash on the atlas edges provides visual feedback. Approach: when `capture_frame_impl` runs, set a timestamp `sys->capture_flash_until_ns = now + 300ms`. In the render path, if current time < flash_until, draw a bright cyan border using the glow shader around the atlas.

Can defer to 8.6 or a follow-up; MVP works without it.

### 8.6 — Deprecate file-trigger screenshot

**File:** `comp_d3d11_service.cpp`

Replace the Phase 7 `shell_screenshot_trigger` file check with a call to `capture_frame_impl(sys, temp_path, IPC_CAPTURE_FLAG_SBS, &dummy)`. Keeps the dev workflow working (touch a file → get a PNG) but routes through the same code path as the IPC call.

## Verification

1. Build: `scripts\build_windows.bat build`
2. Deploy: copy binaries to `C:\Program Files\DisplayXR\Runtime\`
3. Launch: `_package\bin\displayxr-shell.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe`
4. Activate shell (Ctrl+Space if needed), press **Ctrl+Shift+3**
5. Check `%USERPROFILE%\Pictures\DisplayXR\` for new files:
   - `capture_<ts>_L.png` — 1920×2160 left-eye view
   - `capture_<ts>_R.png` — 1920×2160 right-eye view
   - `capture_<ts>_sbs.png` — 3840×2160 SBS
   - `capture_<ts>.json` — metadata sidecar
6. Open L + R in an image diff tool — verify visible parallax on the cube (depth cue)
7. Verify file-trigger fallback still works: `touch %TEMP%\shell_screenshot_trigger` → `%TEMP%\shell_screenshot.png` appears

## Commit plan

- **Commit 1** — Task 8.1 (IPC protocol) — references #43
- **Commit 2** — Task 8.2 + 8.3 (service capture impl + L/R split) — references #43
- **Commit 3** — Task 8.4 (shell hotkey + IPC call + sidecar) — references #43
- **Commit 4** — Task 8.5 (capture flash) — optional, references #43
- **Commit 5** — Task 8.6 (refactor file-trigger to use same code path) — references #43

Wait for user live verification after each commit before proceeding.

## Scope notes / non-goals

- **No recording** — still-frame only. Video capture is Phase 2 of 3d-capture.md.
- **No session capture** — Phase 3.
- **No dataset mode** — Phase 4.
- **No depth capture** — requires DP vtable changes.
- **No privacy / consent UI** — future work; capture fires silently for the MVP.
- **No cross-platform** — Windows D3D11 only. Metal/macOS capture follows in a later phase.
