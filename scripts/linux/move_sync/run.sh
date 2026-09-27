#!/bin/bash
# #1748 move-sync harness: one private headless GNOME Shell run. Every XDG dir
# and the session bus are private, so nothing touches the user's session, and
# only sim-display is loaded, so nothing touches a 3D panel.
#
#   scripts/linux/move_sync/run.sh MODE SCALE OUTDIR DRAG...
#     MODE : default (the runtime's default: move sync when the extension has
#            it) | lattice (DXR_WL_MOVE_SYNC=0, the drag lattice) |
#            reweave (no move sync, no lattice)
#     SCALE: 2 | 1.5 | ... (nearest scale the virtual monitor supports)
#     DRAG : slow_h fast_h back_h slow_d fast_d reverse rest_start release_mid
#            tile_top tile_left freeze_app; rmb_<any of those> runs the same
#            motion as a right-button content drag (see driver.py)
#   env: EXTRA_APP_ENV="K=V ..." (app), SHELL_ENV="K=V ..." (shell),
#        AUDIT=1 (stamp readback, default) | 2 (paint intervals only),
#        EXT_SRC=DIR (install the extension from DIR instead of this tree,
#        e.g. an older version for the fallback check)
#
# Needs: the runtime built with --apps (build/, test_apps/cube_handle_vk_linux).
# The stamp audit overwrites the top-left 352x8 px of every frame
# (DXR_WL_ORIGIN_STAMP=1): a measurement hook, never for a real session.
# Then: analyze.py OUTDIR, summary.py OUTDIR..., stalls.py OUTDIR...
# Extension v10 tag gate (spec §9.9): EXTRA_APP_ENV="DXR_WL_TEST_TAG=off" (a
# synced app that never maps its tag) or "DXR_WL_TEST_TAG=toggle:300:300",
# then gate.py OUTDIR... (held vs plain frames, tag-lost releases).
# Kills only the processes it started.
set -u
H="$(cd "$(dirname "$0")" && pwd)"
BASE="${DXR_MS_HOME:-${TMPDIR:-/tmp}/dxr-move-sync}"
mkdir -p "$BASE"
export XDG_DATA_HOME="$BASE/data" XDG_CONFIG_HOME="$BASE/config" XDG_CACHE_HOME="$BASE/cache"
# Short: the Wayland socket path has a length limit.
export XDG_RUNTIME_DIR="${DXR_MS_RT:-/tmp/dxr-msync-rt}"
mkdir -p "$XDG_RUNTIME_DIR" && chmod 700 "$XDG_RUNTIME_DIR"
unset DBUS_SESSION_BUS_ADDRESS DISPLAY WAYLAND_DISPLAY
exec timeout 600 dbus-run-session -- "$H/session.sh" "$@"
