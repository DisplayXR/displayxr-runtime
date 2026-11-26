#!/bin/bash
# DisplayXR runtime — Linux installer (#705, v1: runtime + sim_display, user-level).
#
# Run from the unpacked tarball root. Default is a USER-LEVEL install — no root:
#   ./install.sh
#     runtime + plug-in     -> $XDG_DATA_HOME/displayxr            (~/.local/share/displayxr)
#     OpenXR ActiveRuntime  -> $XDG_CONFIG_HOME/openxr/1/active_runtime.json
#     DP discovery manifest -> $XDG_DATA_HOME/DisplayXR/DisplayProcessors/200-sim-display.json
#     Vulkan layer manifest -> $XDG_DATA_HOME/vulkan/explicit_layer.d/VkLayer_DXR_queue_lock.json
#                              + implicit_layer.d/ twin, inert unless DXR_VK_QUEUE_LOCK=1 (#886)
#     systemd --user units  -> $XDG_CONFIG_HOME/systemd/user/displayxr.{socket,service}
#                              socket enabled + started: displayxr-service is
#                              socket-activated on first use, exits when idle (#1744)
#     GNOME Shell extension -> $XDG_DATA_HOME/gnome-shell/extensions/window-geometry@displayxr.org
#                              and enabled for this user (unless they disabled it)
#     Dashboard menu entry  -> $XDG_DATA_HOME/applications/displayxr-dashboard.desktop
#                              + icons/hicolor/256x256/apps/displayxr-dashboard.png
#
#   sudo ./install.sh --system
#     runtime + plug-in     -> /usr/local/{bin,lib}
#     OpenXR ActiveRuntime  -> /etc/xdg/openxr/1/active_runtime.json
#     DP discovery manifest -> /usr/local/share/displayxr/DisplayProcessors/200-sim-display.json
#     Vulkan layer manifest -> /usr/local/share/vulkan/explicit_layer.d/VkLayer_DXR_queue_lock.json
#                              + implicit_layer.d/ twin, inert unless DXR_VK_QUEUE_LOCK=1 (#886)
#     systemd user units    -> /usr/local/lib/systemd/user/displayxr.{socket,service}
#                              socket enabled for every user (systemctl --global)
#     GNOME Shell extension -> /usr/local/share/gnome-shell/extensions/window-geometry@displayxr.org
#                              + /etc/xdg/autostart entry enabling it once per user at login
#     Dashboard menu entry  -> /usr/local/share/applications/displayxr-dashboard.desktop
#                              + /usr/local/share/icons/hicolor/256x256/apps/displayxr-dashboard.png
#
# The DisplayProcessors directory is a SHARED discovery root: a vendor plug-in
# installer (e.g. Leia SR) drops its own .so + <probe-order>-<id>.json next to
# the sim-display one; lower probe_order wins. Nothing here assumes sim-display
# is the only display processor.
#
# Every install also writes openxr_displayxr.json, a runtime manifest at a
# fixed path ($PREFIX/ user-level, /usr/local/share/displayxr/ for --system). It lets one app use DisplayXR via
# XR_RUNTIME_JSON without it being the ActiveRuntime. On SteamOS that is the
# Steam launch option `XR_RUNTIME_JSON=<that path> %command%`: SteamVR claims
# the user's ActiveRuntime back whenever it starts, so the ActiveRuntime alone
# is not a stable way to select DisplayXR there.
#
# SteamOS (and any other image-based distro with a read-only /usr): only the
# user-level default works. --system is refused there, since /usr/local and
# /etc/xdg are on the read-only OS image or are reset by OS updates.
#
# Flags: --system  system-wide (needs root; not on SteamOS)
#        --no-service  skip the systemd user units (displayxr-service)
#        --no-gnome-extension  skip the GNOME Shell extension

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"

SYSTEM=0
NO_SERVICE=0
NO_GNOME_EXT=0
for arg in "$@"; do
    case "$arg" in
    --system) SYSTEM=1 ;;
    --no-service) NO_SERVICE=1 ;;
    --no-gnome-extension) NO_GNOME_EXT=1 ;;
    *) echo "Unknown option: $arg (supported: --system --no-service --no-gnome-extension)" >&2; exit 2 ;;
    esac
done

[ -f "$HERE/lib/openxr_displayxr.so" ] || {
    echo "error: run install.sh from the unpacked tarball root (lib/openxr_displayxr.so not found)" >&2
    exit 1
}

