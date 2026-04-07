# DisplayXR Shell

Spatial shell and 3D window manager for [DisplayXR](https://github.com/DisplayXR/displayxr-runtime). Turns a tracked 3D display into a multi-app spatial desktop where windows float in 3D space with correct stereo rendering.

## Status

**Pre-migration.** Shell source currently lives in the runtime repo (`src/xrt/targets/shell/`). Migration to a separate repo is tracked by [displayxr-runtime#4](https://github.com/DisplayXR/displayxr-runtime/issues/4), pending SDK export ([displayxr-runtime#3](https://github.com/DisplayXR/displayxr-runtime/issues/3)).

### Implemented (in runtime repo)

- **Phase 0-3 complete** — multi-compositor, spatial windowing, window chrome, layout presets, Z-depth, rotation, 3D layouts (Theater, Stack, Carousel)
- **Phase 3B complete** — cross-API support (GL, Vulkan, D3D12 apps in shell)
- **Phase 4 planned** — Spatial Companion: hotkey-summoned overlay, 2D window capture, focus-adaptive rendering, graceful exit

### What This Directory Will Become (Post-Migration)

After SDK export and repo split, the shell will live in its own repo:

```
displayxr-shell/
├── src/
│   ├── main.c              — app lifecycle, client monitoring, persistence
│   ├── window_adoption.c   — EnumWindows, 2D/3D classification, state snapshot
│   └── launcher.c          — registered apps, launcher panel, app type detection
├── config/
│   └── registered_apps.json
├── CMakeLists.txt           — find_package(DisplayXRSDK), links ipc_client
└── README.md
```

## Architecture

The shell is a pure IPC client — it communicates with the runtime exclusively through the IPC protocol. Zero compositor dependencies.

```
DisplayXR Shell (policy)
  │
  │  3 IPC calls: shell_activate, shell_set_window_pose, shell_get_window_pose
  │  + capture: shell_add_capture_client, shell_remove_capture_client
  │
  ▼
DisplayXR Runtime (mechanism)
  ├── Multi-compositor: renders all windows as spatial quads
  ├── Capture: Windows.Graphics.Capture for 2D app windows
  ├── Window chrome: title bars, buttons, hit-testing
  └── Display processor: weaver integration
```

**Boundary rule:** Shell decides *what* (window placement, layout, focus policy). Runtime decides *how* (rendering, capture, projection, weaving).

## Build

Currently built as part of the runtime CMake tree:

```bash
# Built automatically with the runtime
cmake --build build --config Release
# Binary output: _package/bin/displayxr-shell.exe
```

Post-migration, will require the DisplayXR Runtime SDK:

```bash
set DISPLAYXR_SDK_PATH=path/to/_package
mkdir build && cd build
cmake .. -G Ninja
cmake --build . --config Release
```

## Binary Releases

Published to [displayxr-shell-releases](https://github.com/DisplayXR/displayxr-shell-releases) (public, binary-only).

## Related

- [displayxr-runtime](https://github.com/DisplayXR/displayxr-runtime) — Open source OpenXR runtime for 3D displays
- [displayxr-shell-releases](https://github.com/DisplayXR/displayxr-shell-releases) — Binary releases and issue tracking

## License

Proprietary. See [LICENSE](../../../../LICENSE).

- Free for personal and non-commercial use
- Commercial use requires a license
- Source code is confidential — do not distribute
