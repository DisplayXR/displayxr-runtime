// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Compile-time sanity stub for the vendor plug-in ABI.
 *
 * Includes `xrt/xrt_plugin.h` so the header is exercised on every build
 * and asserts a small set of structural invariants the runtime side will
 * rely on when registry enumeration / probe-loop code lands (Step 5 of
 * `docs/roadmap/vendor-plugin-architecture.md`). No runtime behavior —
 * the file contains only `_Static_assert`s.
 *
 * Will be replaced by the real plug-in discovery + probe loop in a
 * later step. Keeping it as its own TU now means:
 *   1. The header parses cleanly under MSVC / clang / GCC on every CI
 *      configuration without a hypothetical future TU as a leak detector.
 *   2. The struct-layout asserts catch any unintended field reorder
 *      before the runtime grows code that depends on the layout.
 *
 * @ingroup oxr_main
 */

#include "xrt/xrt_plugin.h"
#include "xrt/xrt_dp_lift.h"
#include "xrt/xrt_conversion_plugin.h"

#include <openxr/openxr.h>
#include <openxr/XR_DXR_lift.h>

#include <stddef.h>


/*
 * Layout asserts. If any of these fire, the cause is a structural change
 * to xrt_plugin.h that needs an XRT_PLUGIN_API_VERSION_* bump.
 */

/* The entry-point symbol the runtime resolves at LoadLibrary time. */
_Static_assert(sizeof(XRT_PLUGIN_ENTRYPOINT_NAME) == sizeof("xrtPluginNegotiate"),
               "xrt_plugin: entry-point symbol name changed; coordinate with all plug-in builds");

/* Current API version is the v5 line (#757 vk_bundle ABI-parity layout +
 * Linux dma-buf device-extension guarantee) — bump deliberately, not by
 * accident. */
_Static_assert(XRT_PLUGIN_API_VERSION_CURRENT == XRT_PLUGIN_API_VERSION_5,
               "xrt_plugin: API version current/v5 drift");

/*
 * Host iface layout. struct_size must be the first field so plug-ins can
 * always read it regardless of API version, and host_api_version sits
 * immediately after for the cheap version check.
 */
_Static_assert(offsetof(struct xrt_plugin_host_iface, struct_size) == 0,
               "xrt_plugin: host_iface.struct_size must be the first field");
_Static_assert(offsetof(struct xrt_plugin_host_iface, host_api_version) == sizeof(uint32_t),
               "xrt_plugin: host_iface.host_api_version must follow struct_size");

/*
 * Plug-in iface layout. struct_size first, then id/display_name pointers
 * for the runtime's logged-line "found plug-in <id> (<display_name>)"
 * trace at probe time.
 */
_Static_assert(offsetof(struct xrt_plugin_iface, struct_size) == 0,
               "xrt_plugin: iface.struct_size must be the first field");
_Static_assert(offsetof(struct xrt_plugin_iface, id) < offsetof(struct xrt_plugin_iface, probe),
               "xrt_plugin: iface.id must precede the probe function pointer");
_Static_assert(offsetof(struct xrt_plugin_iface, display_name) < offsetof(struct xrt_plugin_iface, probe),
               "xrt_plugin: iface.display_name must precede the probe function pointer");

/*
 * v1 commits to having all five per-API DP factories present (each may
 * be NULL at runtime if the plug-in does not support that API). Assert
 * they are all there so plug-ins can rely on the layout.
 */
_Static_assert(offsetof(struct xrt_plugin_iface, create_dp_vk) < offsetof(struct xrt_plugin_iface, destroy),
               "xrt_plugin: create_dp_vk must precede destroy");
_Static_assert(offsetof(struct xrt_plugin_iface, create_dp_d3d11) < offsetof(struct xrt_plugin_iface, destroy),
               "xrt_plugin: create_dp_d3d11 must precede destroy");
_Static_assert(offsetof(struct xrt_plugin_iface, create_dp_d3d12) < offsetof(struct xrt_plugin_iface, destroy),
               "xrt_plugin: create_dp_d3d12 must precede destroy");
_Static_assert(offsetof(struct xrt_plugin_iface, create_dp_gl) < offsetof(struct xrt_plugin_iface, destroy),
               "xrt_plugin: create_dp_gl must precede destroy");
_Static_assert(offsetof(struct xrt_plugin_iface, create_dp_metal) < offsetof(struct xrt_plugin_iface, destroy),
               "xrt_plugin: create_dp_metal must precede destroy");

/*
 * get_display_info is the first additive vtable field after the v1
 * "core" methods. New extensions go after it; this assert keeps
 * destroy as the boundary between the v1 core and the appended
 * surface so plug-ins built against the bare v1 header keep working
 * (their struct_size won't include get_display_info; the runtime
 * checks via struct_size before dereferencing).
 */