# --- SteamOS / read-only OS image -------------------------------------------
# DXR_OS_RELEASE overrides the os-release path (CI tests the SteamOS branch in
# a plain container with it).
OS_RELEASE="${DXR_OS_RELEASE:-/etc/os-release}"
IS_STEAMOS=0
if [ -r "$OS_RELEASE" ] && grep -Eq '^(ID|ID_LIKE)=("?)steamos("?)$' "$OS_RELEASE"; then
    IS_STEAMOS=1
fi
if [ "$SYSTEM" = 1 ]; then
    if [ "$IS_STEAMOS" = 1 ]; then
        echo "error: --system is not supported on SteamOS: /usr/local is on the read-only" >&2
        echo "       OS image and OS updates replace it. Run ./install.sh without --system" >&2
        echo "       (a user-level install into ~/.local/share/displayxr, no root needed)." >&2
        exit 1
    fi
    if [ -d /usr/local ] && ! [ -w /usr/local ] && [ "$(id -u)" = 0 ]; then
        echo "error: /usr/local is read-only on this system (an image-based OS?)." >&2
        echo "       Run ./install.sh without --system for a user-level install." >&2
        exit 1
    fi
fi

if [ "$SYSTEM" = 1 ]; then
    [ "$(id -u)" = 0 ] || { echo "error: --system needs root (use sudo)" >&2; exit 1; }
    PREFIX=/usr/local
    OPENXR_CONF_DIR=/etc/xdg/openxr/1
    DP_ROOT=/usr/local/share/displayxr/DisplayProcessors
    EXT_ROOT=/usr/local/share/gnome-shell/extensions
    SHARE_ROOT=/usr/local/share
else
    DATA_ROOT="${XDG_DATA_HOME:-$HOME/.local/share}"
    CONFIG_ROOT="${XDG_CONFIG_HOME:-$HOME/.config}"
    PREFIX="$DATA_ROOT/displayxr"
    OPENXR_CONF_DIR="$CONFIG_ROOT/openxr/1"
    DP_ROOT="$DATA_ROOT/DisplayXR/DisplayProcessors"
    EXT_ROOT="$DATA_ROOT/gnome-shell/extensions"
    SHARE_ROOT="$DATA_ROOT"
fi
EXT_UUID="window-geometry@displayxr.org"

# --- glibc floor (#1656) ---------------------------------------------------
# The tarball is a relocatable binary drop with no dependency metadata, so
# nothing else stops it being unpacked on a distribution older than the one it
# was built on — where every dlopen() of the runtime then fails with
# "GLIBC_x.yz not found", far from here. package_linux.sh records the highest
# glibc symbol version the binaries reference; refuse early and say so.
if [ -f "$HERE/GLIBC_FLOOR" ]; then
    NEED="$(cat "$HERE/GLIBC_FLOOR")"
    HAVE="$(getconf GNU_LIBC_VERSION 2>/dev/null | awk '{print $2}')"
    [ -n "$HAVE" ] || HAVE="$(ldd --version 2>/dev/null | head -1 | awk '{print $NF}')"
    if [ -n "$HAVE" ] && [ "$(printf '%s\n%s\n' "$NEED" "$HAVE" | sort -V | tail -1)" != "$HAVE" ]; then
        echo "error: this build needs glibc >= $NEED but this system has $HAVE." >&2
        echo "       Use a build made for this distribution (the .deb declares the same" >&2
        echo "       floor as a versioned libc6 dependency), or build from source." >&2
        exit 1
    fi
fi

echo "==> Installing DisplayXR runtime to $PREFIX"
# A RUNNING displayxr.service executes from (and has mapped the libraries in)
# the tree we are about to overwrite: copying over it fails ("Text file busy")
# or crashes it, and a survivor would refuse every new-version client at the
# version gate. Stop it first (as root: in every user manager); the socket
# starts the new binary on the next connect. Never starts anything.
if [ "$NO_SERVICE" = 0 ] && [ -x "$HERE/bin/displayxr-restart-service" ]; then
    "$HERE/bin/displayxr-restart-service" stop || true
fi
mkdir -p "$PREFIX"
cp -R "$HERE/bin" "$PREFIX/"
cp -R "$HERE/lib" "$PREFIX/"

# --- OpenXR ActiveRuntime -------------------------------------------------
mkdir -p "$OPENXR_CONF_DIR"
ACTIVE="$OPENXR_CONF_DIR/active_runtime.json"
if [ -f "$ACTIVE" ] && ! grep -q "openxr_displayxr.so" "$ACTIVE"; then
    echo "    (existing non-DisplayXR active runtime backed up to active_runtime.json.bak)"
    cp "$ACTIVE" "$ACTIVE.bak"
