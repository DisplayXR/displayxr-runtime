# Linux hardware validation (Phase 1b / 2b / 3b + demos)

The DisplayXR runtime and all 5 demos are **build-green on Linux CI**, but CI has
no display, so on-screen behavior is unverified. This is the protocol for a
developer with a Linux box to validate it and report results. Tracked in
[#660](https://github.com/DisplayXR/displayxr-runtime/issues/660) (runtime) and
[#699](https://github.com/DisplayXR/displayxr-runtime/issues/699) (demos).

## What you need

- A Linux box with a **Vulkan-capable GPU + driver** and an **X server** (a normal
  X11 desktop session, or Wayland with XWayland — the runtime presents via XCB).
- No Leia / 3D-display hardware needed: **sim-display** weaves anaglyph (red/cyan)
  or side-by-side to a regular monitor.

**Verify the box first:**
```bash
echo "$DISPLAY"                       # must be non-empty (e.g. :0 or :1)
sudo apt-get install -y vulkan-tools
vulkaninfo --summary | head -n 20     # must list at least one GPU device
```

## One-time setup

Install build + run deps, and clone the runtime **with the 5 demos as siblings**
(the harness auto-finds `../displayxr-demo-*`):
```bash
sudo apt-get install -y \
  git build-essential cmake ninja-build pkg-config \
  libvulkan-dev vulkan-validationlayers glslang-tools \
  libxcb1-dev libxcb-randr0-dev libx11-xcb-dev libx11-dev \
  libwayland-dev libasound2-dev \
  libxcb-glx0-dev libxxf86vm-dev \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libcurl4-openssl-dev \
  libssl-dev zlib1g-dev autoconf automake autoconf-archive libtool nasm zip unzip tar \
  scrot                                        # for screenshots

mkdir -p ~/displayxr && cd ~/displayxr
gh repo clone DisplayXR/displayxr-runtime
for d in mediaplayer gaussiansplat modelviewer avatar earthview; do
  gh repo clone "DisplayXR/displayxr-demo-$d"
done
```

## The tests

Run each from the runtime repo. The harness builds the runtime on first use,
wires `XR_RUNTIME_JSON`/`XRT_PLUGIN_SEARCH_PATH` at it, and launches the target.
**Capture stderr to a log** (there is no log file on Linux — the runtime logs to
stderr) and **grab a screenshot** once the window is up:
```bash
cd ~/displayxr/displayxr-runtime
./scripts/run_linux_demo.sh cube-hosted --output=anaglyph 2>&1 | tee /tmp/cube-hosted.log
# in a second terminal, once the window is visible:
scrot -u /tmp/cube-hosted.png        # active window  (or: scrot /tmp/cube-hosted.png)
```

`anaglyph` (red/cyan) is the default and the easiest "is it stereo" check on a
normal monitor — use it for every test. `--output=sbs` (side-by-side) is an
optional alternative if you'd rather see the two views directly.

| # | Command | Phase | PASS means |
|---|---|---|---|
| 1 | `run_linux_demo.sh cube-hosted` | 1b | A window opens; a spinning textured cube renders; with `--output=anaglyph` you see red/cyan stereo separation; no crash; closes cleanly on window-close. |
| 2 | `run_linux_demo.sh cube-handle` | 3b | Same, but the app supplies its own X11 window — the log shows the **xlib window-binding** path (`XR_EXT_xlib_window_binding`), not hosted self-create. |
| 3 | *(service/IPC — below)* | 2b | The cube renders driven by the out-of-process `displayxr-service`. |
| 4 | `run_linux_demo.sh mediaplayer` | demo | Demo content renders in a window with anaglyph (red/cyan) stereo; no crash. |
| 5 | `run_linux_demo.sh gaussiansplat` | demo | " |
| 6 | `run_linux_demo.sh modelviewer` | demo | " |
| 7 | `run_linux_demo.sh avatar` | demo | " |
| 8 | `run_linux_demo.sh earthview` | demo | " (needs a Google 3D-Tiles API key — see the demo's README; skip if unavailable and note it) |

**Test 3 — service / IPC:**
```bash
./scripts/build_linux.sh --service
SVC=$(find build/src/xrt/targets/service -name displayxr-service -type f | head -1)
"$SVC" &                                          # start the always-on service
XRT_FORCE_MODE=ipc ./scripts/run_linux_demo.sh cube-hosted 2>&1 | tee /tmp/service.log
kill %1                                           # stop the service when done
```

Also try **resizing** each window and **closing** it — both should behave (resize
re-creates the swapchain; close exits the session cleanly).

## Known first-run risks (report these specifically if seen)

CI proved it compiles, not that it runs — these are the likely first live issues:
- **VK-native path not selected** → the harness sets `OXR_ENABLE_VK_NATIVE_COMPOSITOR=1`; the log should show `comp_vk_native` + XCB surface creation. If it falls back, capture the log.
- **`Queue family does not support presentation to XCB surface`** → driver/WSI mismatch — note GPU + driver.
- **Swapchain format/extent** errors from the real driver on swapchain create.
- **Resize** glitches / device-lost on window resize.
- **`xcb_connect failed — is DISPLAY set`** → no X server / `DISPLAY` unset (or running under pure Wayland with no XWayland).

## How to report

Comment on the validation issue with:

1. **Environment block** (paste the output):
   ```bash
   { uname -a; (lsb_release -d 2>/dev/null || cat /etc/os-release | head -1); \
     echo "DISPLAY=$DISPLAY XDG_SESSION_TYPE=$XDG_SESSION_TYPE"; \
     vulkaninfo --summary 2>/dev/null | grep -E 'deviceName|driverName|apiVersion' | head; } 2>&1
   ```
2. **Results table** — for tests 1–8: ✅/❌ + one line. Attach a screenshot per passing test and the `tee` log excerpt for any failure.
3. Anything from **Known first-run risks** you hit, verbatim from the log.

A blocked/failed test with a good log + environment is exactly as useful as a
pass — it's what turns into the next runtime fix.