_Static_assert(offsetof(struct xrt_plugin_iface, destroy) < offsetof(struct xrt_plugin_iface, get_display_info),
               "xrt_plugin: get_display_info must follow destroy (additive-only growth)");
_Static_assert(offsetof(struct xrt_plugin_iface, get_display_info) < offsetof(struct xrt_plugin_iface, set_pose_source),
               "xrt_plugin: set_pose_source must follow get_display_info (additive-only growth)");

/*
 * probe_displays (#69 / ADR-015) is the next additive vtable field after
 * set_pose_source. Appended at the end, NULL-safe, struct_size-guarded —
 * no API version bump.
 */
_Static_assert(offsetof(struct xrt_plugin_iface, set_pose_source) < offsetof(struct xrt_plugin_iface, probe_displays),
               "xrt_plugin: probe_displays must follow set_pose_source (additive-only growth)");

/*
 * Display-info struct's first field must be struct_size so plug-ins
 * can detect host's compile-time sizeof and clamp writes accordingly.
 */
_Static_assert(offsetof(struct xrt_plugin_display_info, struct_size) == 0,
               "xrt_plugin: display_info.struct_size must be the first field");

/*
 * The display-descriptor the runtime hands probe_displays() is likewise
 * struct_size-headed so plug-ins can clamp their reads for forward compat.
 */
_Static_assert(offsetof(struct xrt_display_descriptor, struct_size) == 0,
               "xrt_plugin: display_descriptor.struct_size must be the first field");


/*
 * ADR-053 gates (conversion-provider-discovery.md §8 row 1; #2006). The
 * conversion-provider ABI restates a few legacy value spaces so the dp-lift
 * adapter can copy them verbatim (§4.5: "state is a plain copy") and so a
 * provider's status reads like a display plug-in's. Every pair that MUST stay
 * numerically equal is pinned here, on every build.
 *
 * Every XR_DXR_lift spec-4 app enum with a plug-in counterpart uses the
 * plug-in's values (no boundary needs a hand-written mapping); the pairs are
 * asserted at the end of this file.
 */

/* Entry symbol + ABI line. */
_Static_assert(sizeof(XRT_CONVERSION_PLUGIN_ENTRYPOINT_NAME) == sizeof("xrtConversionPluginNegotiate"),
               "xrt_conversion_plugin: entry-point symbol name changed");
_Static_assert(XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT == XRT_CONVERSION_PLUGIN_API_VERSION_1,
               "xrt_conversion_plugin: API version current/v1 drift");

/* ADR-057 S1: the splat result is a chained top-level struct (the frozen
 * xrt_conv_splat_info never grows); the job I/O and the host iface grew only
 * at their tails / out of reserved[]. */
_Static_assert(XRT_CONVERSION_PLUGIN_HAS_SPLAT_RESULT == 1, "ADR-057 feature macro");
_Static_assert(sizeof(struct xrt_conv_splat_result) == 568, "xrt_conv_splat_result layout changed - append only");
_Static_assert(offsetof(struct xrt_conv_job_out, splat_result) == 336,
               "xrt_conv_job_out: splat_result appended at 336");
_Static_assert(XRT_CONV_FAIL_INPUT == 11u && XRT_CONV_CAP_REMOTE == (1u << 9) && XRT_CONV_CONSENT_OFFBOX == 1u &&
                   XRT_CONV_REFUSAL_REMOTE_ERROR == 6u,
               "ADR-057 values are ABI");

/* Capability state 0..7 == the legacy module status (xrt_dp_lift.h). */
_Static_assert(XRT_CONV_STATE_UNKNOWN == XRT_DP_LIFT_MODULE_UNKNOWN, "conv state UNKNOWN != DP lift module UNKNOWN");
_Static_assert(XRT_CONV_STATE_READY == XRT_DP_LIFT_MODULE_READY, "conv state READY != DP lift module READY");
_Static_assert(XRT_CONV_STATE_WARMING == XRT_DP_LIFT_MODULE_ACTIVATING,
               "conv state WARMING != DP lift module ACTIVATING");
_Static_assert(XRT_CONV_STATE_ABSENT == XRT_DP_LIFT_MODULE_ABSENT, "conv state ABSENT != DP lift module ABSENT");
_Static_assert(XRT_CONV_STATE_INCOMPATIBLE == XRT_DP_LIFT_MODULE_TOO_OLD,
               "conv state INCOMPATIBLE != DP lift module TOO_OLD");