fi
cat > "$ACTIVE" <<EOF
{
    "file_format_version": "1.0.0",
    "runtime": {
        "name": "DisplayXR",
        "library_path": "$PREFIX/lib/openxr_displayxr.so"
    }
}
EOF
echo "==> OpenXR ActiveRuntime: $ACTIVE"

# The same manifest at a fixed path in the install tree, for XR_RUNTIME_JSON
# (per-app selection; see the header). uninstall.sh removes it.
if [ "$SYSTEM" = 1 ]; then
    RUNTIME_JSON=/usr/local/share/displayxr/openxr_displayxr.json
else
    RUNTIME_JSON="$PREFIX/openxr_displayxr.json"
fi
mkdir -p "$(dirname "$RUNTIME_JSON")"
cp "$ACTIVE" "$RUNTIME_JSON"
echo "==> Per-app runtime manifest (XR_RUNTIME_JSON): $RUNTIME_JSON"

# --- Display-processor discovery manifest ----------------------------------
mkdir -p "$DP_ROOT"
cat > "$DP_ROOT/200-sim-display.json" <<EOF
{
    "file_format_version": "1.0",
    "plugin": {
        "id":           "sim-display",
        "display_name": "DisplayXR Sim Display",
        "vendor":       "DisplayXR",
        "version":      "$(cat "$HERE/VERSION" 2>/dev/null || echo unknown)",
        "binary_path":  "$PREFIX/lib/displayxr/plugins/DisplayXR-SimDisplay.so",
        "probe_order":  200,
        "sandbox":      {}
    }
}
EOF
echo "==> Display processor manifest: $DP_ROOT/200-sim-display.json"

# --- Reference input + conversion provider manifests (ADR-054 §2 clause 3) --
# Same discovery root; the `-input-provider.json` / `-conversion-provider.json`
# suffixes route each manifest to its class loader. Both providers decline at
# probe until their ENABLE setting is on (`displayxr-cli input set sim-input
# ENABLE 1` / `conv set sim-conversion ENABLE 1`), so this changes nothing for
# a user: qwerty keeps the hand roles and no conversion task moves.
cat > "$DP_ROOT/200-sim-input-input-provider.json" <<EOF
{
    "file_format_version": "1.0",
    "plugin": {
        "id":           "sim-input",
        "display_name": "DisplayXR Sim Input",
        "vendor":       "DisplayXR",
        "version":      "$(cat "$HERE/VERSION" 2>/dev/null || echo unknown)",
        "binary_path":  "$PREFIX/lib/displayxr/plugins/DisplayXR-SimInput.so",
        "probe_order":  200
    }
}
EOF
cat > "$DP_ROOT/250-sim-conversion-conversion-provider.json" <<EOF
{
    "file_format_version": "1.0",
    "plugin": {
        "id":           "sim-conversion",
        "display_name": "DisplayXR Sim Conversion",
        "vendor":       "DisplayXR",
        "version":      "$(cat "$HERE/VERSION" 2>/dev/null || echo unknown)",
        "binary_path":  "$PREFIX/lib/displayxr/plugins/DisplayXR-SimConversion.so",
        "probe_order":  250,
        "packs":        ["$PREFIX/lib/displayxr/plugins/sim-conversion.models.json"]
    }
}
EOF
echo "==> Reference provider manifests: $DP_ROOT/200-sim-input-input-provider.json, $DP_ROOT/250-sim-conversion-conversion-provider.json"

# --- Gamepad input provider (evdev; SteamOS: Steam Input's virtual pad) -----
# Presence-gated: inert until a pad is attached. Registered only when the
# tarball shipped it. `displayxr-cli input disable gamepad` turns it off.
if [ -f "$PREFIX/lib/displayxr/plugins/DisplayXR-Gamepad.so" ]; then
    cat > "$DP_ROOT/150-gamepad-input-provider.json" <<EOF
{
    "file_format_version": "1.0",
    "plugin": {
        "id":           "gamepad",
        "display_name": "DisplayXR Gamepad",
        "vendor":       "DisplayXR",
        "version":      "$(cat "$HERE/VERSION" 2>/dev/null || echo unknown)",
        "binary_path":  "$PREFIX/lib/displayxr/plugins/DisplayXR-Gamepad.so",
        "probe_order":  150
    }
}
EOF
    echo "==> Gamepad provider manifest: $DP_ROOT/150-gamepad-input-provider.json"
fi

