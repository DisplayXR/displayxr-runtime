# SteamOS support

**Status:** in progress. WS1 (install on an immutable root, with the CI proxy job), WS2 (the KDE window-geometry stand-in), WS3 (gamescope), WS4 (rotation), WS5 (gamepad) and WS7 (arm64 build) are written, on separate branches; WS6 (Steam games) is researched, with its launch options in `install.sh` and the rest left to the hardware run. **No SteamOS hardware run yet.**

Desktop Linux ships for Ubuntu ([linux-support.md](linux-support.md)). SteamOS differs in ways that matter to the runtime:

| | Ubuntu (shipping) | SteamOS 3.8+ |
|---|---|---|
| Base | Debian packages, writable `/usr` | Arch, **read-only OS image**, no dpkg; `/usr`, `/usr/local` and `/opt` can't be written |
| Desktop | GNOME, Wayland-first (X11 legacy) | Desktop Mode: **KDE Plasma 6.4 on Wayland** by default (X11 still selectable through `steamosctl`); Game Mode: **gamescope** (nested Xwayland) |
| Apps | native | native Linux games in **pressure-vessel** (Steam Runtime "sniper"); Windows games through **Proton** |
| GPU | Intel / NVIDIA validated | AMD **RADV** only (never measured; #2371) |
| OpenXR ActiveRuntime | ours after install | SteamVR claims it back whenever it starts |

## Hardware

| Device | CPU / GPU | Role | Priority |
|---|---|---|---|
| Steam Machine (2026) | Zen 4, semi-custom RDNA 3 | Host for an external 3D panel (DP/HDMI + USB eye tracker) | P0 |
| Steam Deck LCD / OLED | Van Gogh APU (RDNA 2); the internal panel is **natively portrait** (800×1280 mode, rotated) | Docked to a 3D panel over USB-C DP alt mode; the internal panel is 2D / sim-display | P1 |
| Other SteamOS handhelds | AMD APUs | As the Deck | P2 |
| Steam Frame | Snapdragon (arm64 SteamOS, FEX) | A headset, not a 3D-display host: arm64 build only | P3 |
| Steam Controller / Steam Input | virtual pad through uinput (VID 0x28de) | Gamepad on all of the above | P0 |

The Leia SR Linux plug-in installs to `/usr/lib/displayxr/plugins` + `/opt/leiasr`, and neither is writable on SteamOS. It needs a user-level layout from the vendor (WS1 follow-up).

## Workstreams

### WS1: install on an immutable root (landed)
- The tarball's `install.sh` already installs user-level by default (XDG paths, no root).
  - On SteamOS (`ID=steamos` in os-release) it now **refuses `--system`**. Elsewhere it refuses `--system` when `/usr/local` is read-only.
- Every install writes `openxr_displayxr.json` at a fixed path in the tree (`~/.local/share/displayxr/` user-level). That lets one game select DisplayXR with a Steam launch option, without fighting SteamVR for the ActiveRuntime:
  ```
  XR_RUNTIME_JSON=$HOME/.local/share/displayxr/openxr_displayxr.json %command%
  ```
- `build_linux.sh --bundled-cjson` compiles cJSON in with hidden symbols. The tarball is built with it, and CI asserts that no file in it needs `libcjson.so.1`, which the SteamOS base image and the Steam Runtime lack. The `.deb` keeps the system library.
- The IPC runtime-dir fallback with no `XDG_RUNTIME_DIR` / `XDG_CACHE_HOME` is now `$HOME/.cache`, then `/tmp`. Before, it was the literal string `~/.cache`, a relative path.
- Not changed: `u_file_get_config_dir` still falls back to `~/.config/monado` (Monado legacy; consent and app-profile files live there), and discovery still ignores `$XDG_DATA_DIRS`. Neither blocks a user-level install.

### WS2: KDE Plasma Wayland (Desktop Mode) (branch `feat/steamos-kde`)
The real KWin publisher is #1928: a native KWin plug-in from an outside contributor, built in their KWin fork and headed upstream. A stock KWin, and so SteamOS (KWin 6.4 on a read-only image), cannot load it. WS2 is therefore a **user-installable, geometry-only stand-in that steps aside for the real one** ([spec §4.4](../specs/runtime/wayland-window-geometry.md)):
- `contrib/kwin/displayxr-window-geometry`: a KWin script that pushes schema-1 snapshots, with no titles and no `buffer_device`.
- `displayxr-kwin-geometry-bridge`: D-Bus-activated, owns `org.displayxr.WindowGeometry`, and serves `GetWindows` / `WindowsChanged`.
  - It accepts snapshots from KWin only, validates and sanitises them (`tests_kwgb_payload`), and runs in KDE sessions only.
  - It never queues for the name and allows replacement, so a real publisher takes over.
- `install.sh`: sets both up in a KDE session (or with `--kwin`), user-level, and enables the script unless the user switched it off. `uninstall.sh` removes them.
- `comp_vk_native_wl_geom.c`: the "no publisher" line names the KDE case and its fix.

Deliberately not done:
- **Display scale from KScreen.** It isn't needed for correctness. Under KWin's Xwayland, DRM sysfs already gives the device mode, and the X11 1:1 gate compares it with the X11 rect. That is right in both of KWin's X11 scaling modes.
- **The workspace hotkey through the XDG `GlobalShortcuts` portal.** It's deferred: asynchronous portal code that can't be run without hardware, and an open question about how KDE's portal identifies a host (non-Flatpak) service. The workspace stays launchable from the dashboard and `displayxr-cli workspace launch`.
- **A note on #1928** explaining that this is a SteamOS stopgap, not a competing publisher. It is to be posted with the PR.

Until the stand-in is verified, selecting X11 in Desktop Mode (`steamosctl`) remains the fallback for windowed weaving.

### WS3: gamescope (Game Mode) (branch `feat/steamos-gamescope`)
Under gamescope, Steam games already get X11 windows on gamescope's Xwayland, so the existing X11 path applies. The Game Mode hazard is **scaling**:
- gamescope's X root is the *game resolution* Steam chose (often 1280×800 by default), and gamescope scales it to its one output.
- It also scales a smaller window up to fill the root.

Either is a resample, which a weave does not survive. So:
- `u_gamescope.{h,c}` (aux_util): detection (`GAMESCOPE_WAYLAND_DISPLAY`, or `gamescope` in `XDG_CURRENT_DESKTOP`), and a pure 1:1 verdict. Unit test: `tests_aux_gamescope`.
- The Vulkan compositor's X11 1:1 gate has a gamescope arm. It runs because gamescope's synthetic RandR output joins no connector, so the connector-mode rule never fires there.
  - If the game resolution is not the panel's native size, or the window is not the game resolution, the session presents 2D. The log line names the fix: the game's **Properties > General > Game Resolution = Native** in Game Mode.
  - A display processor that tolerates a resample keeps weaving.
- `DXR_LINUX_DIRECT_SCANOUT` is ignored under gamescope, because gamescope holds the DRM master.
- The service window's backend auto-select picks X11 under gamescope.
- The 3D panel must be gamescope's output: the Deck docked (gamescope drives the external display only), or a Steam Machine with one display.
- Not done: identifying the panel from DRM sysfs + EDID under gamescope. The panel size comes from the display processor.

### WS4: rotation and panel matching (branch `feat/steamos-rotation`)
- The device mode is now stored in the orientation the desktop presents it (`os_display_desktop_info::native_width`), with a new `rotation` field. Rotation comes from Mutter's transform, or is inferred from DRM sysfs.
- Panel matching accepts the plug-in's size in either orientation on a quarter-turned output.
- The EDID join matches transposed modes.
- The native-Wayland window sizes its buffer from the `wl_output` transform.
- Tests: the Deck cases in `tests_aux_display_desktop_select` and `tests_aux_display_edid_linux`.

### WS5: gamepad on Linux (branch `feat/steamos-gamepad`)
`gamepad_backend_evdev.c` reads `/dev/input/event*` with plain ioctls. It needs no libudev, and hot-plug is a throttled rescan.
- **Device preference:** Steam Input's virtual pad (28DE:11FF) first, then any other pad, then raw Valve devices (the Deck's hid-steam controls).
- **Face buttons:** xpad and spec layouts map to the Xbox positions.
- **Deck triggers:** hid-steam's HAT2 trigger axes are read.
- **Rumble:** over EV_FF.
- **Mapping:** pure, in `gamepad_evdev_map.c`, tested on every platform (`tests_gamepad_evdev_map`).
- **Packaging:** the tarball and the `.deb` ship `DisplayXR-Gamepad.so` and register it.