_Static_assert(XRT_CONV_STATE_UNLICENSED == XRT_DP_LIFT_MODULE_UNLICENSED,
               "conv state UNLICENSED != DP lift module UNLICENSED");
_Static_assert(XRT_CONV_STATE_FAILED == XRT_DP_LIFT_MODULE_LOAD_FAILED,
               "conv state FAILED != DP lift module LOAD_FAILED");
_Static_assert(XRT_CONV_STATE_DISABLED == XRT_DP_LIFT_MODULE_DISABLED,
               "conv state DISABLED != DP lift module DISABLED");

/* Parameter value spaces shared with xrt_dp_lift_params / _stream_info. */
_Static_assert(XRT_CONV_CONTENT_VIDEO == XRT_DP_LIFT_CONTENT_VIDEO, "conv CONTENT_VIDEO parity");
_Static_assert(XRT_CONV_CONTENT_PHOTO == XRT_DP_LIFT_CONTENT_PHOTO, "conv CONTENT_PHOTO parity");
_Static_assert(XRT_CONV_VIEWPOINT_FRAME_DISPLAY == XRT_DP_LIFT_VIEWPOINT_FRAME_DISPLAY, "conv FRAME_DISPLAY parity");
_Static_assert(XRT_CONV_VIEWPOINT_FRAME_RECT == XRT_DP_LIFT_VIEWPOINT_FRAME_RECT, "conv FRAME_RECT parity");
_Static_assert(XRT_CONV_AXIS_X == XRT_DP_LIFT_AXIS_X, "conv AXIS_X parity");
_Static_assert(XRT_CONV_AXIS_XY == XRT_DP_LIFT_AXIS_XY, "conv AXIS_XY parity");
_Static_assert(XRT_CONV_AXIS_XYZ == XRT_DP_LIFT_AXIS_XYZ, "conv AXIS_XYZ parity");
_Static_assert(XRT_CONV_VIEWPOINTS_TRACKED == XRT_DP_LIFT_VIEWPOINTS_TRACKED, "conv VIEWPOINTS_TRACKED parity");
_Static_assert(XRT_CONV_VIEWPOINTS_EXPLICIT == XRT_DP_LIFT_VIEWPOINTS_EXPLICIT, "conv VIEWPOINTS_EXPLICIT parity");
_Static_assert(XRT_CONV_VIEWPOINTS_DISPLAY_RIG == XRT_DP_LIFT_VIEWPOINTS_DISPLAY_RIG,
               "conv VIEWPOINTS_DISPLAY_RIG parity");
_Static_assert(XRT_CONV_VIEWPOINTS_CAMERA_RIG == XRT_DP_LIFT_VIEWPOINTS_CAMERA_RIG,
               "conv VIEWPOINTS_CAMERA_RIG parity");
_Static_assert(XRT_CONV_BLOB_PLY_3DGS == XRT_DP_LIFT_BLOB_PLY_3DGS, "conv BLOB_PLY_3DGS parity");
_Static_assert(XRT_CONV_BLOB_SOG == XRT_DP_LIFT_BLOB_SOG, "conv BLOB_SOG parity");

/* xrt_conv_provider_status restates xrt_plugin_platform_status (layout + state values). */
/* The conversion struct restates the platform status WITHOUT its trailing
 * `uint64_t reserved[8]`, so it is pinned as an exact prefix of it. */
_Static_assert(sizeof(struct xrt_conv_provider_status) == offsetof(struct xrt_plugin_platform_status, reserved),
               "conv provider status != the prefix of plug-in platform status (before reserved[])");
_Static_assert(offsetof(struct xrt_conv_provider_status, state) ==
                   offsetof(struct xrt_plugin_platform_status, state),
               "conv provider status: state offset");
_Static_assert(offsetof(struct xrt_conv_provider_status, flags) ==
                   offsetof(struct xrt_plugin_platform_status, flags),
               "conv provider status: flags offset");
_Static_assert(offsetof(struct xrt_conv_provider_status, hint) == offsetof(struct xrt_plugin_platform_status, hint),
               "conv provider status: hint offset");
_Static_assert(sizeof(((struct xrt_conv_provider_status *)0)->hint) == XRT_PLUGIN_PLATFORM_HINT_MAX,
               "conv provider status: hint size");
_Static_assert(XRT_CONV_PROVIDER_FLAG_FALLBACK == XRT_PLUGIN_PLATFORM_FLAG_FALLBACK,
               "conv provider FALLBACK flag != plug-in platform FALLBACK flag");


/*
 * XR_DXR_lift spec 4: app enum == plug-in ABI value, pair by pair (ADR-053).
 * Input kinds are values in the app API and bits in the ABI:
 * (1 << XR value) == (ABI bit << 1).
 */