# --- VK_LAYER_DXR_queue_lock (#902 / #2036) ---------------------------------
# The Vulkan layer the runtime enables by name at xrCreateVulkanInstanceKHR so
# the #868 repaint can share the app's only graphics queue (Intel and other
# single-graphics-queue GPUs; without it repaint silently stays off). Its
# manifest goes on the loader's DEFAULT explicit-layer path ($XDG_DATA_HOME, or
# /usr/local/share for --system) with an ABSOLUTE library_path into $PREFIX.
VKLAYER_JSON_IN="$HERE/share/vulkan/VkLayer_DXR_queue_lock.json.in"
if [ -f "$PREFIX/lib/libVkLayer_DXR_queue_lock.so" ] && [ -f "$VKLAYER_JSON_IN" ]; then
    mkdir -p "$SHARE_ROOT/vulkan/explicit_layer.d"
    sed "s|@DXR_QL_LIBRARY_PATH@|$PREFIX/lib/libVkLayer_DXR_queue_lock.so|" "$VKLAYER_JSON_IN" \
        >"$SHARE_ROOT/vulkan/explicit_layer.d/VkLayer_DXR_queue_lock.json"
    echo "==> Vulkan layer manifest: $SHARE_ROOT/vulkan/explicit_layer.d/VkLayer_DXR_queue_lock.json"
fi
# #886: the implicit twin for vulkan_enable1 apps (they create their own
# VkInstance, so the runtime cannot inject the layer). Inert unless the app's
# environment has DXR_VK_QUEUE_LOCK=1; DXR_VK_QUEUE_LOCK_DISABLE=1 vetoes it.
VKLAYER_IMPLICIT_JSON_IN="$HERE/share/vulkan/VkLayer_DXR_queue_lock_implicit.json.in"
if [ -f "$PREFIX/lib/libVkLayer_DXR_queue_lock.so" ] && [ -f "$VKLAYER_IMPLICIT_JSON_IN" ]; then
    mkdir -p "$SHARE_ROOT/vulkan/implicit_layer.d"
    sed "s|@DXR_QL_LIBRARY_PATH@|$PREFIX/lib/libVkLayer_DXR_queue_lock.so|" "$VKLAYER_IMPLICIT_JSON_IN" \
        >"$SHARE_ROOT/vulkan/implicit_layer.d/VkLayer_DXR_queue_lock.json"
    echo "==> Vulkan implicit layer manifest (DXR_VK_QUEUE_LOCK=1): $SHARE_ROOT/vulkan/implicit_layer.d/VkLayer_DXR_queue_lock.json"
fi

# --- displayxr-service: socket-activated systemd user units (#1744) --------
# The runtime is HYBRID: ordinary apps run in-process; XR_DXR_weave
# present-owners (the DisplayXR browser) and workspace controllers dial
# $XDG_RUNTIME_DIR/displayxr_comp_ipc. displayxr.socket owns that path, so the
# service starts only when such a client connects, and exits when idle.
# (Replaces v1's always-on displayxr.service, WantedBy=default.target — which
# also could not start at all: the service died on its /dev/null stdin.)
UNIT_SRC="$HERE/share/systemd/user"
if [ "$NO_SERVICE" = 0 ] && [ -x "$PREFIX/bin/displayxr-service" ] &&
    [ -f "$UNIT_SRC/displayxr.socket" ] && [ -f "$UNIT_SRC/displayxr.service.in" ]; then
    if [ "$SYSTEM" = 1 ]; then
        UNIT_DIR=/usr/local/lib/systemd/user
    else
        UNIT_DIR="$CONFIG_ROOT/systemd/user"
        # v1 enabled a plain service into default.target: drop that link.
        if [ -d /run/systemd/system ]; then
            systemctl --user disable displayxr.service >/dev/null 2>&1 || true
        fi
        rm -f "$CONFIG_ROOT/systemd/user/default.target.wants/displayxr.service"
    fi
    mkdir -p "$UNIT_DIR"
    cp "$UNIT_SRC/displayxr.socket" "$UNIT_DIR/displayxr.socket"
    sed "s|@SERVICE_BIN@|$PREFIX/bin/displayxr-service|g" "$UNIT_SRC/displayxr.service.in" \
        >"$UNIT_DIR/displayxr.service"
    echo "==> systemd user units: $UNIT_DIR/displayxr.{socket,service}"
    if [ "$SYSTEM" = 1 ]; then
        if command -v systemctl >/dev/null 2>&1 && systemctl --global enable displayxr.socket >/dev/null 2>&1; then
            echo "    socket enabled for every user from their next login"
            echo "    (now, per user: systemctl --user daemon-reload && systemctl --user start displayxr.socket)"
        else
            echo "    (systemctl unavailable — enable per user: systemctl --user enable --now displayxr.socket)"
        fi
    # Containers / non-systemd sessions have no user manager — skip gracefully.
    elif [ -d /run/systemd/system ] && systemctl --user daemon-reload 2>/dev/null; then
        systemctl --user enable --now displayxr.socket >/dev/null 2>&1 || true
        echo "    socket enabled and listening (the service starts on first use)"
    else
        echo "    (no systemd user session detected — enable later:"
        echo "     systemctl --user daemon-reload && systemctl --user enable --now displayxr.socket)"
    fi
