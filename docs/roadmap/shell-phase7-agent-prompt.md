# Shell Phase 7: Agent Prompt — 3D Icon Rendering

Use this prompt to start a new Claude Code session for implementing Phase 7 on branch `feature/shell-phase7`.

---

## Prompt

```
I'm working on the DisplayXR shell — a spatial window manager for 3D lenticular displays. Phase 7 wires icon textures into the launcher tiles, including stereoscopic 3D icons.

## Context

Read these docs first (in order):
1. `CLAUDE.md` — project overview, build commands, architecture
2. `docs/roadmap/shell-phase7-plan.md` — **the plan you're implementing**
3. `docs/roadmap/shell-phase7-status.md` — task checklist (update as you complete tasks)
4. `docs/specs/displayxr-app-manifest.md` — sidecar spec (icon, icon_3d, icon_3d_layout fields)
5. `docs/roadmap/shell-phase6-status.md` — what Phase 6 delivered

## Branch

You are on `feature/shell-phase7`, branched from `main` after Phase 6 was merged. All work goes here. Commits should reference #43 (Spatial OS tracking issue).

## What already exists

The icon pipeline is partially built from Phase 5:

- **Icon loader** (`src/xrt/compositor/d3d11_service/d3d11_icon_loader.{h,cpp}`): `d3d11_icon_load_from_file(device, path, &srv, &w, &h)` decodes PNG/JPEG via stb_image into a D3D11 SRGB texture + SRV. Ready to use.

- **Sidecar fields**: `icon` (2D PNG), `icon_3d` (SBS stereoscopic PNG), `icon_3d_layout` (sbs-lr/sbs-rl/tb/bt). Parsed by the scanner, stored on `registered_app`, persisted in JSON.

- **IPC gap**: `struct ipc_launcher_app` only carries `name[96]`, `exe_path[256]`, `type[8]` = 360 bytes. No icon paths. `IPC_BUF_SIZE` is 512 bytes — adding icon paths would exceed it.

- **Render gap**: tile draw uses `convert_srgb=2.0` (solid color mode). Needs `convert_srgb=0.0` (textured) with the icon SRV bound.

## Tasks

### 7.1 — IPC: bump buffer + extend app struct

Bump `IPC_BUF_SIZE` from 512 to 1024 in `src/xrt/ipc/shared/ipc_protocol.h`. Add `icon_path[256]`, `icon_3d_path[256]`, `icon_3d_layout[8]` to `struct ipc_launcher_app`. Update `shell_push_registered_apps_to_service` in `main.c` to populate the new fields from `g_registered_apps[i]`.

### 7.2 — Service: icon texture storage + loading

On `d3d11_service_system`, add per-app icon state:
```cpp
struct launcher_icon {
    wil::com_ptr<ID3D11ShaderResourceView> srv_2d;   // from icon
    wil::com_ptr<ID3D11ShaderResourceView> srv_3d;   // from icon_3d
    uint32_t w_2d, h_2d, w_3d, h_3d;
    char layout_3d[8]; // "sbs-lr" etc
};
struct launcher_icon launcher_icons[IPC_LAUNCHER_MAX_APPS];
```

In `comp_d3d11_service_add_launcher_app`: if `app->icon_path[0]`, call `d3d11_icon_load_from_file(sys->device, app->icon_path, &icon.srv_2d, &icon.w_2d, &icon.h_2d)`. Same for `icon_3d_path` → `srv_3d`. In `clear_launcher_apps`: release all SRVs.

### 7.3 — Render: textured tiles

In the launcher tile render loop, when `sys->launcher_icons[full_idx].srv_2d` is non-null:
1. Bind the icon SRV via `PSSetShaderResources`.
2. Set `convert_srgb=0.0` (textured mode — the texture is `R8G8B8A8_UNORM_SRGB` so the GPU auto-linearizes on sample; the PS uses the linearized value directly).
3. Set `src_rect` to `(0, 0, icon_w, icon_h)` and `src_size` to `(icon_w, icon_h)`.
4. Keep `dst_offset/dst_rect_wh` same as the tile position.
5. Draw. The icon replaces the solid-color tile background.

Fallback: if no icon SRV, use the existing solid-color draw.

### 7.4 — SBS per-eye sampling

When `srv_3d` exists AND tile_columns > 1 (stereo mode):
1. Use `srv_3d` instead of `srv_2d`.
2. Compute `src_rect` based on layout + eye:
   - `sbs-lr`, left eye (col=0): `src_rect = (0, 0, w_3d/2, h_3d)`
   - `sbs-lr`, right eye (col=1): `src_rect = (w_3d/2, 0, w_3d/2, h_3d)`
   - `sbs-rl`: swap
   - `tb`, left eye: `src_rect = (0, 0, w_3d, h_3d/2)`
   - `tb`, right eye: `src_rect = (0, h_3d/2, w_3d, h_3d/2)`
   - `bt`: swap
3. Set `src_size = (w_3d, h_3d)`.
4. Each eye's tile view samples its own half of the icon texture.

When tile_columns=1 (mono/warmup mode): use `srv_2d` (or left-eye sub-rect of `srv_3d`).

### 7.5 — Test icons

Generate simple placeholder PNG icons for the 4 cube_handle apps. Options:
- Programmatically generate a solid-color square with a letter (D/G/V/O for D3D11/GL/Vulkan/OpenGL) via stb_image_write
- Or create a simple Python/PowerShell script that generates colored PNGs
- Place in `test_apps/cube_handle_*_win/displayxr/icon.png`
- Update sidecars to add `"icon": "icon.png"`

For the 3D demo: create `icon_sbs.png` (1024×512, left half = slightly left-shifted render, right half = slightly right-shifted) for at least one app. This demonstrates the 3D icon pipeline.

## Key code locations

- `src/xrt/compositor/d3d11_service/d3d11_icon_loader.h` — loader API
- `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` — launcher render block starting at "Phase 5.7 + 5.8: spatial launcher panel"
- `src/xrt/ipc/shared/ipc_protocol.h` — `IPC_BUF_SIZE`, `ipc_launcher_app`
- `src/xrt/targets/shell/main.c` — `shell_push_registered_apps_to_service`
- `src/xrt/compositor/d3d11_service/d3d11_service_shaders.h` — `BlitConstants.src_rect`, `convert_srgb`

## Commit style

- Commit per task (or small group)
- Reference #43 in every commit
- Use `Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>`

## Testing

Build locally: `scripts\build_windows.bat build`. Launch the shell with a cube:
```
_package\bin\displayxr-shell.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe
```
Ctrl+L → tiles should show icon textures. Verify 3D icons on the Leia display (each eye sees its half). Ask the user to eyeball the live display for visual correctness — screenshots are unreliable (per memory).

## When to ask the user

- After completing each task — wait for live verification before committing
- Before making icon design choices (color, style, size)
- When you hit any pipeline or shader issue
```