_Static_assert((uint32_t)XR_LIFT_TASK_DEPTH_MONO_DXR == (uint32_t)XRT_CONV_TASK_DEPTH_MONO,
               "XR_LIFT_TASK_DEPTH_MONO_DXR != XRT_CONV_TASK_DEPTH_MONO");
_Static_assert((uint32_t)XR_LIFT_TASK_DEPTH_VIDEO_DXR == (uint32_t)XRT_CONV_TASK_DEPTH_VIDEO,
               "XR_LIFT_TASK_DEPTH_VIDEO_DXR != XRT_CONV_TASK_DEPTH_VIDEO");
_Static_assert((uint32_t)XR_LIFT_TASK_DISPARITY_STEREO_DXR == (uint32_t)XRT_CONV_TASK_DISPARITY_STEREO,
               "XR_LIFT_TASK_DISPARITY_STEREO_DXR != XRT_CONV_TASK_DISPARITY_STEREO");
_Static_assert((uint32_t)XR_LIFT_TASK_VIEWS_MONO_DXR == (uint32_t)XRT_CONV_TASK_VIEWS_MONO,
               "XR_LIFT_TASK_VIEWS_MONO_DXR != XRT_CONV_TASK_VIEWS_MONO");
_Static_assert((uint32_t)XR_LIFT_TASK_VIEWS_FROM_DEPTH_DXR == (uint32_t)XRT_CONV_TASK_VIEWS_FROM_DEPTH,
               "XR_LIFT_TASK_VIEWS_FROM_DEPTH_DXR != XRT_CONV_TASK_VIEWS_FROM_DEPTH");
_Static_assert((uint32_t)XR_LIFT_TASK_SPLATS_MONO_DXR == (uint32_t)XRT_CONV_TASK_SPLATS_MONO,
               "XR_LIFT_TASK_SPLATS_MONO_DXR != XRT_CONV_TASK_SPLATS_MONO");
_Static_assert((uint32_t)XR_LIFT_TASK_SPLATS_STEREO_DXR == (uint32_t)XRT_CONV_TASK_SPLATS_STEREO,
               "XR_LIFT_TASK_SPLATS_STEREO_DXR != XRT_CONV_TASK_SPLATS_STEREO");
_Static_assert((uint32_t)XR_LIFT_TASK_SPLATS_VIDEO_DXR == (uint32_t)XRT_CONV_TASK_SPLATS_VIDEO,
               "XR_LIFT_TASK_SPLATS_VIDEO_DXR != XRT_CONV_TASK_SPLATS_VIDEO");
_Static_assert((uint32_t)XR_LIFT_TASK_INPAINT_DXR == (uint32_t)XRT_CONV_TASK_INPAINT,
               "XR_LIFT_TASK_INPAINT_DXR != XRT_CONV_TASK_INPAINT");
_Static_assert((uint32_t)XR_LIFT_TASK_STATE_UNKNOWN_DXR == (uint32_t)XRT_CONV_STATE_UNKNOWN,
               "XR_LIFT_TASK_STATE_UNKNOWN_DXR != XRT_CONV_STATE_UNKNOWN");
_Static_assert((uint32_t)XR_LIFT_TASK_STATE_READY_DXR == (uint32_t)XRT_CONV_STATE_READY,
               "XR_LIFT_TASK_STATE_READY_DXR != XRT_CONV_STATE_READY");
_Static_assert((uint32_t)XR_LIFT_TASK_STATE_WARMING_DXR == (uint32_t)XRT_CONV_STATE_WARMING,
               "XR_LIFT_TASK_STATE_WARMING_DXR != XRT_CONV_STATE_WARMING");
_Static_assert((uint32_t)XR_LIFT_TASK_STATE_ABSENT_DXR == (uint32_t)XRT_CONV_STATE_ABSENT,
               "XR_LIFT_TASK_STATE_ABSENT_DXR != XRT_CONV_STATE_ABSENT");
_Static_assert((uint32_t)XR_LIFT_TASK_STATE_INCOMPATIBLE_DXR == (uint32_t)XRT_CONV_STATE_INCOMPATIBLE,
               "XR_LIFT_TASK_STATE_INCOMPATIBLE_DXR != XRT_CONV_STATE_INCOMPATIBLE");
_Static_assert((uint32_t)XR_LIFT_TASK_STATE_UNLICENSED_DXR == (uint32_t)XRT_CONV_STATE_UNLICENSED,
               "XR_LIFT_TASK_STATE_UNLICENSED_DXR != XRT_CONV_STATE_UNLICENSED");
