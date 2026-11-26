# SteamOS hardware validation

The SteamOS work ([roadmap](../roadmap/steamos-support.md); PRs #2842 and #2844–#2848) is green on CI. CI has no SteamOS, no gamescope, no KWin, no Deck and no 3D panel, so none of it has run where it's meant to. This is the protocol for someone with the hardware to check it and report results.

Every PASS below is a specific log line or command output. Report each test as **PASS** or **FAIL** with its conditions: runtime commit, SteamOS version, device, display, and depth/fps where it applies. Never report it as a description of how it looked.

## What you need

| Tier | Hardware | Covers |
|---|---|---|
| **A** (do first) | Any SteamOS device (Steam Deck LCD/OLED or Steam Machine) and an ordinary monitor | Install, KDE geometry, gamescope gate, Deck rotation, gamepad. **sim-display** shows anaglyph (red/cyan) stereo on a normal screen |
| **B** | A Steam Machine (or a docked Deck) and a **Leia SR panel** | Real weaving. **Blocked**: the Leia SR Linux plug-in installs to `/usr/lib/displayxr/plugins` and `/opt/leiasr`, which are read-only on SteamOS. It needs a user-level layout from Leia, or an unofficial manual extraction (§B) |

You also need SteamOS **3.8 or newer** (Desktop Mode is KDE Plasma on Wayland), Desktop Mode for setup (Steam button > Power > Switch to Desktop), and a keyboard.

## One-time setup: build in a distrobox, install on the host

SteamOS has no compiler, but it ships **distrobox** and **podman**. Build in Ubuntu 22.04, the base CI builds the tarball on. Your home folder is shared, so the build output lands where the host can use it.

```bash
# Desktop Mode, Konsole. The build container:
distrobox create --name dxr-build --image ubuntu:22.04
distrobox enter dxr-build

# --- inside the container ---
sudo apt-get update && sudo apt-get install -y \
  git build-essential cmake ninja-build pkg-config python3 \
  libvulkan-dev glslang-tools libeigen3-dev libudev-dev \
  libxcb1-dev libxcb-randr0-dev libxcb-shape0-dev libx11-dev libx11-xcb-dev libwayland-dev \
  libdbus-1-dev libgl-dev libglvnd-dev libegl1-mesa-dev libxrandr-dev libxxf86vm-dev libxcb-glx0-dev
git clone https://github.com/joehillthunder/displayxr-runtime.git ~/dxr && cd ~/dxr
git checkout steamos-integration        # all six SteamOS PRs merged together
git submodule update --init --recursive
CMAKE_BUILD_TYPE=Release ./scripts/build_linux.sh --hybrid --bundled-cjson --apps
./scripts/package_linux.sh --no-build   # -> dist/displayxr-runtime-linux-x86_64-*.tar.gz
exit
# --- back on the SteamOS host ---

mkdir -p ~/dxr-install && tar -C ~/dxr-install -xzf ~/dxr/dist/displayxr-runtime-linux-*.tar.gz
cd ~/dxr-install/displayxr-runtime-linux-*/
./install.sh 2>&1 | tee ~/dxr-install.log
```

Run the cube test app against the **installed** runtime, not the dev build:

```bash
# A helper used throughout: the handle cube against the installed runtime, in sim-display's anaglyph mode.
dxr-cube() {
  XR_RUNTIME_JSON=~/.local/share/displayxr/openxr_displayxr.json \
  LD_LIBRARY_PATH=~/dxr/build/_openxr-1.1.63/lib \
  OXR_ENABLE_VK_NATIVE_COMPOSITOR=1 SIM_DISPLAY_START_3D=1 SIM_DISPLAY_OUTPUT=anaglyph \
  ~/dxr/test_apps/cube_handle_vk_linux/build/cube_handle_vk_linux "$@"
}
```

The runtime logs to **stderr**, so capture every run: `dxr-cube 2>&1 | tee ~/t<N>.log`.

## Tier A tests

| # | Area (PR) | Do | PASS means |
|---|---|---|---|
| A1 | Install (#2842) | `./install.sh` above, then `~/.local/share/displayxr/bin/displayxr-cli selftest` | The install log has `SteamOS: SteamVR takes the OpenXR ActiveRuntime back` and the `XR_RUNTIME_JSON=… PRESSURE_VESSEL_FILESYSTEMS_RW=…` line. `./install.sh --system` is **refused**. The selftest passes. Nothing was written under `/usr` or `/etc` |
| A2 | Install survives an OS update (#2842) (optional, when an update is available) | Apply the SteamOS update, then repeat the selftest | The selftest still passes, and `~/.local/share/displayxr` is intact |
| A3 | KDE publisher (#2847) | In Desktop Mode: `busctl --user call org.displayxr.WindowGeometry /org/displayxr/WindowGeometry org.displayxr.WindowGeometry1 GetWindows`. Then move a window and call it again | It returns a JSON string with `"layout_mode":"logical"` and one entry per normal window, with `frame`, `buffer` and `monitor.device_scale` equal to the display scale. The second call shows the moved `frame`. `journalctl --user -b \| grep kwin-geometry-bridge` shows `serving org.displayxr.WindowGeometry (geometry only)` |
| A4 | KDE windowed weave (#2847) | `dxr-cube --platform=wayland 2>&1 \| tee ~/t4.log`. Move the window | Red/cyan stereo in a **window**, with no `wl_geom: … no window-geometry publisher` line in the log. The image stays stereo while you drag. Then repeat at 125 % display scale |
| A5 | KDE fallback (#2847) | Disable the script (System Settings > Window Management > KWin Scripts), log out and back in, and run A4 again | The log names the fix (`the DisplayXR KWin script is installed … but no publisher answered`). Re-enabling the script restores A4 |
| A6 | Deck rotation (#2844) (Deck only) | Run `displayxr-cli info` on the Deck's internal screen | `desktop rect: 1280x800 … on 'eDP-1' … [rule: connector match]`, then `connector mode 1280x800 (…) - the rect is device pixels (X11 windows land 1:1)`. Before #2844 the rule was `primary fallback` and the connector mode read 800x1280 |
| A7 | Deck rotation, Wayland (#2844) (Deck only) | `dxr-cube --platform=wayland` fullscreen on the internal screen | The log line `3D panel output … mode 1280x800 device px (transform N)` has an odd N. The cube shows landscape at the right size, not portrait or squeezed |
| A8 | Gamepad (#2845) | With Steam running and a pad (Deck controls, Steam Controller or Xbox pad), run `dxr-cube` and move the sticks | `gamepad: evdev /dev/input/eventN opened: 28DE:11FF '…' (… xpad layout, rank 3: Steam Input virtual pad…)`. The sticks move the view, and A/B/X/Y act on the Xbox positions |
| A9 | Gamepad without Steam (#2845) (Deck) | Quit Steam (Desktop Mode, close the Steam client) and run A8 again | The opened line names the Deck's own device (`28DE:1205`, rank 1). Restarting Steam switches to the virtual pad within a few seconds (`replaced by a preferred pad`) |
| A10 | gamescope gate (#2846) | In Desktop Mode, add `~/dxr/test_apps/cube_handle_vk_linux/build/cube_handle_vk_linux` as a non-Steam game. Give it the launch options in the note below. In **Game Mode**, give it a game resolution **smaller than the screen**: on a Steam Machine with a 1080p/4K monitor the default usually is; on a Deck's own 1280×800 screen pick a lower one, because the default already matches. Start it | `~/t10.log` has `NOT_1TO1 (gamescope): presenting 2D … Fix: … Game Resolution = Native`, and the cube is flat, not a double image |
| A11 | gamescope gate, Native (#2846) | Set the shortcut's Properties > General > **Game Resolution = Native**, then start it again | `~/t10.log` has `gamescope 1:1 gate: game resolution WxH = the 3D panel's WxH … weaving`, and the stereo pattern is visible |
| A12 | Native Steam game (WS6) | A 64-bit native Linux OpenXR game, with the launch options `install.sh` printed | The game's log (or the runtime's stderr through `%command% 2>~/t12.log`) shows DisplayXR loaded, not SteamVR, and the game renders |
| A13 | Proton (WS6) | A D3D11 OpenXR title under **GE-Proton**, with the same launch options | DisplayXR is loaded through `wineopenxr`. Record the Proton build and the extensions the game enabled. Then try stock Proton with xrizer, and record PASS or FAIL for each |
| A14 | ARM (#2848) | Only if you have arm64 SteamOS hardware | `displayxr-cli selftest` passes |

To get the depth and stereo check on a normal monitor, keep `SIM_DISPLAY_OUTPUT=anaglyph` and look through red/cyan glasses. `V` toggles 2D/3D.

**Launch options for A10/A11.** `interlaced` is a sim-display mode that needs 1:1 pixels. Anaglyph survives scaling, so it would keep weaving and the gate would never show. Replace `deck` with your user name if it differs:
```
XR_RUNTIME_JSON=/home/deck/.local/share/displayxr/openxr_displayxr.json LD_LIBRARY_PATH=/home/deck/dxr/build/_openxr-1.1.63/lib OXR_ENABLE_VK_NATIVE_COMPOSITOR=1 SIM_DISPLAY_START_3D=1 SIM_DISPLAY_OUTPUT=interlaced %command% 2>/home/deck/t10.log
```
If `~/t10.log` stays empty (Steam didn't apply the redirect), report that, and read the output through `journalctl --user -b` instead.

## Tier B (Leia SR panel), once the plug-in can be installed per user

The Leia `.deb` can't be installed on SteamOS. Until Leia ships a user-level layout, B is **blocked**; report it as such. Unofficial attempt, if you want to try it:
1. Extract the `.deb`: `ar x displayxr-leia-sr_*.deb && tar -xf data.tar.*`.
2. Copy the plug-in `.so` into `~/.local/share/DisplayXR/DisplayProcessors/`, with a manifest whose `binary_path` is absolute.
3. Copy `/opt/leiasr` to a folder in your home and add its `lib` to `LD_LIBRARY_PATH`.

If the plug-in has the `/opt/leiasr` path built in, this fails. Report exactly what failed.

With the panel working:

| # | Do | PASS means |
|---|---|---|
| B1 | A4 and A11 on the panel, Desktop Mode and Game Mode | Glasses-free 3D with no crosstalk, the subject at a middle size, and depth inside the comfort budget (0.14 % front / 0.42 % behind). Record depth % and fps |
| B2 | A Deck docked to the panel over USB-C, in Game Mode | gamescope drives the external display only, and A11's weaving line names the panel's size |
| B3 | Motion-to-photon on RADV | A row for AMD in [linux-motion-to-photon.md](../roadmap/linux-motion-to-photon.md) |

## How to report

Comment on #2842 with:

1. **Environment:**
   ```bash
   { cat /etc/os-release | grep -E '^(NAME|VERSION_ID|BUILD_ID)'; uname -r; \
     echo "XDG_CURRENT_DESKTOP=$XDG_CURRENT_DESKTOP XDG_SESSION_TYPE=$XDG_SESSION_TYPE"; \
     vulkaninfo --summary 2>/dev/null | grep -E 'deviceName|driverName|driverInfo' | head -4; \
     git -C ~/dxr rev-parse --short HEAD; } 2>&1
   ```
2. **Results:** A1–A14 (and B1–B3) as **PASS / FAIL / BLOCKED**, each with the matching log line. For any FAIL, attach the `tee` log.
3. **Screenshots:** a photo of each 3D pass. Anaglyph needs glasses to judge; Tier B needs the panel.

A FAIL or BLOCKED result with a good log is as useful as a PASS: it's what turns into the next fix.
