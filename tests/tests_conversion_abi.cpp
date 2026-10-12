// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Conversion-provider plug-in ABI (ADR-053): frozen-struct layouts,
 *         ABI values, and the describe_cap contract on a stub provider.
 *
 * What must hold:
 *  - every FROZEN embedded struct keeps its size and every field offset
 *    (changing one is an API major bump), on 64-bit targets;
 *  - the iface vtable keeps its slot order (append-only past destroy);
 *  - capability state 0..7 is numerically XRT_DP_LIFT_MODULE_*, 8..10 new;
 *    the other values shared with xrt_dp_lift.h are equal;
 *  - describe_cap: out == NULL returns the count; index past the end returns
 *    XRT_ERROR_FEATURE_NOT_SUPPORTED with *out untouched; a provider writes
 *    only within the struct_size the runtime set; every capability it returns
 *    is well-formed.
 *
 * Built as C++17 on purpose: the header must compile as C11 (see
 * tests_conv_wire_c.c) and as C++17.
 */

#include "catch_amalgamated.hpp"

#include "xrt/xrt_conversion_plugin.h"
#include "xrt/xrt_dp_lift.h"

#include <cstddef>
#include <cstring>
#include <set>

namespace {

/*
 *
 * Stub provider: two capabilities (VIEWS_MONO stream, SPLATS_MONO job).
 *
 */

struct stub_instance
{
	uint64_t generation = 1;
};

stub_instance g_inst;
const struct xrt_conversion_plugin_host_iface *g_host = nullptr;

constexpr uint32_t kStubCapCount = 2;

void
copy_str(char *dst, size_t cap, const char *src)
{
	size_t n = std::strlen(src);
	if (n >= cap) {
		n = cap - 1;
	}
	std::memcpy(dst, src, n);
	dst[n] = 0;
}

#define STUB_SET(OUT, FIELD, VALUE)                                                                                    \
	do {                                                                                                           \
		if (XRT_CONV_COVERS(OUT, struct xrt_conv_capability, FIELD)) {                                         \
			(OUT)->FIELD = (VALUE);                                                                        \
		}                                                                                                      \
	} while (0)

#define STUB_STR(OUT, FIELD, VALUE)                                                                                    \
	do {                                                                                                           \
		if (XRT_CONV_COVERS(OUT, struct xrt_conv_capability, FIELD)) {                                         \
			copy_str((OUT)->FIELD, sizeof((OUT)->FIELD), (VALUE));                                         \
		}                                                                                                      \
	} while (0)

bool
stub_get_provider_status(struct xrt_conv_provider_status *out)
{
	out->state = 0;
	return true;
}

xrt_result_t
stub_probe(struct xrt_conversion_plugin_instance **out_inst)
{
	*out_inst = reinterpret_cast<struct xrt_conversion_plugin_instance *>(&g_inst);
	return XRT_SUCCESS;
}

xrt_result_t
stub_describe_cap(struct xrt_conversion_plugin_instance *inst,
                  uint32_t index,
                  struct xrt_conv_capability *out,
                  uint32_t *out_count)
{
	(void)inst;
	if (out == nullptr) {
		if (out_count == nullptr) {
			return XRT_ERROR_FEATURE_NOT_SUPPORTED;
		}
		*out_count = kStubCapCount;
		return XRT_SUCCESS;
	}
	if (out_count != nullptr) {
		*out_count = kStubCapCount;
	}
	if (index >= kStubCapCount) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}