_Static_assert((uint32_t)XR_LIFT_TASK_STATE_FAILED_DXR == (uint32_t)XRT_CONV_STATE_FAILED,
               "XR_LIFT_TASK_STATE_FAILED_DXR != XRT_CONV_STATE_FAILED");
_Static_assert((uint32_t)XR_LIFT_TASK_STATE_DISABLED_DXR == (uint32_t)XRT_CONV_STATE_DISABLED,
               "XR_LIFT_TASK_STATE_DISABLED_DXR != XRT_CONV_STATE_DISABLED");
_Static_assert((uint32_t)XR_LIFT_TASK_STATE_INSTALLED_DXR == (uint32_t)XRT_CONV_STATE_INSTALLED,
               "XR_LIFT_TASK_STATE_INSTALLED_DXR != XRT_CONV_STATE_INSTALLED");
_Static_assert((uint32_t)XR_LIFT_TASK_STATE_DOWNLOADING_DXR == (uint32_t)XRT_CONV_STATE_DOWNLOADING,
               "XR_LIFT_TASK_STATE_DOWNLOADING_DXR != XRT_CONV_STATE_DOWNLOADING");
_Static_assert((uint32_t)XR_LIFT_TASK_STATE_EVICTED_DXR == (uint32_t)XRT_CONV_STATE_EVICTED,
               "XR_LIFT_TASK_STATE_EVICTED_DXR != XRT_CONV_STATE_EVICTED");
_Static_assert((uint32_t)XR_LIFT_FAILURE_REASON_NONE_DXR == (uint32_t)XRT_CONV_FAIL_NONE,
               "XR_LIFT_FAILURE_REASON_NONE_DXR != XRT_CONV_FAIL_NONE");
_Static_assert((uint32_t)XR_LIFT_FAILURE_REASON_LICENCE_DXR == (uint32_t)XRT_CONV_FAIL_LICENCE,
               "XR_LIFT_FAILURE_REASON_LICENCE_DXR != XRT_CONV_FAIL_LICENCE");
_Static_assert((uint32_t)XR_LIFT_FAILURE_REASON_LOAD_DXR == (uint32_t)XRT_CONV_FAIL_LOAD,
               "XR_LIFT_FAILURE_REASON_LOAD_DXR != XRT_CONV_FAIL_LOAD");
_Static_assert((uint32_t)XR_LIFT_FAILURE_REASON_PLATFORM_DXR == (uint32_t)XRT_CONV_FAIL_PLATFORM,
               "XR_LIFT_FAILURE_REASON_PLATFORM_DXR != XRT_CONV_FAIL_PLATFORM");
_Static_assert((uint32_t)XR_LIFT_FAILURE_REASON_NETWORK_DXR == (uint32_t)XRT_CONV_FAIL_NETWORK,
               "XR_LIFT_FAILURE_REASON_NETWORK_DXR != XRT_CONV_FAIL_NETWORK");
_Static_assert((uint32_t)XR_LIFT_FAILURE_REASON_VERSION_DXR == (uint32_t)XRT_CONV_FAIL_VERSION,
               "XR_LIFT_FAILURE_REASON_VERSION_DXR != XRT_CONV_FAIL_VERSION");
_Static_assert((uint32_t)XR_LIFT_FAILURE_REASON_RESOURCE_DXR == (uint32_t)XRT_CONV_FAIL_RESOURCES,
               "XR_LIFT_FAILURE_REASON_RESOURCE_DXR != XRT_CONV_FAIL_RESOURCES");
_Static_assert((uint32_t)XR_LIFT_FAILURE_REASON_MODEL_DXR == (uint32_t)XRT_CONV_FAIL_MODEL,
               "XR_LIFT_FAILURE_REASON_MODEL_DXR != XRT_CONV_FAIL_MODEL");
_Static_assert((uint32_t)XR_LIFT_FAILURE_REASON_DEVICE_LOST_DXR == (uint32_t)XRT_CONV_FAIL_DEVICE_LOST,
               "XR_LIFT_FAILURE_REASON_DEVICE_LOST_DXR != XRT_CONV_FAIL_DEVICE_LOST");
_Static_assert((uint32_t)XR_LIFT_FAILURE_REASON_CRASHED_DXR == (uint32_t)XRT_CONV_FAIL_CRASH_LOOP,
               "XR_LIFT_FAILURE_REASON_CRASHED_DXR != XRT_CONV_FAIL_CRASH_LOOP");
_Static_assert((uint32_t)XR_LIFT_LICENCE_CLASS_UNKNOWN_DXR == (uint32_t)XRT_CONV_LICENCE_UNKNOWN,
               "XR_LIFT_LICENCE_CLASS_UNKNOWN_DXR != XRT_CONV_LICENCE_UNKNOWN");
