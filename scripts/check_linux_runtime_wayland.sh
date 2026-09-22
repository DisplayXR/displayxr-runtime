#!/usr/bin/env bash
# Copyright 2026, DisplayXR
# SPDX-License-Identifier: BSL-1.0
#
# Assert that a built desktop-Linux runtime (openxr_displayxr.so) carries the
# native-Wayland present path. Called by the release packaging scripts
# (package_deb_linux.sh, package_linux.sh) so an artefact built without it
# fails the job instead of shipping.
#
# Why strings and not NEEDED: the Wayland path makes no libwayland-client call —
# the app owns the connection and the runtime only calls
# vkCreateWaylandSurfaceKHR through the Vulkan loader — so the linker drops
# libwayland-client and `objdump -p` cannot see the difference. What does
# differ is the data the path compiles in:
#
#   XR_DXR_wayland_surface_binding  the OpenXR extension the runtime advertises
#   VK_KHR_wayland_surface          the instance extension it requests
#                                   (vulkan_enable2) and returns
#                                   (vulkan_enable1)
#
# The second one is exactly what was missing when the runtime could not create
# a Wayland surface (#1561); the build also refuses at compile time if the
# define that produces it goes missing (oxr_vulkan.c, comp_vk_glue.c).
#
# Usage: check_linux_runtime_wayland.sh <path/to/openxr_displayxr.so>
set -euo pipefail

SO="${1:-}"
if [ -z "$SO" ] || [ ! -f "$SO" ]; then
    echo "error: check_linux_runtime_wayland.sh: no runtime at '${SO}'" >&2
    exit 2
fi

# Captured first, and searched via a here-string: `x | grep -q` under pipefail
# fails on SIGPIPE when grep exits at the first match.
STRS="$(strings -a "$SO")"
missing=()
for s in XR_DXR_wayland_surface_binding VK_KHR_wayland_surface; do
    if ! grep -qw -- "$s" <<<"$STRS"; then
        missing+=("$s")
    fi
done

if [ "${#missing[@]}" -ne 0 ]; then
    echo "error: $SO has no native-Wayland present path (missing: ${missing[*]})." >&2
    echo "       XRT_HAVE_WAYLAND was OFF at configure time — install libwayland-dev and rebuild" >&2
    echo "       (the CMake summary prints 'WAYLAND: OFF' and a WARNING when this happens)." >&2
    exit 1
fi
echo "==> $SO: native-Wayland present path present (XR_DXR_wayland_surface_binding, VK_KHR_wayland_surface)"