	// Write only within the runtime's struct_size: a provider built against
	// a newer header must not scribble past an older runtime's struct.
	if (index == 0) {
		STUB_SET(out, cap_id, 0x10u);
		STUB_SET(out, task, XRT_CONV_TASK_VIEWS_MONO);
		STUB_SET(out, lanes, XRT_CONV_LANE_STREAM);
		STUB_SET(out, input_kinds, XRT_CONV_INPUT_MONO);
		STUB_SET(out, outputs, XRT_CONV_OUT_VIEWS | XRT_CONV_OUT_DEPTH);
		STUB_SET(out, flags, XRT_CONV_CAP_EXPLICIT_VIEWPOINTS | XRT_CONV_CAP_TEMPORAL);
		STUB_SET(out, import_memory_kinds, XRT_CONV_MEM_CPU | XRT_CONV_MEM_WIN32_NT_HANDLE);
		STUB_SET(out, import_sync_kinds, (1u << XRT_CONV_SYNC_NONE) | (1u << XRT_CONV_SYNC_FENCE_NT));
		STUB_SET(out, input_formats, 1u << XRT_CONV_FMT_RGBA8_UNORM);
		STUB_SET(out, depth_output_format, XRT_CONV_FMT_R32_FLOAT);
		STUB_SET(out, min_views, 2u);
		STUB_SET(out, max_views, XRT_CONV_MAX_VIEWS);
		STUB_SET(out, max_streams, 4u);
		STUB_SET(out, max_concurrent, 1u);
		STUB_SET(out, preferred_input_edge, 518u);
		STUB_SET(out, depth_kind, XRT_CONV_DEPTH_AFFINE_INVERSE);
		STUB_SET(out, depth_measure, XRT_CONV_MEASURE_PLANAR);
		STUB_SET(out, intrinsics_source, XRT_CONV_INTRINSICS_NONE);
		STUB_SET(out, licence_class, XRT_CONV_LICENCE_PERMISSIVE);
		STUB_SET(out, commercial_ok, 1u);
		STUB_STR(out, licence_spdx, "MIT");
		STUB_STR(out, model_id, "stub-views");
		STUB_STR(out, model_version, "1.0");
		STUB_STR(out, backend, "cpu-reference");
		STUB_STR(out, display_name, "Stub views");
		STUB_SET(out, device_luid, 0x1234ull);
		STUB_SET(out, est_latency_ms, 16u);
	} else {
		STUB_SET(out, cap_id, 0x20u);
		STUB_SET(out, task, XRT_CONV_TASK_SPLATS_MONO);
		STUB_SET(out, lanes, XRT_CONV_LANE_JOB);
		STUB_SET(out, input_kinds, XRT_CONV_INPUT_MONO);
		STUB_SET(out, outputs, XRT_CONV_OUT_SPLAT_BLOB);
		STUB_SET(out, flags, XRT_CONV_CAP_USES_MODEL_STORE);
		STUB_SET(out, import_memory_kinds, XRT_CONV_MEM_CPU);
		STUB_SET(out, import_sync_kinds, 1u << XRT_CONV_SYNC_NONE);
		STUB_SET(out, input_formats, 1u << XRT_CONV_FMT_RGBA8_SRGB);
		STUB_SET(out, splat_formats, 1u << XRT_CONV_BLOB_SPZ);
		STUB_SET(out, intrinsics_source, XRT_CONV_INTRINSICS_ESTIMATED);
		STUB_SET(out, licence_class, XRT_CONV_LICENCE_RESEARCH_ONLY);
		STUB_SET(out, commercial_ok, 0u);
		STUB_STR(out, licence_spdx, "LicenseRef-stub");
		STUB_STR(out, model_id, "stub-splats");
		STUB_STR(out, display_name, "Stub splats");
		STUB_SET(out, est_latency_ms, 3000u);
	}
	return XRT_SUCCESS;
}

uint64_t
stub_get_generation(struct xrt_conversion_plugin_instance *inst)
{
	return reinterpret_cast<stub_instance *>(inst)->generation;
}

xrt_result_t
stub_get_state(struct xrt_conversion_plugin_instance *inst, uint32_t cap_id, struct xrt_conv_state *out)
{
	(void)inst;
	if (cap_id != 0x10u && cap_id != 0x20u) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	out->state = cap_id == 0x10u ? XRT_CONV_STATE_READY : XRT_CONV_STATE_INSTALLED;
	return XRT_SUCCESS;
}

void
stub_destroy(struct xrt_conversion_plugin_instance *inst)
{
	(void)inst;
}

struct xrt_conversion_plugin_iface g_iface = [] {
	struct xrt_conversion_plugin_iface i;
	std::memset(&i, 0, sizeof(i));
	i.struct_size = (uint32_t)sizeof(i);
	i.id = "stub";
	i.display_name = "Stub conversion provider";
	i.get_provider_status = stub_get_provider_status;
	i.probe = stub_probe;
	i.describe_cap = stub_describe_cap;
	i.get_generation = stub_get_generation;
	i.get_state = stub_get_state;
	i.destroy = stub_destroy;
	return i;
}();

} // namespace

