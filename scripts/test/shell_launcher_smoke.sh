#!/usr/bin/env bash
# Shell launcher smoke test — autonomous end-to-end exerciser for Phase 5 tasks.
#
# Launches displayxr-shell + cube_handle_d3d11_win, activates the shell via
# Ctrl+Space, captures a screenshot, toggles Ctrl+L to show the launcher panel,
# captures again, toggles Ctrl+L to hide, captures a third time, cleans up.
#
# Outputs under scripts/test/out/:
#   shell_pre.png       — shell active, no launcher
#   shell_launcher.png  — launcher panel ON
#   shell_post.png      — launcher panel OFF again (should match pre)
#   shell_test.log      — shell stdout/stderr
#
# Intended for iterative development of Phase 5 launcher UI. Edit code,
# rebuild, re-run this script, Read the screenshots.
#
# Usage:
#   bash scripts/test/shell_launcher_smoke.sh
#
# Requires a successful `scripts\build_windows.bat build` first.

set -u

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

OUT_DIR="scripts/test/out"
mkdir -p "$OUT_DIR"

SHELL_EXE="_package\\bin\\displayxr-shell.exe"
CUBE_EXE="test_apps\\cube_handle_d3d11_win\\build\\cube_handle_d3d11_win.exe"
LOG="$OUT_DIR/shell_test.log"

SEND_KEYS_PS1="scripts/test/_send_keys.ps1"
CAPTURE_PS1="scripts/test/_capture_shell.ps1"

echo "=== shell_launcher_smoke.sh ==="
echo "repo: $REPO_ROOT"

# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------

kill_all() {
    cmd.exe //c "taskkill /F /IM displayxr-shell.exe" >/dev/null 2>&1 || true
    cmd.exe //c "taskkill /F /IM displayxr-service.exe" >/dev/null 2>&1 || true
    cmd.exe //c "taskkill /F /IM cube_handle_d3d11_win.exe" >/dev/null 2>&1 || true
}

# Send a key combo. Args: hex VK codes. Example: send_combo 0x11 0x4C
send_combo() {
    powershell -NoProfile -ExecutionPolicy Bypass -File "$SEND_KEYS_PS1" "$@"
}

# Capture the shell compositor window to an absolute path. Arg: output PNG path
# relative to repo root.
capture_shell() {
    local out="$1"
    local abs_out
    abs_out="$(cygpath -w "$REPO_ROOT/$out" 2>/dev/null || echo "$REPO_ROOT/$out")"
    powershell -NoProfile -ExecutionPolicy Bypass -File "$CAPTURE_PS1" -OutPath "$abs_out"
}

# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------

trap kill_all EXIT

echo "[1/10] cleaning up prior runs..."
kill_all
sleep 1
# Clear previous outputs so old files don't confuse debugging.
rm -f "$OUT_DIR"/*.png "$LOG" 2>/dev/null || true

echo "[2/10] preflight..."
if [ ! -f "$SHELL_EXE" ]; then
    echo "ERROR: $SHELL_EXE not found. Run scripts\\build_windows.bat build first."
    exit 1
fi
if [ ! -f "$CUBE_EXE" ]; then
    echo "ERROR: $CUBE_EXE not found. Run scripts\\build_windows.bat test-apps first."
    exit 1
fi

echo "[3/10] launching shell + cube (log: $LOG)..."
cmd.exe //c "$SHELL_EXE $CUBE_EXE > $LOG 2>&1" &
SHELL_PID=$!
echo "  shell wrapper pid=$SHELL_PID"

echo "[4/10] waiting 20s for service + shell activation + eye-tracking warmup..."
# When launched with an app arg, displayxr-shell auto-activates the shell
# mode and launches the cube. Eye tracking takes several seconds to stabilize
# after activation — during the warmup window the shell shows the left eye
# stretched to full screen, which makes screenshots misleading. Wait long
# enough for eye tracking to be fully online before capturing.
sleep 20

echo "[5/10] capturing shell_pre.png (no launcher)..."
capture_shell "$OUT_DIR/shell_pre.png"
sleep 1

echo "[7/10] sending Ctrl+L (launcher ON)..."
send_combo 0x11 0x4C
sleep 2

echo "[8/10] capturing shell_launcher.png (launcher ON)..."
capture_shell "$OUT_DIR/shell_launcher.png"
sleep 1

echo "[9/10] sending Ctrl+L (launcher OFF) + capturing shell_post.png..."
send_combo 0x11 0x4C
sleep 2
capture_shell "$OUT_DIR/shell_post.png"
sleep 1

echo "[10/10] cleaning up..."

kill_all
wait $SHELL_PID 2>/dev/null || true

echo ""
echo "=== outputs ==="
ls -la "$OUT_DIR" 2>/dev/null
echo ""
echo "=== shell log (last 25 lines) ==="
tail -25 "$LOG" 2>/dev/null || echo "(no log)"
