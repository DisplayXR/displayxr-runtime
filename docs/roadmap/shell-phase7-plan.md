# Shell Phase 7 Plan: 3D Icon Rendering

**Branch:** `feature/shell-phase7`
**Tracking issue:** #43 (Spatial OS) / #44 (3D Shell)
**Depends on:** Phase 6 complete (merged to main)

## Overview

Phase 5 built the icon loader (`d3d11_icon_load_from_file` in `d3d11_icon_loader.cpp`) and the sidecar spec supports `icon` (2D) and `icon_3d` (stereoscopic SBS) fields, but the render still draws solid-color tile backgrounds. Phase 7 wires icon textures end-to-end:

1. Ship icon file paths from shell → service via IPC
2. Service loads icons into D3D11 textures on launcher open
3. Render samples icon textures on tiles instead of solid color
4. For `icon_3d`: per-eye UV sub-rect sampling (left half for left eye, right half for right eye) so tile icons appear in 3D on the lenticular display

## Infrastructure already in place

| Component | Status | Location |
|---|---|---|
| Icon loader (PNG/JPEG → SRV) | ✅ Built | `d3d11_icon_loader.{h,cpp}` |
| Sidecar spec (`icon`, `icon_3d`, `icon_3d_layout`) | ✅ Spec'd | `docs/specs/displayxr-app-manifest.md` |
| Scanner resolves icon paths | ✅ Working | `shell_app_scan.c` |
| Registered app struct has icon fields | ✅ Fields exist | `main.c: registered_app.icon_path/icon_3d_path/icon_3d_layout` |
| JSON persistence of icon paths | ✅ Saves/loads | `registered_apps_save/load` |
| BlitConstants shader supports textured mode | ✅ `convert_srgb=0.0` | `d3d11_service_shaders.h` |

## What's missing

1. **IPC doesn't carry icon paths.** `ipc_launcher_app` only has `name[96]`, `exe_path[256]`, `type[8]` = 360 bytes. Adding icon paths would exceed `IPC_BUF_SIZE=512`. Fix: bump `IPC_BUF_SIZE` to 1024 and extend `ipc_launcher_app` with icon fields.

2. **Service has no icon textures.** Need per-app `ID3D11ShaderResourceView*` storage on `d3d11_service_system`, loaded via `d3d11_icon_load_from_file` when the launcher opens or when the app list is pushed.

3. **Render doesn't sample textures on tiles.** Currently uses `convert_srgb=2.0` (solid color). Needs `convert_srgb=0.0` (textured) with the icon SRV bound when an icon is available, falling back to solid color when not.

4. **No SBS-aware per-eye sampling.** For `icon_3d` with `sbs-lr` layout, the left-eye tile should sample UV `[0, 0.5]` in X, right-eye `[0.5, 1.0]`. The existing BlitConstants `src_rect` field can select the sub-rect per draw call.

5. **No test icon PNGs.** The 4 cube_handle sidecars have no `icon` field. Need placeholder PNGs or programmatically generated test icons.

## Tasks

| Task | Description |
|------|-------------|
| 7.1 | Bump `IPC_BUF_SIZE` to 1024. Add `icon_path[256]`, `icon_3d_path[256]`, `icon_3d_layout[8]` to `ipc_launcher_app`. Update `shell_push_registered_apps_to_service` to populate them. |
| 7.2 | Add per-app icon SRV storage on `d3d11_service_system`. Load icons via `d3d11_icon_load_from_file` when the app list is pushed (in `comp_d3d11_service_add_launcher_app`). Destroy SRVs on `clear_launcher_apps`. |
| 7.3 | Update launcher tile render: when icon SRV exists, bind it and draw with `convert_srgb=0.0` (textured) instead of solid color. Fallback to solid color when no icon. |
| 7.4 | SBS-aware per-eye sampling for `icon_3d`: compute `src_rect` UV sub-rect based on `icon_3d_layout` and the current eye (left/right tile column). Only applies when `icon_3d` SRV exists. |
| 7.5 | Generate placeholder test icons for the 4 cube_handle apps. Simple colored squares with a letter/shape so the pipeline is visually testable. Update sidecars with `icon` field. |

## Critical files

| File | Changes |
|---|---|
| `src/xrt/ipc/shared/ipc_protocol.h` | Bump `IPC_BUF_SIZE` to 1024. Extend `ipc_launcher_app`. |
| `src/xrt/ipc/server/ipc_server_per_client_thread.c` | Uses `IPC_BUF_SIZE` for stack buffers — automatic with bump. |
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | Icon texture storage, load on push, render with icon SRV, per-eye UV for SBS. |
| `src/xrt/targets/shell/main.c` | Populate icon_path fields in `shell_push_registered_apps_to_service`. |
| `test_apps/cube_handle_*_win/displayxr/*.displayxr.json` | Add `icon` field pointing to placeholder PNGs. |
| `test_apps/cube_handle_*_win/displayxr/icon.png` | New placeholder icon files. |

## Verification

1. Build + launch shell with cube. Ctrl+L → tiles show icon images instead of solid color.
2. Tiles without icons (e.g. user-added via Browse) still render solid color fallback.
3. If `icon_3d` is present: tile appears stereoscopic on the 3D display (left eye sees left half, right eye sees right half). Convergence is comfortable at ZDP.
4. Icon textures survive launcher close/reopen without reloading (cached on sys).
5. `Ctrl+R` refresh reloads icons if sidecars changed.

## SBS icon rendering detail

For a tile with `icon_3d` in `sbs-lr` layout:
- The texture is `W×H` where `W = 2 * eye_width`.
- Left eye: `src_rect = (0, 0, W/2, H)` → samples left half.
- Right eye: `src_rect = (W/2, 0, W/2, H)` → samples right half.
- The render loop already iterates per-view (left tile column 0, right tile column 1). Use the column index to select the sub-rect.
- `sbs-rl`: swap left/right.
- `tb`: top half for left eye, bottom half for right.
- `bt`: swap.
- When tile_columns=1 (mono/2D mode): use left eye sub-rect (or full texture if no `icon_3d`).