_Static_assert((uint32_t)XR_LIFT_LICENCE_CLASS_PERMISSIVE_DXR == (uint32_t)XRT_CONV_LICENCE_PERMISSIVE,
               "XR_LIFT_LICENCE_CLASS_PERMISSIVE_DXR != XRT_CONV_LICENCE_PERMISSIVE");
_Static_assert((uint32_t)XR_LIFT_LICENCE_CLASS_COMMERCIAL_DXR == (uint32_t)XRT_CONV_LICENCE_COMMERCIAL,
               "XR_LIFT_LICENCE_CLASS_COMMERCIAL_DXR != XRT_CONV_LICENCE_COMMERCIAL");
_Static_assert((uint32_t)XR_LIFT_LICENCE_CLASS_RESEARCH_ONLY_DXR == (uint32_t)XRT_CONV_LICENCE_RESEARCH_ONLY,
               "XR_LIFT_LICENCE_CLASS_RESEARCH_ONLY_DXR != XRT_CONV_LICENCE_RESEARCH_ONLY");
_Static_assert((uint32_t)(1u << XR_LIFT_INPUT_KIND_MONO_DXR) == (uint32_t)(XRT_CONV_INPUT_MONO << 1),
               "XR_LIFT_INPUT_KIND_MONO_DXR != XRT_CONV_INPUT_MONO");
_Static_assert((uint32_t)(1u << XR_LIFT_INPUT_KIND_STEREO_PAIR_DXR) == (uint32_t)(XRT_CONV_INPUT_STEREO_PAIR << 1),
               "XR_LIFT_INPUT_KIND_STEREO_PAIR_DXR != XRT_CONV_INPUT_STEREO_PAIR");
_Static_assert((uint32_t)(1u << XR_LIFT_INPUT_KIND_IMAGE_AND_DEPTH_DXR) == (uint32_t)(XRT_CONV_INPUT_COLOR_DEPTH << 1),
               "XR_LIFT_INPUT_KIND_IMAGE_AND_DEPTH_DXR != XRT_CONV_INPUT_COLOR_DEPTH");
_Static_assert((uint32_t)XR_LIFT_DEPTH_KIND_METRIC_DEPTH_DXR == (uint32_t)XRT_CONV_DEPTH_METRIC,
               "XR_LIFT_DEPTH_KIND_METRIC_DEPTH_DXR != XRT_CONV_DEPTH_METRIC");
_Static_assert((uint32_t)XR_LIFT_DEPTH_KIND_METRIC_INVERSE_DXR == (uint32_t)XRT_CONV_DEPTH_METRIC_INVERSE,
               "XR_LIFT_DEPTH_KIND_METRIC_INVERSE_DXR != XRT_CONV_DEPTH_METRIC_INVERSE");
_Static_assert((uint32_t)XR_LIFT_DEPTH_KIND_AFFINE_INVERSE_DEPTH_DXR == (uint32_t)XRT_CONV_DEPTH_AFFINE_INVERSE,
               "XR_LIFT_DEPTH_KIND_AFFINE_INVERSE_DEPTH_DXR != XRT_CONV_DEPTH_AFFINE_INVERSE");
_Static_assert((uint32_t)XR_LIFT_DEPTH_KIND_AFFINE_DISPARITY_DXR == (uint32_t)XRT_CONV_DEPTH_AFFINE_DISPARITY,
               "XR_LIFT_DEPTH_KIND_AFFINE_DISPARITY_DXR != XRT_CONV_DEPTH_AFFINE_DISPARITY");
_Static_assert((uint32_t)XR_LIFT_DEPTH_KIND_AFFINE_DEPTH_DXR == (uint32_t)XRT_CONV_DEPTH_AFFINE,
               "XR_LIFT_DEPTH_KIND_AFFINE_DEPTH_DXR != XRT_CONV_DEPTH_AFFINE");
_Static_assert((uint32_t)XR_LIFT_DEPTH_KIND_PIXEL_DISPARITY_DXR == (uint32_t)XRT_CONV_DEPTH_STEREO_DISPARITY_PX,
               "XR_LIFT_DEPTH_KIND_PIXEL_DISPARITY_DXR != XRT_CONV_DEPTH_STEREO_DISPARITY_PX");
_Static_assert((uint32_t)XR_LIFT_DEPTH_MEASURE_PLANAR_DXR == (uint32_t)XRT_CONV_MEASURE_PLANAR,
               "XR_LIFT_DEPTH_MEASURE_PLANAR_DXR != XRT_CONV_MEASURE_PLANAR");
