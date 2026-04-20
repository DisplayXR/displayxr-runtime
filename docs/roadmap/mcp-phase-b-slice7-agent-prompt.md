# MCP Phase B Slice 7 — Windows Agent Prompt

You are running inside Claude Code on a **Windows machine with a Leia SR display**. Your job is to:

1. Validate that MCP Phase B slices 1–6 + 8 work end-to-end against a live `displayxr-service`.
2. Implement **slice 7** (`capture_frame` service-mode upgrade).
3. Commit and push.

This prompt is self-contained — everything you need is here or in linked docs. Do not wait for clarification; follow the steps in order.

## Context (2-minute read)

**Phase B** extends the MCP server (Phase A, #150) to shell/service mode. Seven slices are already merged on `feature/mcp-phase-b`:

| # | What | Test |
|---|---|---|
| 1 | Service-side MCP server (`u_mcp_server_maybe_start_named("service")`) + adapter `--target service` | `tests\mcp\test_service_handshake.bat` |
| 2 | `list_windows` | `tests\mcp\test_list_windows.bat` |
| 3 | `get/set_window_pose` | `tests\mcp\test_set_window_pose.bat` |
| 4 | `set_focus` + `apply_layout_preset` | `tests\mcp\test_focus_preset.bat` |
| 5 | `save/load_workspace` | `tests\mcp\test_workspace_roundtrip.bat` |
| 6 | Audit log + allowlist | `tests\mcp\test_audit_log.bat` |
| 8 | `list_sessions` (service variant) + PID routing | `tests\mcp\test_session_routing.bat` |

All were compile-checked on macOS + MinGW cross-compile. None have been run on Windows yet. Issue: **#157**. Plan: `docs/roadmap/mcp-phase-b-plan.md`.

Slice 7 is Windows-only because it touches the **D3D11 service compositor** (`comp_d3d11_service.cpp`) which uses WIL and is MSVC-only.

## Phase 1 — Validate slices 1–6, 8

### 1.1 Sync

```bat
cd %USERPROFILE%\Documents\GitHub\displayxr-runtime-pvt
git fetch origin
git checkout feature/mcp-phase-b
git pull
```

### 1.2 Build

```bat
scripts\build_windows.bat all
```

Expected: `_package\DisplayXR\` and `_package\bin\` populated; `test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe` exists. If the build fails on a Phase B file, inspect with `type build\CMakeFiles\CMakeError.log` and fix; the most likely breakage is MSVC-specific behaviour that MinGW missed (see `CLAUDE.md` → "MinGW is NOT MSVC" section).

### 1.3 Start the service + two apps, `DISPLAYXR_MCP=1`

From a cmd.exe window:

```bat
set DISPLAYXR_MCP=1
_package\bin\displayxr-shell.exe ^
    test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe ^
    test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe
```

Leave this running. Per `CLAUDE.md`, launch via `Bash run_in_background: true` with `timeout: 600000`.

### 1.4 Run the Phase B tests

Fresh cmd.exe window:

```bat
cd %USERPROFILE%\Documents\GitHub\displayxr-runtime-pvt
tests\mcp\test_service_handshake.bat
tests\mcp\test_list_windows.bat
tests\mcp\test_set_window_pose.bat
tests\mcp\test_focus_preset.bat
tests\mcp\test_workspace_roundtrip.bat
tests\mcp\test_audit_log.bat
tests\mcp\test_session_routing.bat
```

All seven must print `OK` on the final line. Any `FAIL` — **stop and investigate**. Capture full stderr, find the root cause in the relevant source (`src\xrt\ipc\server\ipc_mcp_tools.c`, `src\xrt\auxiliary\util\u_mcp_*`), fix, rebuild with `scripts\build_windows.bat build`, re-run. Do **not** paper over failures or skip tests.

Record the passing test output for the final commit message.

## Phase 2 — Implement slice 7: `capture_frame` service-mode upgrade

### Goal

When an MCP agent calls `capture_frame` against the service endpoint, the D3D11 service compositor:

1. Reads back the `combined_atlas` (full shell composition).
2. Writes three PNGs: `{base}_atlas.png`, `{base}_L.png`, `{base}_R.png`, where `{base}` comes from the request path.
3. Writes `{base}_windows.json` with a bbox entry per active shell window.
4. Signals the MCP thread via `u_mcp_capture_complete`.

Reference implementation to mirror: `src\xrt\compositor\d3d11\comp_d3d11_compositor.cpp` around line 881 (`d3d11_compositor_service_mcp_capture`). It uses the same `u_mcp_capture_poll / complete` contract and the same `CopySubresourceRegion` staging-texture pattern. **Copy the structure, adapt for the service atlas.**

### Files to touch

1. **`src/xrt/compositor/d3d11_service/comp_d3d11_service.h`** — add one forward-declared function.
2. **`src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp`** — (a) add a `u_mcp_capture_request` member to `d3d11_service_system`, (b) call `u_mcp_capture_install` at system create, (c) uninstall at destroy, (d) poll inside `multi_compositor_render` adjacent to the existing file-trigger block at line ~7929, (e) add the new capture service function.
3. **`src/xrt/ipc/server/ipc_mcp_tools.c`** — register a service-side `capture_frame` tool that invokes the `u_mcp_capture` handler (the handler is already set by the service compositor; the tool just dispatches).

No change to aux_util — `u_mcp_capture` already supports the cross-thread hand-off we need. No change to the adapter.

### Implementation sketch

**Step 1.** In `comp_d3d11_service.h`, add:

```c
/*!
 * Service a pending MCP capture_frame request against the combined
 * shell atlas. Writes {base}_atlas.png, {base}_L.png, {base}_R.png,
 * plus {base}_windows.json. Called from multi_compositor_render just
 * before Present so the atlas is fully populated.
 *
 * @ingroup comp_d3d11_service
 */
void
comp_d3d11_service_poll_mcp_capture(struct xrt_system_compositor *xsysc);
```

**Step 2.** In `comp_d3d11_service.cpp`:

- Add `#include "util/u_mcp_capture.h"` near the top with other aux_util includes.
- Add `struct u_mcp_capture_request mcp_capture;` to `struct d3d11_service_system` (the struct defined around line 345 — see `char layout_3d[8]` in that region).
- In the system create/init path, after the system is constructed, call `u_mcp_capture_init(&sys->mcp_capture)` and `u_mcp_capture_install(&sys->mcp_capture)`. Mirror what `comp_metal_compositor.m:1944` does.
- In the system destroy path, call `u_mcp_capture_uninstall()` then `u_mcp_capture_fini(&sys->mcp_capture)`.
- Implement `comp_d3d11_service_poll_mcp_capture` below the existing `comp_d3d11_service_set_launcher_visible` at the bottom of the file. Body:

```cpp
void
comp_d3d11_service_poll_mcp_capture(struct xrt_system_compositor *xsysc)
{
    if (xsysc == nullptr) return;
    struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
    if (!sys->shell_mode || sys->multi_comp == nullptr) return;

    char base[U_MCP_CAPTURE_PATH_MAX];
    if (!u_mcp_capture_poll(&sys->mcp_capture, base)) return;

    // The request path is a base *without* extension; e.g. the Phase A
    // handler hands us "C:\Temp\displayxr-mcp-capture-NNN-M". We append
    // our own suffixes so the caller gets the three-file bundle.

    struct d3d11_multi_compositor *mc = sys->multi_comp;
    if (mc->combined_atlas == nullptr) {
        u_mcp_capture_complete(&sys->mcp_capture, false);
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);

    D3D11_TEXTURE2D_DESC desc;
    mc->combined_atlas->GetDesc(&desc);
    uint32_t atlas_w = desc.Width, atlas_h = desc.Height;

    // Stage the full atlas once. SBS layout: left half = L eye, right half = R eye.
    D3D11_TEXTURE2D_DESC sd = desc;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    wil::com_ptr<ID3D11Texture2D> staging;
    if (FAILED(sys->device->CreateTexture2D(&sd, nullptr, staging.put()))) {
        u_mcp_capture_complete(&sys->mcp_capture, false);
        return;
    }
    sys->context->CopyResource(staging.get(), mc->combined_atlas.get());

    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(sys->context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m))) {
        u_mcp_capture_complete(&sys->mcp_capture, false);
        return;
    }

    // File 1: atlas.png (whole thing).
    char path[U_MCP_CAPTURE_PATH_MAX + 32];
    snprintf(path, sizeof(path), "%s_atlas.png", base);
    bool ok_atlas = stbi_write_png(path, (int)atlas_w, (int)atlas_h, 4,
                                   m.pData, (int)m.RowPitch) != 0;

    // Files 2 and 3: left and right halves. SBS atlas convention —
    // left view is bytes [0..half_w) of each row, right view is
    // [half_w..atlas_w). stbi_write_png accepts an explicit stride so
    // we reuse the mapped pointer at an offset.
    uint32_t half_w = atlas_w / 2;
    const uint8_t *rows = (const uint8_t *)m.pData;
    snprintf(path, sizeof(path), "%s_L.png", base);
    bool ok_L = stbi_write_png(path, (int)half_w, (int)atlas_h, 4,
                               rows, (int)m.RowPitch) != 0;
    snprintf(path, sizeof(path), "%s_R.png", base);
    bool ok_R = stbi_write_png(path, (int)half_w, (int)atlas_h, 4,
                               rows + half_w * 4, (int)m.RowPitch) != 0;

    sys->context->Unmap(staging.get(), 0);

    // File 4: windows.json — one entry per active client slot.
    snprintf(path, sizeof(path), "%s_windows.json", base);
    FILE *jf = fopen(path, "wb");
    bool ok_json = false;
    if (jf != nullptr) {
        fprintf(jf, "{\n  \"atlas_width\": %u,\n  \"atlas_height\": %u,\n  \"windows\": [",
                atlas_w, atlas_h);
        bool first = true;
        for (int i = 0; i < D3D11_MULTI_MAX_CLIENTS; i++) {
            const d3d11_multi_client_slot *s = &mc->clients[i];
            if (!s->active) continue;
            fprintf(jf, "%s\n    {\"slot\": %d, \"name\": \"%s\", "
                        "\"atlas_bbox\": {\"x\": %d, \"y\": %d, \"w\": %d, \"h\": %d}, "
                        "\"content\": {\"w\": %u, \"h\": %u}}",
                    first ? "" : ",", i, s->app_name,
                    s->window_rect_x, s->window_rect_y,
                    s->window_rect_w, s->window_rect_h,
                    s->content_view_w, s->content_view_h);
            first = false;
        }
        fprintf(jf, "\n  ]\n}\n");
        fclose(jf);
        ok_json = true;
    }

    u_mcp_capture_complete(&sys->mcp_capture, ok_atlas && ok_L && ok_R && ok_json);
}
```

- In `multi_compositor_render`, right after the existing file-trigger block (around line 7929, just before `Present`), call:

```cpp
comp_d3d11_service_poll_mcp_capture((struct xrt_system_compositor *)sys);
```

**Step 3.** In `src/xrt/ipc/server/ipc_mcp_tools.c`, add a service-side `capture_frame` tool:

- Add `#include "util/u_mcp_capture.h"` and forward-declare the capture-handler contract that aux_util already uses: `extern void oxr_mcp_tools_set_capture_handler(...)`. Actually — **don't** redeclare it; `u_mcp_capture_install` already calls it internally. So the ipc_mcp_tools side just needs to register a tool that invokes the registered capture handler. Inspect `src/xrt/state_trackers/oxr/oxr_mcp_tools.c` (around the existing `capture_frame` tool) for the exact API — the handler is registered via `oxr_mcp_tools_set_capture_handler(fn, userdata)` and invoked via a helper inside the tool body. Copy the Phase A tool's structure verbatim but register it under the ipc_mcp_tools namespace so both the per-PID handle-app server and the service server respond to `capture_frame`.
- If re-registration of an already-registered tool name is rejected by `u_mcp_server_register_tool`, let that be the existing Phase A tool registered from `oxr_mcp_tools`. The service instance of the server registers the Phase B tool first (because `ipc_mcp_tools_register` runs before any `oxr_instance_create` happens in a service process — service is not a state-tracker client). Verify behaviour empirically; if you need to guard, check `oxr_mcp_tools_register_all` is not called inside `displayxr-service.exe`.

### Test for slice 7

Create `tests/mcp/test_capture_frame_service.bat` + `tests/mcp/_capture_frame_service_helper.py`:

```python
# Helper outline — adapt from _list_windows_helper.py
# 1. initialize against --target service
# 2. tools/call capture_frame with arguments {} (no path; Phase A default)
# 3. parse result for file paths; assert _atlas.png, _L.png, _R.png, _windows.json exist
# 4. assert _L.png and _R.png dimensions are each half the atlas width
#    (use PIL or stbi_read to verify — PIL is available in Windows Python)
# 5. load _windows.json, assert it's valid JSON with >= len(list_windows) entries
```

Follow the pattern of `tests/mcp/test_list_windows.bat` exactly — `findstr` fallback if `python` is missing, `ADAPTER` discovery, `--target service` probe before invoking the helper.

Run the test with the service + two apps still running from Phase 1.

### Non-goals for slice 7

- **Do not** replace the file-trigger block at `comp_d3d11_service.cpp:7929`. Keep it as a separate debugging affordance.
- **Do not** modify `u_mcp_capture.{h,c}`. Its contract already supports this flow.
- **Do not** add recording / temporal capture — that's Phase B.5 blocked on `docs/roadmap/3d-capture.md`.
- **Do not** change `proto.json` or any IPC message definitions.

## Phase 3 — Commit + push

Follow the commit convention used on slices 1–8. Example:

```
MCP Phase B slice 7: capture_frame service-mode upgrade (#157)

Extends the D3D11 service compositor with an MCP-triggered capture
path that writes a four-file bundle per request:

- <base>_atlas.png  — full combined shell atlas
- <base>_L.png      — left half of the atlas (left eye)
- <base>_R.png      — right half of the atlas (right eye)
- <base>_windows.json — per-active-slot atlas bbox + content dims

Uses the same u_mcp_capture_{install,poll,complete} cross-thread
contract Phase A wired up for the handle-app D3D11, Metal, and GL
compositors. Atlas readback is a single staging-texture CopyResource
+ Map; L/R split reuses the mapped pointer at a half-width offset
so we only pay one GPU→CPU copy per frame.

- compositor/d3d11_service/comp_d3d11_service.{h,cpp}: add
  u_mcp_capture_request member, install/uninstall in system
  create/destroy, poll in multi_compositor_render just before
  Present. The existing file-trigger block at line ~7929 is
  preserved as a separate interactive debugging path.
- ipc/server/ipc_mcp_tools.c: register service-side capture_frame
  tool that invokes the installed handler.
- tests/mcp: add test_capture_frame_service.{sh,bat} + helper.
  Requires DISPLAYXR_MCP=1 shell + apps running; asserts the four
  files exist, L/R are each half the atlas width, and windows.json
  lists at least as many entries as list_windows reports.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
```

Then:

```bat
git add -A
git commit -F -   :: paste the message via HEREDOC equivalent
git push
```

### Final report to the user

After push, summarize in one paragraph:

- Slice 7 implementation status (committed SHA, lines changed).
- Phase B validation status — which of the 8 tests passed on Windows, any that regressed during slice 7 work and how you fixed them.
- Whether the David "3pm review" story (the Phase B gate from `docs/roadmap/mcp-spec-v0.2.md` §7) is reproducible end-to-end: call `save_workspace`, perturb poses, call `load_workspace`, call `capture_frame`, inspect the captured atlas.

Keep it under 200 words. No emojis.

## Reference commands you'll need

| Purpose | Command |
|---|---|
| Rebuild runtime only | `scripts\build_windows.bat build` |
| Rebuild test apps | `scripts\build_windows.bat test-apps` |
| Capture a compositor screenshot (non-MCP) | `touch "%TEMP%\shell_screenshot_trigger"` then read `%TEMP%\shell_screenshot.png` |
| Crash dump | see `CLAUDE.md` → "Debugging Crashes on Windows" |

## If the Phase 1 validation exposes a bug in slices 1–6 or 8

Fix it on the same branch as a prior slice's follow-up commit (message: `MCP Phase B slice N follow-up: <what> (#157)`). Do **not** squash or force-push.