fi

# --- GNOME Shell extension (windowed Wayland weaving + capture exclusion) ----
# Required on GNOME/Wayland for both features; harmless elsewhere (GNOME Shell
# is the only thing that ever loads it). Enabling is per user: the installing
# user here, every user at their next login for --system.
EXT_SRC="$HERE/share/gnome-shell/extensions/$EXT_UUID"
if [ "$NO_GNOME_EXT" = 0 ] && [ -f "$EXT_SRC/metadata.json" ]; then
    mkdir -p "$EXT_ROOT/$EXT_UUID"
    # Both entry-point forms plus the logic they share. Which one ends up in
    # the extension.js slot GNOME reads depends on the running shell and is
    # decided by displayxr-gnome-extension-enable (GNOME 45+ loads an ES
    # module; 40-44, e.g. Ubuntu 22.04's GNOME 42, cannot parse one).
    for f in extension.js extension-gnome42.js lib.js metadata.json; do
        cp "$EXT_SRC/$f" "$EXT_ROOT/$EXT_UUID/"
    done
    echo "==> GNOME Shell extension: $EXT_ROOT/$EXT_UUID"
    if [ "$SYSTEM" = 1 ]; then
        mkdir -p /etc/xdg/autostart
        cat > /etc/xdg/autostart/displayxr-gnome-extension-enable.desktop <<EOF
[Desktop Entry]
Type=Application
Name=DisplayXR GNOME Shell extension
Comment=Enables the DisplayXR window-geometry extension once per user, never over an opt-out
Exec=$PREFIX/bin/displayxr-gnome-extension-enable
TryExec=$PREFIX/bin/displayxr-gnome-extension-enable
OnlyShowIn=GNOME;
NoDisplay=true
X-GNOME-Autostart-enabled=true
EOF
        echo "    enabled for each user at their next GNOME login (not for a user who disabled it)"
    else
        "$PREFIX/bin/displayxr-gnome-extension-enable" --install || true
    fi
    echo "    LOG OUT AND BACK IN for it to load: a Wayland session cannot reload GNOME Shell."
fi

# --- DisplayXR Dashboard: applications-menu entry ----------------------------
# The binary went in with bin/ above, beside the displayxr-cli it runs.
if [ -x "$PREFIX/bin/displayxr-dashboard" ] && [ -f "$HERE/share/applications/displayxr-dashboard.desktop.in" ]; then
    mkdir -p "$SHARE_ROOT/applications" "$SHARE_ROOT/icons/hicolor/256x256/apps"
    sed "s|@DASHBOARD_BIN@|$PREFIX/bin/displayxr-dashboard|g" \
        "$HERE/share/applications/displayxr-dashboard.desktop.in" >"$SHARE_ROOT/applications/displayxr-dashboard.desktop"
    cp "$HERE/share/icons/displayxr-dashboard.png" "$SHARE_ROOT/icons/hicolor/256x256/apps/displayxr-dashboard.png"
    echo "==> DisplayXR Dashboard: $SHARE_ROOT/applications/displayxr-dashboard.desktop"
fi

echo ""
echo "DisplayXR installed. Sanity check:"
echo "    $PREFIX/bin/displayxr-cli selftest"
if [ "$IS_STEAMOS" = 1 ]; then
    echo ""
    echo "SteamOS: SteamVR takes the OpenXR ActiveRuntime back when it starts. To run a"
    echo "Steam game on DisplayXR regardless, set its launch options (Properties > General):"
    echo "    XR_RUNTIME_JSON=$RUNTIME_JSON PRESSURE_VESSEL_FILESYSTEMS_RW=\$XDG_RUNTIME_DIR/displayxr_comp_ipc %command%"
    echo "(64-bit OpenXR games only. Windows games under Proton need more; see"
    echo " docs/roadmap/steamos-support.md, WS6.)"
fi
