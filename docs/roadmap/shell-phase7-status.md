# Shell Phase 7 Status: 3D Icon Rendering

**Branch:** `feature/shell-phase7`
**Status:** Complete — all tasks implemented and verified on Leia display
**Date:** 2026-04-13

## Scope

Wire icon textures end-to-end: IPC icon path plumbing → D3D11 texture loading → textured tile rendering → SBS per-eye sampling for 3D icons.

**Full plan:** [shell-phase7-plan.md](shell-phase7-plan.md)

## Tasks

| Status | Task | Description |
|--------|------|-------------|
| [x] | 7.1 | Bump IPC_BUF_SIZE, extend ipc_launcher_app with icon fields |
| [x] | 7.2 | Per-app icon SRV storage + load on push |
| [x] | 7.3 | Textured tile render (fallback to solid color when no icon) |
| [x] | 7.4 | SBS per-eye UV sub-rect for icon_3d |
| [x] | 7.5 | Placeholder test icons for cube_handle apps |

## Additional work (beyond original 5 tasks)

- Physically square tiles via pixel-aspect correction for SBS displays
- Icons fill tiles completely (no padding)
- Label font reduced 50% for better proportion
- Selection glow replaces white outline, follows keyboard selection
- Glow shader: separate X/Y extents for non-square pixel layouts, inset by corner radius
- All-four-corner rounding mode (radius<0, aspect<0) in shader
- Compositor file-trigger screenshot (`%TEMP%\shell_screenshot_trigger` → PNG)
- `stb_image_write` integration for back-buffer read-back

## Commits

- `c71efcd62` Shell 7: icon textures on launcher tiles + compositor screenshot
- `871926573` Shell 7: fix square tile aspect — use pixel-space equality
- `9b2a7c5b2` Shell 7: physically square tiles, label sizing, selection glow fixes

## Design Decisions

- Tile aspect uses physical pixel ratio (m_per_px_x / m_per_px_y) so tiles appear square on the Leia display despite SBS halving horizontal pixel density
- Icons drawn at full tile size (no aspect-fit) since both are physically square
- Selection glow only (no static running-tile glow) — cleaner visual
- Compositor screenshot via file trigger rather than IPC — simpler, works from any process