_Static_assert((uint32_t)XR_LIFT_DEPTH_MEASURE_RAY_DXR == (uint32_t)XRT_CONV_MEASURE_RAY,
               "XR_LIFT_DEPTH_MEASURE_RAY_DXR != XRT_CONV_MEASURE_RAY");
_Static_assert((uint32_t)XR_LIFT_INTRINSICS_SOURCE_UNKNOWN_DXR == (uint32_t)XRT_CONV_INTRINSICS_NONE,
               "XR_LIFT_INTRINSICS_SOURCE_UNKNOWN_DXR != XRT_CONV_INTRINSICS_NONE");
_Static_assert((uint32_t)XR_LIFT_INTRINSICS_SOURCE_GIVEN_DXR == (uint32_t)XRT_CONV_INTRINSICS_GIVEN,
               "XR_LIFT_INTRINSICS_SOURCE_GIVEN_DXR != XRT_CONV_INTRINSICS_GIVEN");
_Static_assert((uint32_t)XR_LIFT_INTRINSICS_SOURCE_ESTIMATED_DXR == (uint32_t)XRT_CONV_INTRINSICS_ESTIMATED,
               "XR_LIFT_INTRINSICS_SOURCE_ESTIMATED_DXR != XRT_CONV_INTRINSICS_ESTIMATED");
_Static_assert((uint32_t)XR_LIFT_INTRINSICS_SOURCE_ASSUMED_DXR == (uint32_t)XRT_CONV_INTRINSICS_ASSUMED,
               "XR_LIFT_INTRINSICS_SOURCE_ASSUMED_DXR != XRT_CONV_INTRINSICS_ASSUMED");
_Static_assert((uint32_t)XR_LIFT_COORDINATE_SYSTEM_OPENCV_RDF_DXR == (uint32_t)XRT_CONV_COORDS_OPENCV,
               "XR_LIFT_COORDINATE_SYSTEM_OPENCV_RDF_DXR != XRT_CONV_COORDS_OPENCV");
_Static_assert((uint32_t)XR_LIFT_COORDINATE_SYSTEM_RUB_DXR == (uint32_t)XRT_CONV_COORDS_RUB,
               "XR_LIFT_COORDINATE_SYSTEM_RUB_DXR != XRT_CONV_COORDS_RUB");
_Static_assert((uint32_t)XR_LIFT_PIXEL_FORMAT_UNKNOWN_DXR == (uint32_t)XRT_CONV_FMT_UNKNOWN,
               "XR_LIFT_PIXEL_FORMAT_UNKNOWN_DXR != XRT_CONV_FMT_UNKNOWN");
_Static_assert((uint32_t)XR_LIFT_PIXEL_FORMAT_RGBA8_UNORM_DXR == (uint32_t)XRT_CONV_FMT_RGBA8_UNORM,
               "XR_LIFT_PIXEL_FORMAT_RGBA8_UNORM_DXR != XRT_CONV_FMT_RGBA8_UNORM");
_Static_assert((uint32_t)XR_LIFT_PIXEL_FORMAT_RGBA8_SRGB_DXR == (uint32_t)XRT_CONV_FMT_RGBA8_SRGB,
               "XR_LIFT_PIXEL_FORMAT_RGBA8_SRGB_DXR != XRT_CONV_FMT_RGBA8_SRGB");
_Static_assert((uint32_t)XR_LIFT_PIXEL_FORMAT_BGRA8_UNORM_DXR == (uint32_t)XRT_CONV_FMT_BGRA8_UNORM,
               "XR_LIFT_PIXEL_FORMAT_BGRA8_UNORM_DXR != XRT_CONV_FMT_BGRA8_UNORM");
_Static_assert((uint32_t)XR_LIFT_PIXEL_FORMAT_BGRA8_SRGB_DXR == (uint32_t)XRT_CONV_FMT_BGRA8_SRGB,
               "XR_LIFT_PIXEL_FORMAT_BGRA8_SRGB_DXR != XRT_CONV_FMT_BGRA8_SRGB");
_Static_assert((uint32_t)XR_LIFT_PIXEL_FORMAT_NV12_DXR == (uint32_t)XRT_CONV_FMT_NV12,
               "XR_LIFT_PIXEL_FORMAT_NV12_DXR != XRT_CONV_FMT_NV12");
_Static_assert((uint32_t)XR_LIFT_PIXEL_FORMAT_R8_UNORM_DXR == (uint32_t)XRT_CONV_FMT_R8_UNORM,
               "XR_LIFT_PIXEL_FORMAT_R8_UNORM_DXR != XRT_CONV_FMT_R8_UNORM");