extern "C" xrt_result_t
xrtConversionPluginNegotiate(uint32_t runtime_api_version,
                             const struct xrt_conversion_plugin_host_iface *host,
                             struct xrt_conversion_plugin_iface **out_iface,
                             uint32_t *out_plugin_api_version)
{
	if (out_iface == nullptr || out_plugin_api_version == nullptr) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	*out_plugin_api_version = XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT;
	if (runtime_api_version != XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	g_host = host;
	*out_iface = &g_iface;
	return XRT_SUCCESS;
}

// The exported name and the typedef agree.
static const xrt_conversion_plugin_negotiate_fn_t kNegotiate = xrtConversionPluginNegotiate;


/*
 *
 * Layout.
 *
 */

TEST_CASE("conversion ABI: frozen embedded structs keep their layout", "[conversion_abi]")
{
	STATIC_REQUIRE(sizeof(struct xrt_conv_sync) == 40);
	STATIC_REQUIRE(offsetof(struct xrt_conv_sync, kind) == 0);
	STATIC_REQUIRE(offsetof(struct xrt_conv_sync, reserved_0) == 4);
	STATIC_REQUIRE(offsetof(struct xrt_conv_sync, handle) == 8);
	STATIC_REQUIRE(offsetof(struct xrt_conv_sync, import_key) == 16);
	STATIC_REQUIRE(offsetof(struct xrt_conv_sync, acquire_value) == 24);
	STATIC_REQUIRE(offsetof(struct xrt_conv_sync, release_value) == 32);

	STATIC_REQUIRE(sizeof(struct xrt_conv_depth_info) == 128);
	STATIC_REQUIRE(offsetof(struct xrt_conv_depth_info, kind) == 0);
	STATIC_REQUIRE(offsetof(struct xrt_conv_depth_info, measure) == 4);
	STATIC_REQUIRE(offsetof(struct xrt_conv_depth_info, intrinsics_source) == 8);
	STATIC_REQUIRE(offsetof(struct xrt_conv_depth_info, value_scale) == 12);
	STATIC_REQUIRE(offsetof(struct xrt_conv_depth_info, value_shift) == 16);
	STATIC_REQUIRE(offsetof(struct xrt_conv_depth_info, near_value) == 20);
	STATIC_REQUIRE(offsetof(struct xrt_conv_depth_info, far_value) == 24);
	STATIC_REQUIRE(offsetof(struct xrt_conv_depth_info, invalid_sample) == 28);
	STATIC_REQUIRE(offsetof(struct xrt_conv_depth_info, range_min) == 32);
	STATIC_REQUIRE(offsetof(struct xrt_conv_depth_info, range_max) == 36);
	STATIC_REQUIRE(offsetof(struct xrt_conv_depth_info, range_p05) == 40);
	STATIC_REQUIRE(offsetof(struct xrt_conv_depth_info, range_p95) == 44);
	STATIC_REQUIRE(offsetof(struct xrt_conv_depth_info, fx) == 48);
	STATIC_REQUIRE(offsetof(struct xrt_conv_depth_info, fy) == 52);
	STATIC_REQUIRE(offsetof(struct xrt_conv_depth_info, cx) == 56);
	STATIC_REQUIRE(offsetof(struct xrt_conv_depth_info, cy) == 60);
	STATIC_REQUIRE(offsetof(struct xrt_conv_depth_info, source_width) == 64);
	STATIC_REQUIRE(offsetof(struct xrt_conv_depth_info, source_height) == 68);
	STATIC_REQUIRE(offsetof(struct xrt_conv_depth_info, convergence_value) == 72);
	STATIC_REQUIRE(offsetof(struct xrt_conv_depth_info, suggested_convergence) == 76);
	STATIC_REQUIRE(offsetof(struct xrt_conv_depth_info, suggested_strength) == 80);
	STATIC_REQUIRE(offsetof(struct xrt_conv_depth_info, suggest_flags) == 84);
	STATIC_REQUIRE(offsetof(struct xrt_conv_depth_info, relief_valid) == 88);
	STATIC_REQUIRE(offsetof(struct xrt_conv_depth_info, relief_scale) == 92);
	STATIC_REQUIRE(offsetof(struct xrt_conv_depth_info, relief_offset) == 96);
	STATIC_REQUIRE(offsetof(struct xrt_conv_depth_info, temporal) == 100);
	STATIC_REQUIRE(offsetof(struct xrt_conv_depth_info, same_inference) == 104);
	STATIC_REQUIRE(offsetof(struct xrt_conv_depth_info, has_confidence) == 108);
	STATIC_REQUIRE(offsetof(struct xrt_conv_depth_info, source_time_ns) == 112);
	STATIC_REQUIRE(offsetof(struct xrt_conv_depth_info, provider_frame_id) == 120);

	STATIC_REQUIRE(sizeof(struct xrt_conv_views_info) == 124);
	STATIC_REQUIRE(offsetof(struct xrt_conv_views_info, view_count) == 0);
	STATIC_REQUIRE(offsetof(struct xrt_conv_views_info, layout) == 4);
	STATIC_REQUIRE(offsetof(struct xrt_conv_views_info, reserved_0) == 8);
	STATIC_REQUIRE(offsetof(struct xrt_conv_views_info, eye_xyz) == 12);
	STATIC_REQUIRE(offsetof(struct xrt_conv_views_info, baseline_m) == 108);
	STATIC_REQUIRE(offsetof(struct xrt_conv_views_info, hfov_deg) == 112);
	STATIC_REQUIRE(offsetof(struct xrt_conv_views_info, zero_parallax_disparity) == 116);
	STATIC_REQUIRE(offsetof(struct xrt_conv_views_info, reserved_1) == 120);

	STATIC_REQUIRE(sizeof(struct xrt_conv_splat_info) == 72);
	STATIC_REQUIRE(offsetof(struct xrt_conv_splat_info, blob_format) == 0);
	STATIC_REQUIRE(offsetof(struct xrt_conv_splat_info, coords) == 4);
	STATIC_REQUIRE(offsetof(struct xrt_conv_splat_info, color_space) == 8);
	STATIC_REQUIRE(offsetof(struct xrt_conv_splat_info, metric) == 12);
	STATIC_REQUIRE(offsetof(struct xrt_conv_splat_info, intrinsics_source) == 16);
	STATIC_REQUIRE(offsetof(struct xrt_conv_splat_info, fx) == 20);
	STATIC_REQUIRE(offsetof(struct xrt_conv_splat_info, fy) == 24);
	STATIC_REQUIRE(offsetof(struct xrt_conv_splat_info, cx) == 28);
	STATIC_REQUIRE(offsetof(struct xrt_conv_splat_info, cy) == 32);
	STATIC_REQUIRE(offsetof(struct xrt_conv_splat_info, grid_width) == 36);
	STATIC_REQUIRE(offsetof(struct xrt_conv_splat_info, grid_height) == 40);
	STATIC_REQUIRE(offsetof(struct xrt_conv_splat_info, baseline_m) == 44);
	STATIC_REQUIRE(offsetof(struct xrt_conv_splat_info, focus_z_m) == 48);
	STATIC_REQUIRE(offsetof(struct xrt_conv_splat_info, near_m) == 52);
	STATIC_REQUIRE(offsetof(struct xrt_conv_splat_info, far_m) == 56);
	STATIC_REQUIRE(offsetof(struct xrt_conv_splat_info, reserved_0) == 60);
	STATIC_REQUIRE(offsetof(struct xrt_conv_splat_info, splat_count) == 64);

	STATIC_REQUIRE(sizeof(struct xrt_conv_stereo_calibration) == 32);
	STATIC_REQUIRE(offsetof(struct xrt_conv_stereo_calibration, eye_order) == 0);
	STATIC_REQUIRE(offsetof(struct xrt_conv_stereo_calibration, fx) == 4);
	STATIC_REQUIRE(offsetof(struct xrt_conv_stereo_calibration, fy) == 8);
	STATIC_REQUIRE(offsetof(struct xrt_conv_stereo_calibration, cx) == 12);
	STATIC_REQUIRE(offsetof(struct xrt_conv_stereo_calibration, cy) == 16);
	STATIC_REQUIRE(offsetof(struct xrt_conv_stereo_calibration, baseline_m) == 20);
	STATIC_REQUIRE(offsetof(struct xrt_conv_stereo_calibration, infinity_shift_px) == 24);
	STATIC_REQUIRE(offsetof(struct xrt_conv_stereo_calibration, sbs_packed) == 28);
}

TEST_CASE("conversion ABI: xrt_conv_image layout on 64-bit targets", "[conversion_abi]")
{
	if (sizeof(void *) != 8) {
		SKIP("xrt_conv_image holds cpu_ptr: pinned on 64-bit targets only");
	}
	CHECK(sizeof(struct xrt_conv_image) == 128);
	CHECK(offsetof(struct xrt_conv_image, memory_kind) == 0);
	CHECK(offsetof(struct xrt_conv_image, reserved_0) == 4);
	CHECK(offsetof(struct xrt_conv_image, handle) == 8);
	CHECK(offsetof(struct xrt_conv_image, import_key) == 16);
	CHECK(offsetof(struct xrt_conv_image, cpu_ptr) == 24);
	CHECK(offsetof(struct xrt_conv_image, byte_size) == 32);
	CHECK(offsetof(struct xrt_conv_image, width) == 40);
	CHECK(offsetof(struct xrt_conv_image, height) == 44);
	CHECK(offsetof(struct xrt_conv_image, format) == 48);
	CHECK(offsetof(struct xrt_conv_image, color_space) == 52);
	CHECK(offsetof(struct xrt_conv_image, row_stride) == 56);
	CHECK(offsetof(struct xrt_conv_image, plane1_offset) == 60);
	CHECK(offsetof(struct xrt_conv_image, plane1_stride) == 64);
	CHECK(offsetof(struct xrt_conv_image, array_layer) == 68);
	CHECK(offsetof(struct xrt_conv_image, drm_modifier) == 72);
	CHECK(offsetof(struct xrt_conv_image, adapter_luid) == 80);
	CHECK(offsetof(struct xrt_conv_image, sync) == 88);
}

TEST_CASE("conversion ABI: top-level structs start with struct_size, no hidden padding where pinned",
          "[conversion_abi]")
{
	STATIC_REQUIRE(offsetof(struct xrt_conv_capability, struct_size) == 0);
	STATIC_REQUIRE(offsetof(struct xrt_conv_state, struct_size) == 0);
	STATIC_REQUIRE(offsetof(struct xrt_conv_provider_status, struct_size) == 0);
	STATIC_REQUIRE(offsetof(struct xrt_conv_adapter_hint, struct_size) == 0);
	STATIC_REQUIRE(offsetof(struct xrt_conv_model_location, struct_size) == 0);
	STATIC_REQUIRE(offsetof(struct xrt_conv_blob_sink, struct_size) == 0);

	// The explicit reserved_0 removes the 4 bytes of hidden padding before
	// device_luid that deleting exclusive_group would have left.
	STATIC_REQUIRE(offsetof(struct xrt_conv_capability, reserved_0) == 332);
	STATIC_REQUIRE(offsetof(struct xrt_conv_capability, device_luid) == 336);
	STATIC_REQUIRE(offsetof(struct xrt_conv_state, eta_ns) == 24);
	STATIC_REQUIRE(offsetof(struct xrt_conv_params, stereo) == 184);
}

TEST_CASE("conversion ABI: iface slot order", "[conversion_abi]")
{
	using I = struct xrt_conversion_plugin_iface;
	const size_t p = sizeof(void *);
	CHECK(offsetof(I, id) == 8);
	CHECK(offsetof(I, display_name) == 8 + 1 * p);
	CHECK(offsetof(I, vendor) == 8 + 2 * p);
	CHECK(offsetof(I, version) == 8 + 3 * p);
	CHECK(offsetof(I, get_provider_status) == 8 + 4 * p);
	CHECK(offsetof(I, probe) == 8 + 5 * p);
	CHECK(offsetof(I, describe_cap) == 8 + 6 * p);
	CHECK(offsetof(I, get_generation) == 8 + 7 * p);
	CHECK(offsetof(I, get_state) == 8 + 8 * p);
	CHECK(offsetof(I, warm) == 8 + 9 * p);
	CHECK(offsetof(I, evict) == 8 + 10 * p);
	CHECK(offsetof(I, stream_create) == 8 + 11 * p);
	CHECK(offsetof(I, stream_get_output_desc) == 8 + 12 * p);
	CHECK(offsetof(I, stream_convert) == 8 + 13 * p);
	CHECK(offsetof(I, stream_destroy) == 8 + 14 * p);
	CHECK(offsetof(I, job_execute) == 8 + 15 * p);
	CHECK(offsetof(I, destroy) == 8 + 16 * p);
	CHECK(sizeof(I) == 8 + 17 * p);

	using H = struct xrt_conversion_plugin_host_iface;
	CHECK(offsetof(H, get_path) == 16);
	CHECK(offsetof(H, get_setting) == 16 + 1 * p);
	CHECK(offsetof(H, get_adapter_hint) == 16 + 2 * p);
	CHECK(offsetof(H, model_resolve) == 16 + 3 * p);
	CHECK(offsetof(H, model_request) == 16 + 4 * p);
	CHECK(offsetof(H, notify_changed) == 16 + 5 * p);
	CHECK(offsetof(H, reserved) == 16 + 6 * p);
	CHECK(sizeof(H) == 16 + 16 * p);
}


/*
 *
 * Values.
 *
 */

TEST_CASE("conversion ABI: state 0..7 equal XRT_DP_LIFT_MODULE_*, 8..10 new", "[conversion_abi]")
{
	STATIC_REQUIRE(XRT_CONV_STATE_UNKNOWN == XRT_DP_LIFT_MODULE_UNKNOWN);
	STATIC_REQUIRE(XRT_CONV_STATE_READY == XRT_DP_LIFT_MODULE_READY);
	STATIC_REQUIRE(XRT_CONV_STATE_WARMING == XRT_DP_LIFT_MODULE_ACTIVATING);
	STATIC_REQUIRE(XRT_CONV_STATE_ABSENT == XRT_DP_LIFT_MODULE_ABSENT);
	STATIC_REQUIRE(XRT_CONV_STATE_INCOMPATIBLE == XRT_DP_LIFT_MODULE_TOO_OLD);
	STATIC_REQUIRE(XRT_CONV_STATE_UNLICENSED == XRT_DP_LIFT_MODULE_UNLICENSED);
	STATIC_REQUIRE(XRT_CONV_STATE_FAILED == XRT_DP_LIFT_MODULE_LOAD_FAILED);
	STATIC_REQUIRE(XRT_CONV_STATE_DISABLED == XRT_DP_LIFT_MODULE_DISABLED);
	STATIC_REQUIRE(XRT_CONV_STATE_DISABLED == 7u);
	STATIC_REQUIRE(XRT_CONV_STATE_INSTALLED == 8u);
	STATIC_REQUIRE(XRT_CONV_STATE_DOWNLOADING == 9u);
	STATIC_REQUIRE(XRT_CONV_STATE_EVICTED == 10u);
}

TEST_CASE("conversion ABI: values shared with xrt_dp_lift.h and the frozen vocabulary", "[conversion_abi]")
{
	STATIC_REQUIRE(XRT_CONV_CONTENT_VIDEO == XRT_DP_LIFT_CONTENT_VIDEO);
	STATIC_REQUIRE(XRT_CONV_CONTENT_PHOTO == XRT_DP_LIFT_CONTENT_PHOTO);
	STATIC_REQUIRE(XRT_CONV_BLOB_PLY_3DGS == XRT_DP_LIFT_BLOB_PLY_3DGS);
	STATIC_REQUIRE(XRT_CONV_BLOB_SOG == XRT_DP_LIFT_BLOB_SOG);

	STATIC_REQUIRE(XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT == 1);
	CHECK(std::strcmp(XRT_CONVERSION_PLUGIN_ENTRYPOINT_NAME, "xrtConversionPluginNegotiate") == 0);
	STATIC_REQUIRE(XRT_CONV_MAX_VIEWS == 8);
	STATIC_REQUIRE(XRT_CONV_MAX_CAPABILITIES == 32);
	STATIC_REQUIRE(XRT_CONV_MAX_FRAME_IMAGES == 4);

	STATIC_REQUIRE(XRT_CONV_TASK_DEPTH_MONO == 1u && XRT_CONV_TASK_DEPTH_VIDEO == 2u &&
	               XRT_CONV_TASK_DISPARITY_STEREO == 3u && XRT_CONV_TASK_VIEWS_MONO == 4u &&
	               XRT_CONV_TASK_VIEWS_FROM_DEPTH == 5u && XRT_CONV_TASK_SPLATS_MONO == 6u &&
	               XRT_CONV_TASK_SPLATS_STEREO == 7u && XRT_CONV_TASK_SPLATS_VIDEO == 8u &&
	               XRT_CONV_TASK_INPAINT == 9u && XRT_CONV_TASK_RESERVED_FIRST == 10u);

	STATIC_REQUIRE(XRT_CONV_FAIL_NONE == 0u && XRT_CONV_FAIL_LICENCE == 1u && XRT_CONV_FAIL_LOAD == 2u &&
	               XRT_CONV_FAIL_PLATFORM == 3u && XRT_CONV_FAIL_NETWORK == 4u && XRT_CONV_FAIL_VERSION == 5u &&
	               XRT_CONV_FAIL_RESOURCES == 6u && XRT_CONV_FAIL_MODEL == 7u &&
	               XRT_CONV_FAIL_DEVICE_LOST == 8u && XRT_CONV_FAIL_CRASH_LOOP == 9u);

	STATIC_REQUIRE(XRT_CONV_LICENCE_UNKNOWN == 0u && XRT_CONV_LICENCE_PERMISSIVE == 1u &&
	               XRT_CONV_LICENCE_COMMERCIAL == 2u && XRT_CONV_LICENCE_RESEARCH_ONLY == 3u);

	STATIC_REQUIRE(XRT_CONV_CONVERGENCE_AUTO == 0u && XRT_CONV_CONVERGENCE_DECODED == 1u);

	STATIC_REQUIRE(XRT_CONV_CAP_LANES_CONCURRENT == (1u << 7) && XRT_CONV_CAP_WARM_CONCURRENT == (1u << 8));
	STATIC_REQUIRE(XRT_CONV_FMT_R16_UNORM == 10u);
	STATIC_REQUIRE(XRT_CONV_DEPTH_STEREO_DISPARITY_PX == 6u);
	STATIC_REQUIRE(XRT_CONV_VIEWS_ROW == 0u && XRT_CONV_VIEWS_ARRAY == 1u && XRT_CONV_VIEWS_ATLAS == 2u);
}


/*
 *
 * describe_cap contract on the stub.
 *
 */

namespace {

struct xrt_conversion_plugin_iface *
negotiate_stub()
{
	struct xrt_conversion_plugin_host_iface host;
	std::memset(&host, 0, sizeof(host));
	host.struct_size = (uint32_t)sizeof(host);
	host.host_api_version = XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT;

	struct xrt_conversion_plugin_iface *iface = nullptr;
	uint32_t plugin_version = 0;
	REQUIRE(kNegotiate(XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT, &host, &iface, &plugin_version) ==
	        XRT_SUCCESS);
	REQUIRE(plugin_version == XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT);
	REQUIRE(iface != nullptr);
	return iface;
}

bool
nul_terminated(const char *s, size_t n)
{
	return std::memchr(s, '\0', n) != nullptr;
}

bool
is_power_of_two(uint32_t v)
{
	return v != 0 && (v & (v - 1)) == 0;
}

} // namespace

TEST_CASE("conversion ABI: negotiate + whole-field coverage macros", "[conversion_abi]")
{
	struct xrt_conversion_plugin_iface *iface = negotiate_stub();
	CHECK(iface->struct_size == sizeof(struct xrt_conversion_plugin_iface));
	CHECK(XRT_CONV_IFACE_HAS(iface, describe_cap));
	CHECK(XRT_CONV_IFACE_HAS(iface, get_state));
	CHECK_FALSE(XRT_CONV_IFACE_HAS(iface, warm)); // the stub leaves it NULL

	// An older provider whose struct_size ends half-way through a slot does
	// not cover it.
	struct xrt_conversion_plugin_iface older = *iface;
	older.struct_size = (uint32_t)(offsetof(struct xrt_conversion_plugin_iface, get_state) + 4);
	CHECK(XRT_CONV_IFACE_HAS(&older, get_generation));
	CHECK_FALSE(XRT_CONV_IFACE_HAS(&older, get_state));

	struct xrt_conversion_plugin_iface *null_iface = nullptr;
	CHECK_FALSE(XRT_CONV_IFACE_HAS(null_iface, describe_cap));

	// A runtime of another major is refused.
	struct xrt_conversion_plugin_iface *other = nullptr;
	uint32_t v = 0;
	CHECK(kNegotiate(XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT + 1, nullptr, &other, &v) != XRT_SUCCESS);
	CHECK(other == nullptr);
}

TEST_CASE("conversion ABI: describe_cap contract", "[conversion_abi]")
{
	struct xrt_conversion_plugin_iface *iface = negotiate_stub();
	struct xrt_conversion_plugin_instance *inst = nullptr;
	REQUIRE(iface->probe(&inst) == XRT_SUCCESS);

	const uint64_t gen_before = iface->get_generation(inst);

	// out == NULL: the count, index ignored.
	uint32_t count = 0xFFFFFFFFu;
	REQUIRE(iface->describe_cap(inst, 12345u, nullptr, &count) == XRT_SUCCESS);
	REQUIRE(count == kStubCapCount);
	REQUIRE(count <= XRT_CONV_MAX_CAPABILITIES);

	std::set<uint32_t> ids;
	for (uint32_t i = 0; i < count; i++) {
		struct xrt_conv_capability cap;
		std::memset(&cap, 0, sizeof(cap));
		cap.struct_size = (uint32_t)sizeof(cap);
		uint32_t n = 0;
		REQUIRE(iface->describe_cap(inst, i, &cap, &n) == XRT_SUCCESS);
		CHECK(n == count);
		CHECK(cap.struct_size == sizeof(cap)); // the provider never rewrites it

		INFO("capability " << i);
		// Identity: non-zero, unique.
		CHECK(cap.cap_id != 0);
		CHECK(ids.insert(cap.cap_id).second);
		// A real, non-reserved task.
		CHECK(cap.task != XRT_CONV_TASK_NONE);
		CHECK(cap.task < XRT_CONV_TASK_RESERVED_FIRST);
		CHECK(cap.task != XRT_CONV_TASK_SPLATS_VIDEO);
		CHECK(cap.task != XRT_CONV_TASK_INPAINT);
		// At least one lane, no unknown lane bits.
		CHECK(cap.lanes != 0);
		CHECK((cap.lanes & ~(XRT_CONV_LANE_STREAM | XRT_CONV_LANE_JOB)) == 0);
		CHECK(cap.input_kinds != 0);
		CHECK((cap.input_kinds & XRT_CONV_INPUT_ENCODED) == 0); // reserved in v1
		CHECK(cap.outputs != 0);
		// Every capability imports CPU memory (hosting rule).
		CHECK((cap.import_memory_kinds & XRT_CONV_MEM_CPU) != 0);
		CHECK(cap.licence_class <= XRT_CONV_LICENCE_RESEARCH_ONLY);
		CHECK(cap.commercial_ok <= 1u);
		if (cap.licence_class == XRT_CONV_LICENCE_RESEARCH_ONLY || cap.licence_class == XRT_CONV_LICENCE_UNKNOWN) {
			CHECK(cap.commercial_ok == 0u);
		}
		// VIEWS_* must honour explicit viewpoints and declare a sane view range.
		if (cap.task == XRT_CONV_TASK_VIEWS_MONO || cap.task == XRT_CONV_TASK_VIEWS_FROM_DEPTH) {
			CHECK((cap.flags & XRT_CONV_CAP_EXPLICIT_VIEWPOINTS) != 0);
			CHECK(cap.min_views >= 2u);
			CHECK(cap.min_views <= cap.max_views);
			CHECK(cap.max_views <= XRT_CONV_MAX_VIEWS);
			CHECK((cap.outputs & XRT_CONV_OUT_VIEWS) != 0);
		}
		if ((cap.outputs & XRT_CONV_OUT_DEPTH) != 0) {
			CHECK(cap.depth_kind != XRT_CONV_DEPTH_UNKNOWN);
			CHECK((cap.depth_output_format == XRT_CONV_FMT_R32_FLOAT ||
			       cap.depth_output_format == XRT_CONV_FMT_R16_FLOAT ||
			       cap.depth_output_format == XRT_CONV_FMT_R8_UNORM));
		}
		if ((cap.outputs & XRT_CONV_OUT_SPLAT_BLOB) != 0) {
			CHECK((cap.lanes & XRT_CONV_LANE_JOB) != 0);
			CHECK(cap.splat_formats != 0);
		}
		// Strings are NUL-terminated inside their arrays.
		CHECK(nul_terminated(cap.licence_spdx, sizeof(cap.licence_spdx)));
		CHECK(nul_terminated(cap.model_id, sizeof(cap.model_id)));
		CHECK(nul_terminated(cap.model_version, sizeof(cap.model_version)));
		CHECK(nul_terminated(cap.backend, sizeof(cap.backend)));
		CHECK(nul_terminated(cap.display_name, sizeof(cap.display_name)));
		CHECK(cap.reserved_0 == 0u);

		// Readiness answers for every described cap_id.
		struct xrt_conv_state st;
		std::memset(&st, 0, sizeof(st));
		st.struct_size = (uint32_t)sizeof(st);
		REQUIRE(iface->get_state(inst, cap.cap_id, &st) == XRT_SUCCESS);
		CHECK(st.state <= XRT_CONV_STATE_EVICTED);
	}

	// index >= count: the documented error, *out untouched.
	struct xrt_conv_capability past;
	std::memset(&past, 0xA5, sizeof(past));
	past.struct_size = (uint32_t)sizeof(past);
	struct xrt_conv_capability before = past;
	CHECK(iface->describe_cap(inst, count, &past, nullptr) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
	CHECK(iface->describe_cap(inst, count + 7u, &past, nullptr) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
	CHECK(std::memcmp(&past, &before, sizeof(past)) == 0);

	// The walk is a snapshot of one generation.
	CHECK(iface->get_generation(inst) == gen_before);
}

TEST_CASE("conversion ABI: a provider writes only within the runtime's struct_size", "[conversion_abi]")
{
	struct xrt_conversion_plugin_iface *iface = negotiate_stub();
	struct xrt_conversion_plugin_instance *inst = nullptr;
	REQUIRE(iface->probe(&inst) == XRT_SUCCESS);

	// An older runtime whose capability ended right before device_luid.
	const uint32_t older = (uint32_t)offsetof(struct xrt_conv_capability, reserved_0);
	struct xrt_conv_capability cap;
	std::memset(&cap, 0, sizeof(cap));
	cap.struct_size = older;
	REQUIRE(iface->describe_cap(inst, 0, &cap, nullptr) == XRT_SUCCESS);

	CHECK(cap.task == XRT_CONV_TASK_VIEWS_MONO);
	CHECK(std::strcmp(cap.display_name, "Stub views") == 0);
	const unsigned char *bytes = reinterpret_cast<const unsigned char *>(&cap);
	size_t nonzero = 0;
	for (size_t i = older; i < sizeof(cap); i++) {
		nonzero += bytes[i] != 0 ? 1u : 0u;
	}
	CHECK(nonzero == 0u);
	CHECK(cap.device_luid == 0u);
	CHECK(cap.est_latency_ms == 0u);
}

TEST_CASE("conversion ABI: memory kinds are single bits, sync kinds index masks", "[conversion_abi]")
{
	const uint32_t mems[] = {XRT_CONV_MEM_CPU,          XRT_CONV_MEM_WIN32_NT_HANDLE, XRT_CONV_MEM_WIN32_KMT_HANDLE,
	                         XRT_CONV_MEM_VK_OPAQUE_FD, XRT_CONV_MEM_VK_OPAQUE_WIN32, XRT_CONV_MEM_DMABUF,
	                         XRT_CONV_MEM_IOSURFACE,    XRT_CONV_MEM_AHARDWAREBUFFER};
	uint32_t all = 0;
	for (uint32_t m : mems) {
		CHECK(is_power_of_two(m));
		CHECK((all & m) == 0);
		all |= m;
	}
	STATIC_REQUIRE(XRT_CONV_SYNC_MTL_SHARED_EVENT < 32u);
	STATIC_REQUIRE(XRT_CONV_FMT_R16_UNORM < 32u);
}
