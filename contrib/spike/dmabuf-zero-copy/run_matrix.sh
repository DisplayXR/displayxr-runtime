#!/bin/sh
# Run the spike matrix; one log per row in build/logs/, one summary line per row on stdout.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
LOGS="$HERE/build/logs"
mkdir -p "$LOGS"
row() { # name, consumer args, -- , producer args
	name=$1; shift
	"$HERE/run_pair.sh" "$@" >"$LOGS/$name.log" 2>&1
	rc=$?
	c=$(grep -E "consumer: RESULT|producer\[return\]: RESULT" "$LOGS/$name.log" | grep -oE "modifier=[^ ]+ \([^)]*\)|frames=[0-9]+|import_failed=[0-9]+|mismatched_frames=[0-9]+|import_us avg=[0-9]+ p99=[0-9]+|import\+sample_us avg=[0-9]+ p99=[0-9]+|import\+sample\+readback_us avg=[0-9]+ p99=[0-9]+|gpu_sample_us avg=[0-9]+ p99=[0-9]+|fds start=[0-9]+ ready=[0-9]+ peak=[0-9]+ end=[0-9]+|fds ready=[0-9]+ peak=[0-9]+ end=[0-9]+" | tr '\n' ' ')
	p=$(grep -E "producer: RESULT|consumer\[return\]: RESULT" "$LOGS/$name.log" | grep -oE " fps=[0-9.]+|modifier=[^ ]+ \([^)]*\)|fds start=[0-9]+ after_setup=[0-9]+ peak=[0-9]+ end=[0-9]+|fds ready=[0-9]+ peak=[0-9]+ end=[0-9]+" | tr '\n' ' ')
	echo "$name rc=$rc | $c | $p"
}
for fmt in ARGB8888 ABGR8888; do
	for mods in linear auto 0x0100000000000001 0x0100000000000009 0x0100000000000010; do
		row "${fmt}_${mods}_A" -- --format=$fmt --modifiers=$mods --mode=A --frames=600
		row "${fmt}_${mods}_B" -- --format=$fmt --modifiers=$mods --mode=B --frames=120
	done
done
# chromium-exact: the browser's Wayland allocation, byte for byte -- v1 gbm_bo_create_with_modifiers
# (no usage flags) fed the compositor's zwp_linux_dmabuf_v1 v4 default-feedback main-device tranche
# list for the format, unfiltered (it includes DRM_FORMAT_MOD_INVALID here).
WL_ABGR=$("$HERE/build/wl_query" --modlist=ABGR8888)
WL_ARGB=$("$HERE/build/wl_query" --modlist=ARGB8888)
echo "wayland main-device list ABGR8888: $WL_ABGR"
echo "wayland main-device list ARGB8888: $WL_ARGB"
row chromium-exact_ABGR8888_A -- --alloc=chromium --format=ABGR8888 --modifiers=$WL_ABGR --mode=A --frames=600
row chromium-exact_ABGR8888_B -- --alloc=chromium --format=ABGR8888 --modifiers=$WL_ABGR --mode=B --frames=120
row chromium-exact_ARGB8888_A -- --alloc=chromium --format=ARGB8888 --modifiers=$WL_ARGB --mode=A --frames=600
row chromium-exact_ARGB8888_B -- --alloc=chromium --format=ARGB8888 --modifiers=$WL_ARGB --mode=B --frames=120
row chromium-exact_ABGR8888_A_control_glFinish_nofence -- --alloc=chromium --format=ABGR8888 --modifiers=$WL_ABGR --mode=A --frames=600 --no-fence
row chromium-exact_ABGR8888_A_list_without_INVALID -- --alloc=chromium --format=ABGR8888 --modifiers=$(echo "$WL_ABGR" | sed 's/,0x00ffffffffffffff//') --mode=A --frames=600
row chromium-exact_ABGR8888_A_no_modifiers_advertised -- --alloc=chromium --format=ABGR8888 --modifiers=none --mode=A --frames=600
# return path (woven-output direction): Vulkan allocates+exports, GL imports+samples+reads back.
row return_ABGR8888_linear --return-path=linear --format=ABGR8888 --frames=600 -- --return-path
row return_ABGR8888_driverpick_from_wayland_list --return-path=list:$WL_ABGR --format=ABGR8888 --frames=600 -- --return-path
row return_ARGB8888_driverpick_from_wayland_list --return-path=list:$WL_ARGB --format=ARGB8888 --frames=600 -- --return-path
# usage-flag variants (ARGB8888, auto list)
row ARGB8888_auto_rendering_A -- --format=ARGB8888 --modifiers=auto --usage=rendering --mode=A --frames=600
row ARGB8888_linearusage_A -- --format=ARGB8888 --modifiers=none --usage=rendering,linear --mode=A --frames=600
row ARGB8888_nomodapi_A -- --format=ARGB8888 --modifiers=none --usage=rendering,scanout --mode=A --frames=600
row XRGB8888_auto_A -- --format=XRGB8888 --modifiers=auto --mode=A --frames=600
# throughput (unpaced) and cached-import variants
row ARGB8888_auto_A_unpaced -- --format=ARGB8888 --modifiers=auto --mode=A --frames=600 --fps=0
row ARGB8888_auto_A_cached --cache-imports -- --format=ARGB8888 --modifiers=auto --mode=A --frames=600
# negative controls: both MUST show mismatches (proves the check is sensitive)
row NEG_wrong_modifier --force-modifier=0x0100000000000009 -- --format=ARGB8888 --modifiers=0x0100000000000010 --mode=A --frames=60
row NEG_no_release_wait --sparse-verify -- --format=ARGB8888 --modifiers=auto --mode=A --frames=600 --fps=0 --no-release-wait
# probes: multi-memory-plane Intel CCS modifiers (expected to be refused on an Xe2-class GPU)
row PROBE_2plane_4_TILED_MTL_RC_CCS -- --alloc=chromium --format=ABGR8888 --modifiers=0x010000000000000d --frames=10
row PROBE_4_TILED_BMG_CCS -- --alloc=chromium --format=ABGR8888 --modifiers=0x0100000000000011 --frames=10
