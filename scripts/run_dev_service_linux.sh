#!/usr/bin/env bash
# Copyright 2026, DisplayXR
# SPDX-License-Identifier: BSL-1.0
#
# Start a dev displayxr-service from the build tree on a real Linux desktop,
# beside an installed runtime, with sim-display as its only component. For the
# weave probes (test_apps/probes/weave_probe_vk_linux) and any other IPC client
# you want to point at a build-tree service without touching the installed one.
#
#   ./scripts/run_dev_service_linux.sh &          # service in the foreground of this job
#   . "$XDG_RUNTIME_DIR/dxr-dev/client.env"       # the client's side, in the shell that runs the probe
#   build/run_weave_probe_vk_linux.sh --cursor
#
# What it sets up, and why (docs/getting-started/building.md, "A dev service
# beside an installed runtime"):
#
#   socket    The service binds, and a client dials, $XDG_RUNTIME_DIR/displayxr_comp_ipc
#             (u_file_get_path_in_runtime_dir); there is no other knob. The
#             installed displayxr.socket owns that path in the real runtime
#             dir, so the dev pair gets a SUB-directory of it as its
#             XDG_RUNTIME_DIR. A directory elsewhere (/tmp/...) would work for
#             the socket too; the sub-directory keeps it on the per-user tmpfs.
#   Wayland   libwayland resolves a relative WAYLAND_DISPLAY against
#             XDG_RUNTIME_DIR, so a changed XDG_RUNTIME_DIR hides the
#             compositor's socket. WAYLAND_DISPLAY is made absolute.
#   D-Bus     The session-bus fallback is $XDG_RUNTIME_DIR/bus, which the
#             sub-directory does not have, so DBUS_SESSION_BUS_ADDRESS is set
#             explicitly: the real bus by default, a path that does not exist
#             with --no-session-bus.
#   plug-ins  XRT_PLUGIN_SEARCH_PATH is PREPENDED to the built-in roots
#             (~/.local/share/DisplayXR/DisplayProcessors,
#             /usr/lib/displayxr/plugins, ...), it does not replace them, and
#             build/_plugins also holds the sim-input, sim-stylus, sim-tracking,
#             gamepad and conversion plug-ins. So: a private directory with
#             only sim-display's manifest and library, DXR_PLUGIN_EXCLUSIVE to
#             keep every other display plug-in from being loaded, and the three
#             per-class kill switches for the other component classes.
#   captures  TMPDIR is private, so capture triggers and their output do not
#             mix with the installed service's.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/build"
NAME="dxr-dev"
SESSION_BUS=1
PRINT_ONLY=0

usage() {
    cat <<EOF
usage: $0 [--build-dir DIR] [--name NAME] [--no-session-bus] [--print-env] [-- SERVICE_ARGS...]

  --build-dir DIR    build tree (default: <repo>/build; needs build_linux.sh --service or --hybrid)
  --name NAME        sub-directory of \$XDG_RUNTIME_DIR for this dev service (default: dxr-dev)
  --no-session-bus   point DBUS_SESSION_BUS_ADDRESS at a path that does not exist, so the dev
                     service reaches nothing on the session bus (GNOME Shell extension, Mutter
                     DisplayConfig). Not for a run that tests windowed weaving, the workspace
                     hotkey, or anything else the service gets over D-Bus.
  --print-env        set up the directories, write client.env, print it, do not start the service

Environment you set is passed through to the service (SIM_DISPLAY_*, DXR_*, XRT_LOG, ...).
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --name) NAME="$2"; shift 2 ;;
    --no-session-bus) SESSION_BUS=0; shift ;;
    --print-env) PRINT_ONLY=1; shift ;;
    -h | --help) usage; exit 0 ;;
    --) shift; break ;;
    *) usage >&2; exit 2 ;;
    esac
done