### WS6: Steam games (research done; launch options in `install.sh`; hardware test pending)
Sources are community wikis and project READMEs, not Valve documentation (vronlinux.org, ArchWiki, NixOS wiki, xrizer, ValveSoftware/Proton#6038, proton-ge-custom#906). Every point here is to be confirmed on hardware.

**Native Linux OpenXR games (Steam Linux Runtime / pressure-vessel).**
- The game runs in a container that shares `$HOME`, so the user-level runtime (`~/.local/share/displayxr`) and its manifest are visible. Per game, the launch option `install.sh` prints on SteamOS is:
  ```
  XR_RUNTIME_JSON=$HOME/.local/share/displayxr/openxr_displayxr.json PRESSURE_VESSEL_FILESYSTEMS_RW=$XDG_RUNTIME_DIR/displayxr_comp_ipc %command%
  ```
  The game's own OpenXR loader honours `XR_RUNTIME_JSON` (the game is never elevated). It selects DisplayXR without touching SteamVR's ActiveRuntime.
- **The socket.** `PRESSURE_VESSEL_FILESYSTEMS_RW` exposes the service socket. pressure-vessel does not share all of `$XDG_RUNTIME_DIR`, which is the Monado/WiVRn pattern. Ordinary apps run DisplayXR in-process (hybrid), so they don't need it. Present-owners and workspace clients do.
- **Not for DisplayXR:** `PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1` imports only the host's default Monado/WiVRn runtimes, and must not be combined with `XR_RUNTIME_JSON`.
- **Libraries:** the runtime is `dlopen`ed inside the container. Its glibc floor (2.35) is above sniper's 2.31, but pressure-vessel uses the host's newer glibc when the graphics stack needs it, and SteamOS's is newer. Its other `DT_NEEDED` (Vulkan loader, X11/xcb, libdbus, libudev) are expected in sniper, which is a gaming runtime that carries those; cJSON is bundled into the tarball (WS1).
  - Unverified: that every one resolves in sniper. A CI job in a plain sniper container can't settle it, because it would fail on glibc without pressure-vessel's host import, so this is a hardware check.
- **64-bit only.** The runtime ships x86_64 only, so 32-bit OpenXR games can't load it. `XR_RUNTIME_JSON` is also seen by 32-bit processes, which then fail.

**Windows OpenXR games (Proton).**
- Proton's `wineopenxr` forwards a Windows game's OpenXR calls to the host's Linux runtime. Graphics go through Vulkan, with D3D11/12 via DXVK/vkd3d. DisplayXR is a Linux Vulkan runtime, so this is the path, in the legacy-app class: no `XR_DXR_*` reaches a Windows game through `wineopenxr`, and framing comes from `DXR_LEGACY_CAMERA_RIG`.
- **Stock Proton** sets up `wineopenxr` only after Proton's OpenVR client initialises, which needs an OpenVR runtime: SteamVR running, or xrizer standing in for it (`VR_OVERRIDE`). Without one, the game stays flat (log: vrclient init failure, OpenXR negotiation error −6). On a 3D-display box with no headset, that means installing SteamVR or xrizer just to satisfy Proton.
- **GE-Proton** ships a "decoupled" `wineopenxr` that needs no OpenVR runtime. It's 64-bit only in recent releases; Proton Experimental has the 32-bit pieces. This is the simpler first test.
- The same launch options apply. They reach the Linux side of the Proton process.
- To record per title in COMPAT.md: Proton build, whether `wineopenxr` loaded DisplayXR (the runtime's `loaded from:` log line), the graphics API, and the extensions the game enabled.

**Hardware checklist additions:** a native OpenXR Steam game (64-bit) with the launch options above, in Desktop Mode and in Game Mode (where the WS3 gamescope gate applies); one D3D11 OpenXR title under GE-Proton; the same title under stock Proton with xrizer.

### WS7: arm64, build only (branch `feat/steamos-arm64`)
- The `.deb` soname gate allows `ld-linux-aarch64.so.1`.
- The tarball README names the real architecture.
- A non-required `Linux arm64 (build only)` CI job runs on `ubuntu-24.04-arm`: hybrid build, headless selftest, tarball package, user-level install, and selftest from the installed paths.
- Nothing x86-only was found in the Linux build: SSE flags are already x86-gated, and the ONNX webcam estimator, the one x86_64-only download, is opt-in (`stub` by default).
- **Not shipped for arm64:** there is no arm64 Leia SR, and the Steam Frame is a headset, not a 3D-display host.
- x86 games running under FEX on arm64 SteamOS would need the x86_64 runtime, not this build, and are untested.

## CI
`build-linux.yml` → **`SteamOS (Arch)`** (not required):
- Builds the hybrid runtime with `--bundled-cjson` on `archlinux:base-devel`.
- Runs the headless selftest and the SteamOS-relevant host unit tests.
- Packages the tarball and installs it as a **non-root user** with a SteamOS os-release (`DXR_OS_RELEASE`): `--system` refused, user install, `displayxr-cli selftest`, the service smoke test, uninstall, and nothing written outside `$HOME`.

## Hardware checklist (later)
Report each line Pass/Fail with its conditions (runtime + plug-in version, depth %, fps, depth budget).
1. Steam Machine + 3D panel, Desktop Mode X11: `cube_handle_vk_linux`, `cube_hosted_legacy_vk_linux`, then a Unity Linux player.
2. The same in Desktop Mode Wayland (after WS2) and in Game Mode (after WS3).
3. A native Steam OpenXR game with the `XR_RUNTIME_JSON` launch option (pressure-vessel). Then a Proton OpenXR title.
4. Steam Deck docked to the panel. Steam Deck internal panel with sim-display (rotation, WS4).
5. Steam Controller and Deck controls through Steam Input driving the app camera (WS5).
6. A RADV motion-to-photon row in [linux-motion-to-photon.md](linux-motion-to-photon.md), and atlas captures for depth.
