#!/usr/bin/env bash
# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
#
# #886 — VK_LAYER_DXR_queue_lock activation routes through the REAL Vulkan
# loader, with the dev-tree explicit AND implicit manifests installed side by
# side exactly as the .deb installs them (same layer name in both dirs):
#
#   * the runtime's enable2 injection (layer NAMED) still works with the
#     implicit twin installed and its env unset — the regression that would
#     cost every Unity app its repaint;
#   * DXR_VK_QUEUE_LOCK=1 alone puts the layer in an app's chain (the
#     vulkan_enable1 route), reported as IMPLICIT;
#   * named + DXR_VK_QUEUE_LOCK=1 loads it ONCE (a double chain would deadlock
#     on its own per-queue mutex at the probe's vkQueueSubmit) and reports
#     NAMED — what an app that enables it itself looks like;
#   * DXR_VK_QUEUE_LOCK=0 / unset, or DXR_VK_QUEUE_LOCK_DISABLE=1, keep the
#     implicit layer out; VK_INSTANCE_LAYERS reports LOADER_ENV.
#
# Isolation without the newer loaders' VK_ADD_* variables (Ubuntu 22.04's
# loader predates them): XDG_DATA_HOME points at a scratch tree holding the two
# manifests, XDG_DATA_DIRS / XDG_CONFIG_* at an empty dir so an INSTALLED
# DisplayXR manifest cannot shadow the dev one, and the driver manifests are
# passed explicitly (VK_ICD_FILENAMES, honoured by every loader).
#
# Usage: tests_vk_queue_lock_activation.sh <probe> <layer-build-dir>
# Exit 77 (SKIP) without a Vulkan driver.
set -u
PROBE="$1"
LAYER_DIR="$2"

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/data/vulkan/explicit_layer.d" "$T/data/vulkan/implicit_layer.d" "$T/empty"
cp "$LAYER_DIR/VkLayer_DXR_queue_lock.json" "$T/data/vulkan/explicit_layer.d/" || exit 1
cp "$LAYER_DIR/implicit/VkLayer_DXR_queue_lock.json" "$T/data/vulkan/implicit_layer.d/" || exit 1
grep -q '"enable_environment"' "$T/data/vulkan/implicit_layer.d/VkLayer_DXR_queue_lock.json" || {
	echo "FAIL: the implicit manifest has no enable_environment" >&2
	exit 1
}

# Prefer lavapipe (no real GPU touched); else every installed driver.
ICDS=""
for d in /usr/share/vulkan/icd.d /etc/vulkan/icd.d /usr/local/share/vulkan/icd.d; do
	[ -f "$d/lvp_icd.json" ] && { ICDS="$d/lvp_icd.json"; break; }
done
if [ -z "$ICDS" ]; then
	ICDS="$(ls /usr/share/vulkan/icd.d/*.json /etc/vulkan/icd.d/*.json 2>/dev/null | paste -sd: -)"
fi
[ -n "$ICDS" ] || { echo "SKIP: no Vulkan driver manifest"; exit 77; }

base=(env -u DXR_VK_QUEUE_LOCK -u DXR_VK_QUEUE_LOCK_DISABLE -u VK_INSTANCE_LAYERS -u VK_LOADER_LAYERS_ENABLE
	-u VK_LOADER_LAYERS_DISABLE -u VK_LAYER_PATH -u VK_ADD_LAYER_PATH -u VK_IMPLICIT_LAYER_PATH
	-u VK_ADD_IMPLICIT_LAYER_PATH -u VK_DRIVER_FILES
	XDG_DATA_HOME="$T/data" XDG_DATA_DIRS="$T/empty" XDG_CONFIG_HOME="$T/empty" XDG_CONFIG_DIRS="$T/empty"
	VK_ICD_FILENAMES="$ICDS")

fail=0
check() { # <label> <expected marker> <expected activation> -- env/args...
	local label="$1" want_m="$2" want_a="$3"
	shift 3
	local out rc
	out="$(timeout 30 "${base[@]}" "$@" 2>/dev/null | tail -1)"
	rc=${PIPESTATUS[0]}
	if [ "$rc" = 77 ]; then
		echo "SKIP: $out"
		exit 77
	fi
	local want="marker=$want_m activation=$want_a"
	if [ "$rc" = 0 ] && [[ "$out" == *"$want"* ]]; then
		echo "ok   $label: $out"
	else
		echo "FAIL $label: rc=$rc got '$out', want '$want'" >&2
		fail=1
	fi
}

check "enable2 injection, implicit twin installed, env unset" yes 1 "$PROBE" named
check "enable1 app, DXR_VK_QUEUE_LOCK=1 (implicit)" yes 2 DXR_VK_QUEUE_LOCK=1 "$PROBE"
check "app names it AND DXR_VK_QUEUE_LOCK=1 (loaded once)" yes 1 DXR_VK_QUEUE_LOCK=1 "$PROBE" named
check "enable1 app, env unset (implicit stays out)" no 0 "$PROBE"
check "enable1 app, DXR_VK_QUEUE_LOCK=0" no 0 DXR_VK_QUEUE_LOCK=0 "$PROBE"
check "DXR_VK_QUEUE_LOCK=1 + DXR_VK_QUEUE_LOCK_DISABLE=1" no 0 DXR_VK_QUEUE_LOCK=1 DXR_VK_QUEUE_LOCK_DISABLE=1 "$PROBE"
check "VK_INSTANCE_LAYERS forces it (loader env)" yes 3 VK_INSTANCE_LAYERS=VK_LAYER_DXR_queue_lock "$PROBE"
exit $fail