_Static_assert((uint32_t)XR_LIFT_PIXEL_FORMAT_R16_FLOAT_DXR == (uint32_t)XRT_CONV_FMT_R16_FLOAT,
               "XR_LIFT_PIXEL_FORMAT_R16_FLOAT_DXR != XRT_CONV_FMT_R16_FLOAT");
_Static_assert((uint32_t)XR_LIFT_PIXEL_FORMAT_R32_FLOAT_DXR == (uint32_t)XRT_CONV_FMT_R32_FLOAT,
               "XR_LIFT_PIXEL_FORMAT_R32_FLOAT_DXR != XRT_CONV_FMT_R32_FLOAT");
_Static_assert((uint32_t)XR_LIFT_PIXEL_FORMAT_RGBA16_FLOAT_DXR == (uint32_t)XRT_CONV_FMT_RGBA16_FLOAT,
               "XR_LIFT_PIXEL_FORMAT_RGBA16_FLOAT_DXR != XRT_CONV_FMT_RGBA16_FLOAT");
_Static_assert((uint32_t)XR_LIFT_PIXEL_FORMAT_R16_UNORM_DXR == (uint32_t)XRT_CONV_FMT_R16_UNORM,
               "XR_LIFT_PIXEL_FORMAT_R16_UNORM_DXR != XRT_CONV_FMT_R16_UNORM");
_Static_assert((uint32_t)XR_LIFT_COLOR_SPACE_UNKNOWN_DXR == (uint32_t)XRT_CONV_CS_UNKNOWN,
               "XR_LIFT_COLOR_SPACE_UNKNOWN_DXR != XRT_CONV_CS_UNKNOWN");
_Static_assert((uint32_t)XR_LIFT_COLOR_SPACE_SRGB_DXR == (uint32_t)XRT_CONV_CS_SRGB,
               "XR_LIFT_COLOR_SPACE_SRGB_DXR != XRT_CONV_CS_SRGB");
_Static_assert((uint32_t)XR_LIFT_COLOR_SPACE_LINEAR_SRGB_DXR == (uint32_t)XRT_CONV_CS_LINEAR,
               "XR_LIFT_COLOR_SPACE_LINEAR_SRGB_DXR != XRT_CONV_CS_LINEAR");
_Static_assert((uint32_t)XR_LIFT_COLOR_SPACE_BT709_LIMITED_DXR == (uint32_t)XRT_CONV_CS_BT709_LIMITED,
               "XR_LIFT_COLOR_SPACE_BT709_LIMITED_DXR != XRT_CONV_CS_BT709_LIMITED");
_Static_assert((uint32_t)XR_LIFT_COLOR_SPACE_BT709_FULL_DXR == (uint32_t)XRT_CONV_CS_BT709_FULL,
               "XR_LIFT_COLOR_SPACE_BT709_FULL_DXR != XRT_CONV_CS_BT709_FULL");
_Static_assert((uint32_t)XR_LIFT_VIEWS_LAYOUT_ROW_DXR == (uint32_t)XRT_CONV_VIEWS_ROW,
               "XR_LIFT_VIEWS_LAYOUT_ROW_DXR != XRT_CONV_VIEWS_ROW");
_Static_assert((uint32_t)XR_LIFT_VIEWS_LAYOUT_ARRAY_LAYERS_DXR == (uint32_t)XRT_CONV_VIEWS_ARRAY,
               "XR_LIFT_VIEWS_LAYOUT_ARRAY_LAYERS_DXR != XRT_CONV_VIEWS_ARRAY");
_Static_assert((uint32_t)XR_LIFT_VIEWS_LAYOUT_ATLAS_DXR == (uint32_t)XRT_CONV_VIEWS_ATLAS,
               "XR_LIFT_VIEWS_LAYOUT_ATLAS_DXR != XRT_CONV_VIEWS_ATLAS");
_Static_assert((uint32_t)XR_LIFT_BLOB_FORMAT_PLY_3DGS_DXR == (uint32_t)XRT_CONV_BLOB_PLY_3DGS,
               "XR_LIFT_BLOB_FORMAT_PLY_3DGS_DXR != XRT_CONV_BLOB_PLY_3DGS");
_Static_assert((uint32_t)XR_LIFT_BLOB_FORMAT_SOG_DXR == (uint32_t)XRT_CONV_BLOB_SOG,
               "XR_LIFT_BLOB_FORMAT_SOG_DXR != XRT_CONV_BLOB_SOG");
_Static_assert(XR_LIFT_MAX_VIEWS_DXR == XRT_CONV_MAX_VIEWS, "XR_LIFT_MAX_VIEWS_DXR != XRT_CONV_MAX_VIEWS");
