# DisplayXR Demo — Gaussian Splatting

Real-time 3D Gaussian Splatting viewer for glasses-free 3D displays, built on the DisplayXR runtime via OpenXR with Vulkan. Loads `.spz` and `.ply` files, renders with asymmetric per-eye Kooima projection for the full stereo/multiview experience.

> Auto-synced from [`DisplayXR/displayxr-runtime-pvt`](https://github.com/DisplayXR/displayxr-runtime-pvt) on every tagged release. Do not edit directly — open issues on the source repo.

## Download

Prebuilt binaries are attached to every [release](https://github.com/DisplayXR/displayxr-demo-gaussiansplat/releases):

- **Windows** — `gaussian_splatting_handle_vk_win-v<version>.zip` (unzip, run the exe next to its bundled DLLs)
- **macOS** — coming soon (build from source for now; macOS CI is re-enabled when MoltenVK support stabilizes)

A test scene, `butterfly.spz`, is bundled and auto-loads at startup.

## Controls

| Input | Action |
|---|---|
| WASD / Q / E | Strafe the virtual display in 3D |
| Left-click drag | Rotate the virtual display |
| Scroll / trackpad | Zoom (virtual display height) |
| Double-click | Focus on the splat under the cursor (smooth pose transition) |
| `-` / `=` | Decrease / increase depth + IPD together (10 %–100 %) |
| `M` | Auto-orbit: slow turntable rotation when idle for > 10 s |
| `F` | Flip scene Y-axis (fix for splats trained in the opposite Y convention) |
| `V` | Cycle rendering modes advertised by the display runtime |
| `L` or top-bar **Open…** | Load a different `.ply` / `.spz` file |
| Drag-and-drop (macOS) | Load a `.ply` / `.spz` dropped onto the window |
| Space | Reset pose, zoom, depth, auto-orbit, flip |
| Tab | Toggle HUD |
| Esc | Quit |

## Build from source

### Prerequisites (both platforms)
- CMake ≥ 3.21 + Ninja
- [Vulkan SDK](https://www.lunarg.com/vulkan-sdk/) (includes `glslangValidator`)
- [OpenXR loader](https://github.com/KhronosGroup/OpenXR-SDK) (find_package-visible)
- A DisplayXR-compatible runtime (install from [displayxr-shell-releases](https://github.com/DisplayXR/displayxr-shell-releases) for the real 3D display path; the demo also runs against the null compositor with `SIM_DISPLAY_OUTPUT=anaglyph`)

### macOS
```bash
brew install cmake ninja vulkan-sdk openxr-loader
./scripts/build_macos.sh
# Run
./build/macos/gaussian_splatting_handle_vk_macos
```

### Windows
```bat
REM Set OpenXR_ROOT to your OpenXR SDK install if find_package can't see it.
scripts\build_windows.bat
REM Run
build\windows\Release\gaussian_splatting_handle_vk_win.exe
```

## Repo layout

```
.
├── macos/                  Platform-specific entry + window handling
├── windows/                Platform-specific entry + window handling
├── 3dgs_common/            Vulkan compute pipeline, PLY + SPZ loaders
├── common/                 Shared helpers: Kooima math, input, HUD
├── openxr_includes/        Vendored OpenXR headers (incl. DisplayXR extensions)
└── scripts/                Build scripts for each platform
```

The `common/` and `openxr_includes/` directories are also used by other DisplayXR demos — content is synchronized from the DisplayXR runtime source tree on each release tag.

## License

BSL-1.0 — see `LICENSE`.
