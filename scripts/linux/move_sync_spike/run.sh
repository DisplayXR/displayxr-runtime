#!/bin/bash
# #1748 spike harness: one private headless GNOME Shell 50 run. Every XDG dir
# and the session bus are private, so nothing touches the user's session, and
# only sim-display is loaded, so nothing touches a 3D panel.
#
#   scripts/linux/move_sync_spike/run.sh MODE SCALE OUTDIR DRAG...
#     MODE : table | reweave | sync
#     SCALE: 2 | 1.5 | ... (nearest scale the virtual monitor supports)
#     DRAG : slow_h fast_h back_h slow_d fast_d reverse rest_start release_mid
#            tile_top tile_left freeze_app   (see driver.py)
#   env: EXTRA_APP_ENV="K=V ..." (app), SHELL_ENV="K=V ..." (shell),
#        AUDIT=1 (stamp readback, default) | 2 (paint intervals only)
#
# Needs: the runtime built with --apps (build/, test_apps/cube_handle_vk_linux).
# Then: analyze.py OUTDIR, summary.py OUTDIR..., stalls.py OUTDIR...
set -u
H="$(cd "$(dirname "$0")" && pwd)"
BASE="${DXR_SPIKE_HOME:-${TMPDIR:-/tmp}/dxr-move-sync-spike}"
mkdir -p "$BASE"
export XDG_DATA_HOME="$BASE/data" XDG_CONFIG_HOME="$BASE/config" XDG_CACHE_HOME="$BASE/cache"
# Short: the Wayland socket path has a length limit.
export XDG_RUNTIME_DIR="${DXR_SPIKE_RT:-/tmp/dxr-msync-rt}"
mkdir -p "$XDG_RUNTIME_DIR" && chmod 700 "$XDG_RUNTIME_DIR"
unset DBUS_SESSION_BUS_ADDRESS DISPLAY WAYLAND_DISPLAY
exec timeout 600 dbus-run-session -- "$H/session.sh" "$@"
