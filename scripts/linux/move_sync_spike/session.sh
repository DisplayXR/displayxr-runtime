#!/bin/bash
# Runs INSIDE dbus-run-session (see run.sh): a headless GNOME Shell with this
# tree's window-geometry extension, cube_handle_vk_linux on sim_display, and
# scripted title-bar drags through org.gnome.Mutter.RemoteDesktop.
set -u
H="$(cd "$(dirname "$0")" && pwd)"
WT="$(cd "$H/../../.." && pwd)"
MODE=$1; SCALE=$2; OUT=$3; shift 3
rm -rf "$OUT"; mkdir -p "$OUT"
unset DISPLAY WAYLAND_DISPLAY XDG_SESSION_TYPE XDG_CURRENT_DESKTOP GNOME_SETUP_DISPLAY
export GSETTINGS_BACKEND=dconf

EXT="$XDG_DATA_HOME/gnome-shell/extensions/window-geometry@displayxr.org"
rm -rf "$EXT"; mkdir -p "$EXT"
cp "$WT"/contrib/gnome-shell/window-geometry@displayxr.org/{metadata.json,extension.js,lib.js} "$EXT/"
gsettings set org.gnome.shell disable-user-extensions false
gsettings set org.gnome.shell enabled-extensions "['window-geometry@displayxr.org']"
gsettings set org.gnome.shell welcome-dialog-last-shown-version '999'
gsettings set org.gnome.desktop.interface enable-animations false
gsettings set org.gnome.desktop.interface enable-hot-corners false
gsettings set org.gnome.mutter edge-tiling true

# shellcheck disable=SC2086
env ${SHELL_ENV:-} DISPLAYXR_DEBUG=1 DISPLAYXR_STAMP_AUDIT="${AUDIT:-1}" gnome-shell --headless --wayland \
    --no-x11 --wayland-display wayland-dxr-msync --virtual-monitor 3840x2160 > "$OUT/shell.log" 2>&1 &
SHELL_PID=$!
for _ in $(seq 1 60); do
    gdbus call --session --dest org.displayxr.WindowGeometry --object-path /org/displayxr/WindowPlacement \
        --method org.displayxr.WindowPlacement1.GetPlacementCapabilities > "$OUT/caps.txt" 2>&1 && break
    sleep 0.5
done
python3 "$H/driver.py" scale "$SCALE" > "$OUT/driver.log" 2>&1
sleep 1

# sim_display ONLY. An installed runtime package puts a vendor plug-in in
# /usr/lib/displayxr/plugins, which discovery always scans: an isolated
# XRT_PLUGIN_SEARCH_PATH alone does NOT keep it out, and it would bind the
# panel. DXR_PLUGIN_EXCLUSIVE does; the guard below aborts if it ever fails.
PLUG="$XDG_RUNTIME_DIR/plug"
mkdir -p "$PLUG"
cp "$WT/build/_plugins/DisplayXR-SimDisplay.so" "$PLUG/"
sed "s|\"binary_path\":.*|\"binary_path\":  \"$PLUG/DisplayXR-SimDisplay.so\",|" \
    "$WT/build/_plugins/200-sim-display.json" > "$PLUG/200-sim-display.json"
export XRT_PLUGIN_SEARCH_PATH="$PLUG" DXR_PLUGIN_EXCLUSIVE=sim-display
export XDG_CURRENT_DESKTOP=GNOME WAYLAND_DISPLAY=wayland-dxr-msync
export XR_RUNTIME_JSON="$WT/build/openxr_displayxr-dev.json"
export LD_LIBRARY_PATH="$(ls -d "$WT"/build/_openxr-*/lib | head -1)"
export VK_LAYER_PATH="$WT/build/src/xrt/targets/vk_layer"
export OXR_ENABLE_VK_NATIVE_COMPOSITOR=1 SIM_DISPLAY_OUTPUT=anaglyph
# The phase model: period 5 device px is not a divisor of the 3-logical-px
# lattice probe grid at any tested scale, so the table baseline is constrained.
export SIM_DISPLAY_INTERLACE_PERIOD="${SIM_PERIOD:-5}"
# The simulated panel = the virtual monitor, so the window is "on the panel".
export SIM_DISPLAY_PIXEL_W=3840 SIM_DISPLAY_PIXEL_H=2160
export DXR_WL_ORIGIN_STAMP=1 DXR_CUBE_WINDOW="${DXR_CUBE_WINDOW:-1600x1000}" XRT_LOG=info
case "$MODE" in
    table) ;;
    reweave) export DXR_WL_DRAG_LATTICE=0 ;;
    sync) export DXR_WL_MOVE_SYNC=1 DXR_WL_DRAG_LATTICE=0 ;;
    *) echo "unknown mode $MODE"; kill $SHELL_PID; exit 2 ;;
esac
for kv in ${EXTRA_APP_ENV:-}; do export "${kv?}"; done
"$WT/test_apps/cube_handle_vk_linux/build/cube_handle_vk_linux" --platform=wayland --windowed > "$OUT/app.log" 2>&1 &
APP_PID=$!
unset WAYLAND_DISPLAY
echo "shell $SHELL_PID app $APP_PID" > "$OUT/pids"

abort() {
    echo "ABORT: $1" >> "$OUT/driver.log"
    kill -9 $APP_PID 2>/dev/null; kill $SHELL_PID 2>/dev/null; sleep 1; kill -9 $SHELL_PID 2>/dev/null
    exit 3
}
for _ in $(seq 1 40); do
    grep -qiE "leia_lnx_plugin|active plug-in: id=leia|DisplayXR-LeiaSR" "$OUT/app.log" && abort "a vendor plug-in was touched"
    grep -q "active plug-in: id=sim-display" "$OUT/app.log" && break
    sleep 0.25
done
grep -q "active plug-in: id=sim-display" "$OUT/app.log" || abort "sim-display not active"

for _ in $(seq 1 60); do
    python3 "$H/driver.py" ready "$APP_PID" "$OUT/ready.json" > /dev/null 2>&1 && break
    sleep 0.5
done
sleep 2
for kind in "$@"; do
    echo "== drag $kind" >> "$OUT/driver.log"
    python3 "$H/driver.py" ready "$APP_PID" "$OUT/ready.json" >> "$OUT/driver.log" 2>&1
    APP_PID=$APP_PID python3 "$H/driver.py" drag "$OUT/ready.json" "$kind" "$OUT/marks.log" >> "$OUT/driver.log" 2>&1
    sleep 1.2
done
python3 "$H/driver.py" ready "$APP_PID" "$OUT/final.json" >> "$OUT/driver.log" 2>&1
# Only the PIDs this script started.
kill $APP_PID 2>/dev/null; sleep 1; kill -9 $APP_PID 2>/dev/null
kill $SHELL_PID 2>/dev/null; sleep 1; kill -9 $SHELL_PID 2>/dev/null
wait 2>/dev/null
echo done >> "$OUT/driver.log"