SERVICE="$BUILD_DIR/src/xrt/targets/service/displayxr-service"
SRC_PLUGINS="$BUILD_DIR/_plugins"
[ -x "$SERVICE" ] || { echo "error: $SERVICE missing (build with scripts/build_linux.sh --hybrid or --service)" >&2; exit 1; }
if [ ! -f "$SRC_PLUGINS/DisplayXR-SimDisplay.so" ] || [ ! -f "$SRC_PLUGINS/200-sim-display.json" ]; then
    echo "error: sim-display plug-in missing under $SRC_PLUGINS" >&2; exit 1
fi

REAL_RT="${XDG_RUNTIME_DIR:-}"
if [ -z "$REAL_RT" ] || [ ! -d "$REAL_RT" ]; then
    echo "error: XDG_RUNTIME_DIR is not set to a directory (run this from the desktop session)" >&2; exit 1
fi
case "$NAME" in
*/* | "" | . | ..) echo "error: --name must be a plain directory name" >&2; exit 2 ;;
esac

DEV_RT="$REAL_RT/$NAME"
PLUG="$DEV_RT/plugins"
CAP="$DEV_RT/tmp"
mkdir -p "$DEV_RT" "$PLUG" "$CAP"
chmod 0700 "$DEV_RT"

# sim-display only: its library, and its manifest with binary_path repointed
# at the copy (the build tree's manifest names build/_plugins).
cp "$SRC_PLUGINS/DisplayXR-SimDisplay.so" "$PLUG/"
sed "s|\"binary_path\":.*|\"binary_path\":  \"$PLUG/DisplayXR-SimDisplay.so\",|" \
    "$SRC_PLUGINS/200-sim-display.json" > "$PLUG/200-sim-display.json"

# An absolute WAYLAND_DISPLAY survives the XDG_RUNTIME_DIR change.
WL="${WAYLAND_DISPLAY:-}"
if [ -n "$WL" ] && [ "${WL#/}" = "$WL" ]; then
    WL="$REAL_RT/$WL"
fi

if [ "$SESSION_BUS" = 1 ]; then
    BUS="${DBUS_SESSION_BUS_ADDRESS:-}"
    if [ -z "$BUS" ] && [ -S "$REAL_RT/bus" ]; then
        BUS="unix:path=$REAL_RT/bus"
    fi
else
    BUS="unix:path=$DEV_RT/no-session-bus"
fi

# The client's half: same runtime dir (the socket), same TMPDIR (captures), and
# the two session addresses a windowed client needs after the directory change.
# The service's display processor is the service's business: nothing here.
{
    echo "export XDG_RUNTIME_DIR='$DEV_RT'"
    echo "export TMPDIR='$CAP'"
    echo "export XRT_FORCE_MODE=ipc"
    if [ -n "$WL" ]; then echo "export WAYLAND_DISPLAY='$WL'"; fi
    if [ "$SESSION_BUS" = 1 ] && [ -n "$BUS" ]; then echo "export DBUS_SESSION_BUS_ADDRESS='$BUS'"; fi
} > "$DEV_RT/client.env"

echo "dev service runtime dir: $DEV_RT (socket: $DEV_RT/displayxr_comp_ipc)" >&2
echo "client environment ($DEV_RT/client.env):" >&2
sed 's/^/    /' "$DEV_RT/client.env" >&2

if [ "$PRINT_ONLY" = 1 ]; then
    exit 0
fi

export XDG_RUNTIME_DIR="$DEV_RT"
export TMPDIR="$CAP"
if [ -n "$WL" ]; then export WAYLAND_DISPLAY="$WL"; fi
if [ -n "$BUS" ]; then export DBUS_SESSION_BUS_ADDRESS="$BUS"; fi
export XRT_PLUGIN_SEARCH_PATH="$PLUG"
export DXR_PLUGIN_EXCLUSIVE=sim-display
export DXR_CONV_PROVIDERS=0 DXR_INPUT_PROVIDERS=0 DXR_TRACKING_PROVIDERS=0
exec "$SERVICE" "$@" </dev/null
