// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Conversion-provider plug-in negotiation interface (ADR-053).
 *
 * The third plug-in type, next to the display-processor contract
 * (xrt_plugin.h) and input providers (xrt_input_plugin.h). A conversion
 * provider turns 2D into 3D: depth from one image or a video, disparity
 * from a rectified stereo pair, N views from one image (or from an image
 * plus depth), Gaussian splats from a photo or a stereo pair. It exposes
 * a fixed vocabulary of TASKS (XRT_CONV_TASK_*) as CAPABILITIES; the
 * runtime picks a capability per task, owns every thread, queue, policy
 * and app-facing API (XR_DXR_lift), and calls the provider to do units
 * and pixels only.
 *
 * Independent of the display-processor ABI: a conversion vendor is not a
 * display vendor. Versions and ships separately (ADR-020 rules apply:
 * struct_size on every TOP-LEVEL struct, append-only, loader-side major
 * reject). Structs embedded by value in another struct are FROZEN: they
 * carry no struct_size and are pinned by sizeof tripwires at the end of
 * this header; growth goes at the tail of the enclosing top-level struct
 * or into a new iface slot.
 *
 * Hosting-agnostic: the same DLL runs inside the service or inside a
 * supervised conversion-host process, unchanged. The provider never
 * receives a runtime graphics device and never exports memory: every
 * image is runtime-allocated and arrives as an OS shareable handle or as
 * CPU memory (xrt_conv_image). These structs are NOT a wire format: handles
 * and cpu_ptr are process-local, and a satellite proxy re-materialises
 * every image for the process that calls the provider.
 *
 * Discovery: HKLM\Software\DisplayXR\ConversionProviders\<id> (Windows),
 * NNN-<id>-conversion-provider.json in the shared roots (POSIX),
 * libdxrc<NNN>_<id>.so in the runtime APK (Android).
 * Spec: docs/specs/runtime/conversion-provider-discovery.md.
 *
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_results.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 *
 * Versioning, entry point.
 *
 */

/*! ABI major. Independent of XRT_PLUGIN_API_VERSION_CURRENT and
 *  XRT_INPUT_PLUGIN_API_VERSION_CURRENT. Same major == compatible; a
 *  different major is rejected by the loader before any dispatch. */
#define XRT_CONVERSION_PLUGIN_API_VERSION_1 1
#define XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT XRT_CONVERSION_PLUGIN_API_VERSION_1

/*! The conversion symbol. A DLL that is ALSO a display plug-in exports
 *  xrtPluginNegotiate as well (discovery spec §4: one export per type). */
#define XRT_CONVERSION_PLUGIN_ENTRYPOINT_NAME "xrtConversionPluginNegotiate"

#if defined(_WIN32) || defined(__CYGWIN__)
#define XRT_CONVERSION_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define XRT_CONVERSION_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define XRT_CONVERSION_PLUGIN_EXPORT
#endif

/*! Whole-field struct_size coverage (same rule as XRT_DP_HAS_SLOT). */
#define XRT_CONV_COVERS(PTR, TYPE, FIELD)                                                                              \
	((PTR) != NULL && (PTR)->struct_size >= offsetof(TYPE, FIELD) + sizeof(((TYPE *)0)->FIELD))
/*! Coverage + non-NULL, for an appended iface slot. */
#define XRT_CONV_IFACE_HAS(IFACE, FIELD)                                                                               \
	(XRT_CONV_COVERS(IFACE, struct xrt_conversion_plugin_iface, FIELD) && (IFACE)->FIELD != NULL)

#ifndef XRT_CONV_ABI_ASSERT
#if defined(__cplusplus)
#define XRT_CONV_ABI_ASSERT(c, m) static_assert(c, m)
#else
#define XRT_CONV_ABI_ASSERT(c, m) _Static_assert(c, m)
#endif
#endif

/* Ceilings (a provider reports its own, smaller, limits). */
/*! == XR_LIFT_MAX_VIEWS_DXR. Never edited: it sizes frozen arrays. A higher
 *  ceiling is a later slot + tail fields. */
#define XRT_CONV_MAX_VIEWS 8
#define XRT_CONV_MAX_CAPABILITIES 32 //!< per provider
#define XRT_CONV_MAX_FRAME_IMAGES 4  //!< inputs per stream frame / job
#define XRT_CONV_NAME_MAX 64
#define XRT_CONV_DETAIL_MAX 128

struct xrt_conversion_plugin_instance; //!< opaque, provider-defined (may be NULL)

/*
 *
 * Vocabulary (frame D3).
 *
 */

/*! @name Tasks (xrt_conv_capability::task). Stable ABI values. @{ */
#define XRT_CONV_TASK_NONE 0u
#define XRT_CONV_TASK_DEPTH_MONO 1u       //!< one image → depth (stateless)
#define XRT_CONV_TASK_DEPTH_VIDEO 2u      //!< frames → depth; temporal; cut/seek reset
#define XRT_CONV_TASK_DISPARITY_STEREO 3u //!< rectified pair + calibration → disparity (+ confidence)
#define XRT_CONV_TASK_VIEWS_MONO 4u       //!< image → N ≥ 2 views (+ same-inference depth)
#define XRT_CONV_TASK_VIEWS_FROM_DEPTH 5u //!< image + depth → N views (DIBR + inpaint)
#define XRT_CONV_TASK_SPLATS_MONO 6u      //!< photo → 3DGS blob (job lane)
#define XRT_CONV_TASK_SPLATS_STEREO 7u    //!< pair + calibration → metric 3DGS blob (job lane)
#define XRT_CONV_TASK_SPLATS_VIDEO 8u     //!< RESERVED — no code in v1 (sequence → splat sequence)
#define XRT_CONV_TASK_INPAINT 9u          //!< RESERVED — no code in v1 (image + mask → filled image)
#define XRT_CONV_TASK_RESERVED_FIRST 10u  //!< 10..63 reserved; ≥ 64 invalid
/*! @} */

/*! @name Lanes (bits) @{ */
#define XRT_CONV_LANE_STREAM (1u << 0) //!< stream_* slots: per-frame, runtime latest-wins
#define XRT_CONV_LANE_JOB (1u << 1)    //!< job_execute: one-shot, progress + cancel
/*! @} */

/*! @name Input kinds (bits) @{ */
#define XRT_CONV_INPUT_MONO (1u << 0)        //!< one colour image
#define XRT_CONV_INPUT_STEREO_PAIR (1u << 1) //!< rectified L/R (two images or one SBS) + calibration
#define XRT_CONV_INPUT_COLOR_DEPTH (1u << 2) //!< colour + depth (VIEWS_FROM_DEPTH)
//! RESERVED — no code in v1 (job lane, encoded bytes; the runtime sends decoded pixels)
#define XRT_CONV_INPUT_ENCODED (1u << 3)
/*! @} */

/*! @name Output bits @{ */
#define XRT_CONV_OUT_VIEWS (1u << 0)
#define XRT_CONV_OUT_DEPTH (1u << 1) //!< primary for DEPTH_*; same-inference aux for VIEWS_*
#define XRT_CONV_OUT_CONFIDENCE (1u << 2)
#define XRT_CONV_OUT_DISPARITY (1u << 3)  //!< DISPARITY_STEREO primary
#define XRT_CONV_OUT_SPLAT_BLOB (1u << 4) //!< SPLATS_* (blob sink)
/*! @} */

/*
 *
 * Image transport (frame D6).
 *
 */

/*! @name Memory kinds (bits in import masks; exactly one bit in an image) @{ */
#define XRT_CONV_MEM_CPU (1u << 0)              //!< host memory (shm when hosted out of the service)
#define XRT_CONV_MEM_WIN32_NT_HANDLE (1u << 1)  //!< D3D11/D3D12 shared NT handle
#define XRT_CONV_MEM_WIN32_KMT_HANDLE (1u << 2) //!< legacy D3D11 shared handle (same adapter only)
#define XRT_CONV_MEM_VK_OPAQUE_FD (1u << 3)
#define XRT_CONV_MEM_VK_OPAQUE_WIN32 (1u << 4)
#define XRT_CONV_MEM_DMABUF (1u << 5) //!< + drm_modifier
#define XRT_CONV_MEM_IOSURFACE (1u << 6)
#define XRT_CONV_MEM_AHARDWAREBUFFER (1u << 7)
/*! @} */

/*! @name Sync kinds (xrt_conv_sync::kind; capability masks use 1u << kind) @{ */
#define XRT_CONV_SYNC_NONE 0u               //!< producer finished before the handle was handed over
#define XRT_CONV_SYNC_KEYED_MUTEX 1u        //!< on the image itself; acquire/release keys
#define XRT_CONV_SYNC_FENCE_NT 2u           //!< D3D11/D3D12 shared fence NT handle + value
#define XRT_CONV_SYNC_VK_SEMAPHORE_FD 3u    //!< timeline semaphore, opaque fd + value
#define XRT_CONV_SYNC_VK_SEMAPHORE_WIN32 4u //!< timeline semaphore, NT handle + value
#define XRT_CONV_SYNC_SYNC_FD 5u            //!< sync_file (Linux / Android), binary
#define XRT_CONV_SYNC_MTL_SHARED_EVENT 6u   //!< MTLSharedEvent + value
/*! @} */

/*! @name Formats (own values — NOT DXGI / VkFormat; capability masks use 1u << fmt) @{ */
#define XRT_CONV_FMT_UNKNOWN 0u
#define XRT_CONV_FMT_RGBA8_UNORM 1u
#define XRT_CONV_FMT_RGBA8_SRGB 2u
#define XRT_CONV_FMT_BGRA8_UNORM 3u
#define XRT_CONV_FMT_BGRA8_SRGB 4u
#define XRT_CONV_FMT_NV12 5u
#define XRT_CONV_FMT_R8_UNORM 6u
#define XRT_CONV_FMT_R16_FLOAT 7u
#define XRT_CONV_FMT_R32_FLOAT 8u
#define XRT_CONV_FMT_RGBA16_FLOAT 9u
#define XRT_CONV_FMT_R16_UNORM 10u
/*! @} */

/*! @name Colour space @{ */
#define XRT_CONV_CS_UNKNOWN 0u
#define XRT_CONV_CS_SRGB 1u //!< sRGB-encoded values (in a UNORM or SRGB format)
#define XRT_CONV_CS_LINEAR 2u
#define XRT_CONV_CS_BT709_LIMITED 3u //!< YUV video range
#define XRT_CONV_CS_BT709_FULL 4u
#define XRT_CONV_CS_BT2020_PQ 5u
#define XRT_CONV_CS_DATA 6u //!< not colour (depth / disparity / confidence)
/*! @} */

/*! A sync primitive attached to an image. The handle is BORROWED for the
 *  call; a provider that needs it longer duplicates it. Equal import_key ⇒
 *  same primitive (cache the import).
 *  FROZEN: growth goes at the tail of the enclosing top-level struct or into
 *  a new slot. sizeof == 40 (asserted). */
struct xrt_conv_sync
{
	uint32_t kind;       //!< XRT_CONV_SYNC_*
	uint32_t reserved_0; //!< explicit padding; 0
	uint64_t handle;     //!< NT HANDLE / fd / port, widened
	uint64_t import_key;
	uint64_t acquire_value; //!< wait for ≥ this before access (keyed mutex: acquire key)
	uint64_t release_value; //!< signal when done (keyed mutex: release key)
};

/*! One image. Every image a provider sees is RUNTIME-allocated. Handles and
 *  cpu_ptr are valid only in the calling process; a satellite proxy
 *  re-materialises the image per hosting (never copies this struct across).
 *  FROZEN: growth goes at the tail of the enclosing top-level struct or into
 *  a new slot. sizeof == 128 on 64-bit targets (asserted). */
struct xrt_conv_image
{
	uint32_t memory_kind; //!< exactly one XRT_CONV_MEM_* bit; 0 = "not present / not requested"
	uint32_t reserved_0;  //!< explicit padding; 0
	uint64_t handle;      //!< shareable handle (not CPU), borrowed for the call
	uint64_t import_key;  //!< runtime-unique per allocation; same key ⇒ same memory
	void *cpu_ptr;        //!< XRT_CONV_MEM_CPU only, mapped in the calling process
	uint64_t byte_size;
	uint32_t width, height; //!< texels (plane 0)
	uint32_t format;        //!< XRT_CONV_FMT_*
	uint32_t color_space;   //!< XRT_CONV_CS_*
	uint32_t row_stride;    //!< bytes for CPU / linear; 0 for opaque GPU textures
	uint32_t plane1_offset; //!< NV12 chroma (CPU / linear)
	uint32_t plane1_stride;
	uint32_t array_layer;  //!< layer of an array image (ADR-032); 0 for flat
	uint64_t drm_modifier; //!< DMABUF only
	uint64_t adapter_luid; //!< adapter the memory lives on; 0 = CPU / unknown
	struct xrt_conv_sync sync;
};

/*
 *
 * Readiness (frame D8).
 *
 */

/*! @name Capability state. 0..7 are NUMERICALLY IDENTICAL to
 *  XRT_DP_LIFT_MODULE_* (asserted in oxr_plugin_stub.c) so the legacy
 *  adapter copies module_status verbatim. 8.. are new, append-only. @{ */
#define XRT_CONV_STATE_UNKNOWN 0u
#define XRT_CONV_STATE_READY 1u        //!< resident; converts now
#define XRT_CONV_STATE_WARMING 2u      //!< load / licence / compile in progress; progress valid
#define XRT_CONV_STATE_ABSENT 3u       //!< its dependency or model is not installed
#define XRT_CONV_STATE_INCOMPATIBLE 4u //!< installed but unusable (version / OS / GPU / driver)
#define XRT_CONV_STATE_UNLICENSED 5u
#define XRT_CONV_STATE_FAILED 6u      //!< failure_reason says which
#define XRT_CONV_STATE_DISABLED 7u    //!< configuration
#define XRT_CONV_STATE_INSTALLED 8u   //!< present and cold; warm() makes it READY
#define XRT_CONV_STATE_DOWNLOADING 9u //!< model bytes in flight; progress valid
#define XRT_CONV_STATE_EVICTED 10u    //!< was READY; unloaded (idle)
/*! @} */

/*! @name failure_reason @{ */
#define XRT_CONV_FAIL_NONE 0u
#define XRT_CONV_FAIL_LICENCE 1u
#define XRT_CONV_FAIL_LOAD 2u
#define XRT_CONV_FAIL_PLATFORM 3u
#define XRT_CONV_FAIL_NETWORK 4u
#define XRT_CONV_FAIL_VERSION 5u
#define XRT_CONV_FAIL_RESOURCES 6u //!< VRAM / memory
#define XRT_CONV_FAIL_MODEL 7u     //!< missing / bad hash / unsupported
#define XRT_CONV_FAIL_DEVICE_LOST 8u
#define XRT_CONV_FAIL_CRASH_LOOP 9u //!< satellite only: the crash-loop cutoff; a single crash is WARMING(restarting)
/*! @} */

/*! Runtime pre-sizes + zero-fills; provider answers from memory. */
struct xrt_conv_state
{
	uint32_t struct_size;
	uint32_t state;          //!< XRT_CONV_STATE_*
	uint32_t failure_reason; //!< XRT_CONV_FAIL_*
	uint32_t retryable;      //!< non-zero: a later warm() may succeed
	float progress;          //!< [0,1] WARMING / DOWNLOADING; < 0 = indeterminate
	uint32_t reserved_0;
	uint64_t eta_ns;                  //!< 0 = unknown
	char version[48];                 //!< dependency / model version actually resolved
	char detail[XRT_CONV_DETAIL_MAX]; //!< for a human, verbatim, never parsed
};

/*
 *
 * Capability descriptor (frame D2).
 *
 */

/*! @name Licence class @{ */
#define XRT_CONV_LICENCE_UNKNOWN 0u       //!< selection treats it as RESEARCH_ONLY
#define XRT_CONV_LICENCE_PERMISSIVE 1u    //!< open licence, commercial use OK
#define XRT_CONV_LICENCE_COMMERCIAL 2u    //!< proprietary, licensed for commercial use
#define XRT_CONV_LICENCE_RESEARCH_ONLY 3u //!< non-commercial: explicit opt-in only
/*! @} */

/*! @name Depth kinds (frame D9) @{ */
#define XRT_CONV_DEPTH_UNKNOWN 0u
#define XRT_CONV_DEPTH_METRIC 1u              //!< metres
#define XRT_CONV_DEPTH_METRIC_INVERSE 2u      //!< 1 / metres
#define XRT_CONV_DEPTH_AFFINE_INVERSE 3u      //!< inverse depth up to scale + shift (larger = nearer)
#define XRT_CONV_DEPTH_AFFINE_DISPARITY 4u    //!< disparity up to scale + shift (larger = nearer)
#define XRT_CONV_DEPTH_AFFINE 5u              //!< depth up to scale + shift (larger = farther)
#define XRT_CONV_DEPTH_STEREO_DISPARITY_PX 6u //!< px disparity of the given rectified pair (xL − xR)
/*! @} */
/*! @name Depth measure (xrt_conv_capability::depth_measure, xrt_conv_depth_info::measure) @{ */
#define XRT_CONV_MEASURE_PLANAR 0u //!< to the image plane (z)
#define XRT_CONV_MEASURE_RAY 1u    //!< along the ray
/*! @} */

/*! @name Intrinsics source @{ */
#define XRT_CONV_INTRINSICS_NONE 0u
#define XRT_CONV_INTRINSICS_GIVEN 1u     //!< echoed from params
#define XRT_CONV_INTRINSICS_ESTIMATED 2u //!< estimated by the model
#define XRT_CONV_INTRINSICS_ASSUMED 3u   //!< fixed convention (nominal FOV)
/*! @} */

/*! @name Capability flags @{ */
#define XRT_CONV_CAP_TEMPORAL (1u << 0)              //!< per-stream state; honours FRAME_RESET
#define XRT_CONV_CAP_EXPLICIT_VIEWPOINTS (1u << 1)   //!< honours params.viewpoints_xyz (REQUIRED for VIEWS_*)
#define XRT_CONV_CAP_OFFAXIS_CAMERA (1u << 2)        //!< uses the rect / nominal-z geometry (ADR-048 block)
#define XRT_CONV_CAP_SUGGESTS_CONVERGENCE (1u << 3)  //!< fills suggested_convergence (D10 hint)
#define XRT_CONV_CAP_NEEDS_NETWORK_TO_WARM (1u << 4) //!< first warm may need the network
#define XRT_CONV_CAP_USES_MODEL_STORE (1u << 5)      //!< resolves models via host->model_resolve (§4)
#define XRT_CONV_CAP_DETERMINISTIC (1u << 6)         //!< same input ⇒ same bytes (tests)
#define XRT_CONV_CAP_LANES_CONCURRENT (1u << 7)      //!< job_execute may overlap stream_* calls; else serialised
#define XRT_CONV_CAP_WARM_CONCURRENT (1u << 8) //!< warm may overlap converts; else serialised (stall reported in stats)
/*! @} */

/*! One capability = (task × model × backend × device). Top-level: the
 *  runtime zero-fills ONE struct and sets struct_size before each
 *  describe_cap(); the provider writes within struct_size. A process-wide
 *  model choice is one capability whose variant follows the quality hint,
 *  not a group of mutually exclusive capabilities. */
struct xrt_conv_capability
{
	uint32_t struct_size;
	uint32_t cap_id;      //!< provider-chosen, non-zero, stable for the instance's life
	uint32_t task;        //!< XRT_CONV_TASK_*
	uint32_t lanes;       //!< XRT_CONV_LANE_*
	uint32_t input_kinds; //!< XRT_CONV_INPUT_*
	uint32_t outputs;     //!< XRT_CONV_OUT_* it CAN produce
	uint32_t flags;       //!< XRT_CONV_CAP_*

	/* Transport (the runtime converts / copies for anything not listed). */
	uint32_t import_memory_kinds; //!< XRT_CONV_MEM_* bits
	uint32_t import_sync_kinds;   //!< 1u << XRT_CONV_SYNC_*
	uint32_t input_formats;       //!< 1u << XRT_CONV_FMT_* for colour input
	uint32_t depth_output_format; //!< XRT_CONV_FMT_R32_FLOAT / R16_FLOAT / R8_UNORM

	/* Limits — ENFORCED by the runtime. */
	uint32_t min_views, max_views;
	uint32_t max_streams;          //!< 0 = runtime default (4)
	uint32_t max_concurrent;       //!< converts in flight across this cap's streams; 0 = 1
	uint32_t max_input_edge;       //!< px; runtime downscales above it; 0 = no limit
	uint32_t preferred_input_edge; //!< the model's native long edge; 0 = n/a

	/* Output contract (frame D9). */
	uint32_t depth_kind;        //!< XRT_CONV_DEPTH_* of the primary / aux depth
	uint32_t depth_measure;     //!< XRT_CONV_MEASURE_*
	uint32_t intrinsics_source; //!< what results will carry
	uint32_t splat_formats;     //!< 1u << XRT_CONV_BLOB_*

	/* Licence + identity (diagnostics; never shown to apps as names). */
	uint32_t licence_class; //!< XRT_CONV_LICENCE_*
	uint32_t commercial_ok; //!< 1 = model AND every dependency permit commercial use
	char licence_spdx[48];  //!< "MIT", "Apache-2.0", "LicenseRef-<owner>"
	char model_id[XRT_CONV_NAME_MAX];
	char model_version[32];
	char backend[32]; //!< e.g. "onnxruntime-dml", "vendor-native"
	char display_name[XRT_CONV_NAME_MAX];
	uint32_t reserved_0; //!< explicit padding before device_luid; 0

	/* Device + cost estimates (0 = unknown). */
	uint64_t device_luid; //!< adapter it runs on once warm; 0 = CPU / undecided
	uint32_t est_warmup_ms;
	uint32_t est_latency_ms; //!< per convert at preferred_input_edge, or per job
	uint32_t est_vram_mb;
	uint32_t est_ram_mb;
};

/*
 *
 * Parameters.
 *
 */

/*! @name Content hint, viewpoint frame / source, axis mode (xrt_conv_params)
 *  Equal to xrt_dp_lift.h where they overlap (asserted in oxr_plugin_stub.c). @{ */
#define XRT_CONV_CONTENT_VIDEO 0u
#define XRT_CONV_CONTENT_PHOTO 1u
#define XRT_CONV_VIEWPOINT_FRAME_DISPLAY 0u
#define XRT_CONV_VIEWPOINT_FRAME_RECT 1u
#define XRT_CONV_AXIS_X 1u
#define XRT_CONV_AXIS_XY 2u
#define XRT_CONV_AXIS_XYZ 3u
#define XRT_CONV_VIEWPOINTS_TRACKED 0u
#define XRT_CONV_VIEWPOINTS_EXPLICIT 1u
#define XRT_CONV_VIEWPOINTS_DISPLAY_RIG 2u
#define XRT_CONV_VIEWPOINTS_CAMERA_RIG 3u
/*! @} */

/*! @name xrt_conv_params::frame_flags @{ */
#define XRT_CONV_FRAME_RESET (1u << 0)   //!< cut / seek: drop temporal state BEFORE this frame
#define XRT_CONV_FRAME_INPAINT (1u << 1) //!< fill disocclusions
/*! @} */

/*! @name xrt_conv_params::convergence_mode @{ */
#define XRT_CONV_CONVERGENCE_AUTO 0u    //!< no plane given: the provider's own estimate, or its default
#define XRT_CONV_CONVERGENCE_DECODED 1u //!< params.convergence is the zero-parallax plane in DECODED depth units
/*! @} */

/*! Rectified stereo calibration — the XR_DXR_stereo_camera shape.
 *  FROZEN: growth goes at the tail of the enclosing top-level struct or into
 *  a new slot. sizeof == 32 (asserted). */
struct xrt_conv_stereo_calibration
{
	uint32_t eye_order;      //!< 0 = left first, 1 = right first
	float fx, fy, cx, cy;    //!< px of ONE rectified eye
	float baseline_m;        //!< 0 = UNKNOWN; never defaulted
	float infinity_shift_px; //!< disparity at infinity after rectification
	uint32_t sbs_packed;     //!< 1 = both eyes in one side-by-side image
};

/*! Everything the runtime decided (policy is runtime-owned, D10). Top-level:
 *  the runtime sets struct_size; the provider reads only within it. */
struct xrt_conv_params
{
	uint32_t struct_size;
	uint32_t frame_flags;    //!< XRT_CONV_FRAME_*
	uint64_t source_time_ns; //!< echoed in results
	uint64_t frame_id;       //!< per stream, increasing

	/* Look. */
	uint32_t convergence_mode; //!< XRT_CONV_CONVERGENCE_*
	float convergence;         //!< DECODED mode: the zero-parallax plane in the DECODED units of this
	                           //!< stream's depth output (value = value_scale * sample + value_shift, the
	                           //!< mirror of depth_info.convergence_value). Polarity follows the depth
	                           //!< kind: larger = nearer for *_INVERSE / *_DISPARITY, larger = farther
	                           //!< for METRIC_DEPTH / AFFINE. The runtime maps an app's [0,1]
	                           //!< "relative depth at glass" onto this using the last result's range;
	                           //!< the provider never normalises it again. A given plane also means
	                           //!< "provider auto-convergence OFF".
	float strength;            //!< 1 = provider budget
	uint32_t view_count;       //!< VIEWS_*: validated against min/max_views
	uint32_t content_hint;     //!< XRT_CONV_CONTENT_*
	float input_scale;         //!< ADVISORY; the runtime already applied max_input_edge
	float focal_px;            //!< input px; ≤ 0 = unknown (never invented)

	/* Viewpoint policy (ADR-048), already applied. */
	uint32_t viewpoint_frame;
	uint32_t viewpoint_source;
	uint32_t axis_mode;
	uint32_t viewpoint_count;                    //!< == view_count for VIEWS_*; else 0
	float viewpoints_xyz[XRT_CONV_MAX_VIEWS][3]; //!< metres, rect-relative, +z toward viewer
	float rect_width_m, rect_height_m;
	float baseline_m;
	float max_offset_m;
	float nominal_z_m;

	/* Stereo input (DISPARITY_STEREO, SPLATS_STEREO). */
	struct xrt_conv_stereo_calibration stereo;
};

/*
 *
 * Results (frame D9).
 *
 */

/*! FROZEN: growth goes at the tail of the enclosing top-level struct or into
 *  a new slot. sizeof == 128 (asserted). */
struct xrt_conv_depth_info
{
	uint32_t kind;    //!< XRT_CONV_DEPTH_*
	uint32_t measure; //!< XRT_CONV_MEASURE_*
	uint32_t intrinsics_source;
	float value_scale, value_shift; //!< value = scale * sample + shift (scale 0 read as 1)
	float near_value, far_value;    //!< decoded; far < near ⇒ infinite far; 0,0 = unknown
	float invalid_sample;           //!< raw "no data" value; NaN = none
	float range_min, range_max;     //!< decoded per-frame extent BEFORE any normalisation
	float range_p05, range_p95;     //!< 0,0 = not computed
	float fx, fy, cx, cy;           //!< depth-image texels; ≤ 0 = unknown
	uint32_t source_width, source_height;
	float convergence_value; //!< decoded value at zero disparity of the paired views; valid iff (suggest_flags & 4)
	float suggested_convergence; //!< provider hint, DECODED units (see params.convergence); valid iff
	                             //!< (suggest_flags & 1)
	float suggested_strength; //!< provider hint for the disparity budget, 1 = its own; valid iff (suggest_flags &
	                          //!< 2). Both hints are IGNORED by policy unless the provider is validated against the
	                          //!< corpus.
	uint32_t suggest_flags;   //!< bit0 suggested_convergence, bit1 suggested_strength, bit2 convergence_value
	uint32_t relief_valid;    //!< ADR-046 A1: z_front_m = relief_scale/value + relief_offset
	float relief_scale, relief_offset;
	uint32_t temporal;       //!< from a temporal model
	uint32_t same_inference; //!< same inference as this frame's views
	uint32_t has_confidence;
	uint64_t source_time_ns;
	uint64_t provider_frame_id; //!< informational
};

/*! @name Views layout (xrt_conv_stream_info::views_layout, xrt_conv_views_info::layout) @{ */
#define XRT_CONV_VIEWS_ROW 0u   //!< N views side by side, view 0 leftmost (today's SBS / NVIEW)
#define XRT_CONV_VIEWS_ARRAY 1u //!< view i in array layer i (ADR-032)
/*! RESERVED: columns × rows, row-major. The runtime picks ATLAS or ARRAY when
 *  N × w exceeds the texture limit (not reachable at 8 views). */
#define XRT_CONV_VIEWS_ATLAS 2u
/*! @} */

/*! FROZEN: growth goes at the tail of the enclosing top-level struct or into
 *  a new slot. sizeof == 124 (asserted; XRT_CONV_MAX_VIEWS 8). */
struct xrt_conv_views_info
{
	uint32_t view_count;
	uint32_t layout;                      //!< must equal the target's layout
	uint32_t reserved_0;                  //!< 0
	float eye_xyz[XRT_CONV_MAX_VIEWS][3]; //!< viewpoints actually rendered
	float baseline_m;
	float hfov_deg;                //!< one view; ≤ 0 = unknown
	float zero_parallax_disparity; //!< fraction of view width at the zero-parallax plane
	float reserved_1;
};

/*! @name Blob formats (1, 2 equal XRT_DP_LIFT_BLOB_*) @{ */
#define XRT_CONV_BLOB_PLY_3DGS 1u
#define XRT_CONV_BLOB_SOG 2u
#define XRT_CONV_BLOB_SPZ 3u               //!< canonical splat output
#define XRT_CONV_BLOB_RAW_IMAGE 4u         //!< job-lane image (layout in the matching *_info)
#define XRT_CONV_BLOB_SEQUENCE_MANIFEST 5u //!< RESERVED — no code in v1
/*! @} */
/*! @name Splat coordinate convention (xrt_conv_splat_info::coords) @{ */
#define XRT_CONV_COORDS_OPENCV 1u //!< +x right, +y down, +z forward, metres
#define XRT_CONV_COORDS_RUB 2u    //!< +x right, +y up, +z back
/*! @} */

/*! FROZEN: growth goes at the tail of the enclosing top-level struct or into
 *  a new slot. sizeof == 72 (asserted). */
struct xrt_conv_splat_info
{
	uint32_t blob_format;
	uint32_t coords;      //!< XRT_CONV_COORDS_*
	uint32_t color_space; //!< tag honestly
	uint32_t metric;      //!< 1 = metres are real
	uint32_t intrinsics_source;
	float fx, fy, cx, cy; //!< of the grid actually unprojected through
	uint32_t grid_width, grid_height;
	float baseline_m;               //!< 0 = unknown; never defaulted
	float focus_z_m, near_m, far_m; //!< 0 = unknown
	uint32_t reserved_0;            //!< explicit padding before splat_count; 0
	uint64_t splat_count;
};

/*
 *
 * Stream lane.
 *
 */

/*! stream_create input. Top-level: the runtime sets struct_size. */
struct xrt_conv_stream_info
{
	uint32_t struct_size;
	uint32_t input_kind; //!< one XRT_CONV_INPUT_* bit
	uint32_t outputs;    //!< subset of cap.outputs the runtime will request
	uint32_t content_hint;
	uint32_t view_count;   //!< VIEWS_*: fixed for the stream
	uint32_t views_layout; //!< XRT_CONV_VIEWS_*
	uint32_t max_input_width, max_input_height;
};

/*! Output extents for a given input extent — pure, cheap, any thread. Top-level:
 *  the runtime zero-fills and sets struct_size; the provider writes within it. */
struct xrt_conv_output_desc
{
	uint32_t struct_size;
	uint32_t views_width, views_height; //!< whole target (ROW: N·w × h; ARRAY: per layer)
	uint32_t views_format;
	uint32_t depth_width, depth_height; //!< 0 = no depth for this stream
	uint32_t depth_format;
	uint32_t confidence_width, confidence_height, confidence_format;
	uint32_t disparity_width, disparity_height, disparity_format;
};

/*! @name Image roles (xrt_conv_frame_in::image_role, xrt_conv_job_in::image_role) @{ */
#define XRT_CONV_IMG_COLOR 0u
#define XRT_CONV_IMG_COLOR_RIGHT 1u //!< second eye when not sbs_packed
#define XRT_CONV_IMG_DEPTH_IN 2u    //!< VIEWS_FROM_DEPTH (meaning in depth_in_info)
/*! @} */

/*! One stream frame's inputs (stream_convert). Top-level: the runtime sets
 *  struct_size. images[i] has role image_role[i], for i < image_count. */
struct xrt_conv_frame_in
{
	uint32_t struct_size;
	uint32_t image_count;
	uint32_t image_role[XRT_CONV_MAX_FRAME_IMAGES];
	struct xrt_conv_image images[XRT_CONV_MAX_FRAME_IMAGES];
	struct xrt_conv_depth_info depth_in_info;
};

/*! Targets are runtime-allocated to xrt_conv_output_desc; memory_kind 0 =
 *  not requested. The provider writes every requested target, then signals
 *  its sync (or drains before returning when kind == NONE), sets `written`,
 *  and fills the (frozen, fixed-size) info structs it has data for; fields
 *  appended later go at the tail of this struct, covered by its struct_size. */
struct xrt_conv_frame_out
{
	uint32_t struct_size;
	uint32_t written; //!< XRT_CONV_OUT_* actually written
	struct xrt_conv_image views;
	struct xrt_conv_image depth;
	struct xrt_conv_image confidence;
	struct xrt_conv_image disparity;
	struct xrt_conv_views_info views_info;
	uint32_t reserved_pad; //!< explicit padding before depth_info (8-aligned); 0
	struct xrt_conv_depth_info depth_info;
};

/*
 *
 * Job lane.
 *
 */

/*! Runtime-owned byte sink (shm, file or IPC behind it). false = stop. */
struct xrt_conv_blob_sink
{
	uint32_t struct_size;
	uint32_t role; //!< XRT_CONV_OUT_* this sink receives
	void *ctx;
	bool (*begin)(void *ctx, uint32_t blob_format, uint64_t size_hint);
	bool (*write)(void *ctx, const void *bytes, uint64_t count);
};

/*! One job's inputs (job_execute). Top-level: the runtime sets struct_size.
 *  Decoded pixels only in v1; the encoded fields are reserved. */
struct xrt_conv_job_in
{
	uint32_t struct_size;
	uint32_t input_kind;
	uint32_t image_count;
	uint32_t encoded_count; //!< RESERVED (XRT_CONV_INPUT_ENCODED, no code in v1): 0
	uint32_t image_role[XRT_CONV_MAX_FRAME_IMAGES];
	struct xrt_conv_image images[XRT_CONV_MAX_FRAME_IMAGES];
	const void *encoded[2];         //!< RESERVED: NULL in v1
	uint64_t encoded_size[2];       //!< RESERVED: 0 in v1
	uint32_t requested_blob_format; //!< XRT_CONV_BLOB_*; 0 = provider default
	uint32_t reserved_0;
};

/*! Runtime-owned control block for job_execute: sinks, progress, cancel. */
struct xrt_conv_job_control
{
	uint32_t struct_size;
	uint32_t sink_count;
	struct xrt_conv_blob_sink *sinks;
	void *ctx;                                                      //!< passed to progress and is_cancelled
	void (*progress)(void *ctx, float fraction, const char *stage); //!< runtime throttles
	/*! Poll at least every 250 ms; true ⇒ stop and return. Thread-safe,
	 *  non-blocking, cheap; once true it stays true for the call. In a
	 *  satellite the host's pipe thread is what sets the state it reads. */
	bool (*is_cancelled)(void *ctx);
};

/*! job_execute out-struct. Top-level: the runtime zero-fills and sets
 *  struct_size; the provider fills the (frozen) info structs it has data for. */
struct xrt_conv_job_out
{
	uint32_t struct_size;
	uint32_t written; //!< XRT_CONV_OUT_* delivered to sinks
	struct xrt_conv_splat_info splat_info;
	struct xrt_conv_depth_info depth_info;
	struct xrt_conv_views_info views_info;
	uint32_t reserved_pad; //!< explicit tail padding (8-aligned size); 0
};

/*
 *
 * Provider status (ADR-045 shape).
 *
 */

/*! Same layout and state values as xrt_plugin_platform_status (asserted in
 *  oxr_plugin_stub.c), restated so this header stays self-contained.
 *  "Platform" = the provider's own dependencies (a vendor conversion
 *  runtime, an inference runtime, a driver), never a display. */
struct xrt_conv_provider_status
{
	uint32_t struct_size;
	uint32_t state; //!< xrt_plugin_platform_state values
	uint32_t flags; //!< XRT_CONV_PROVIDER_FLAG_*
	uint32_t reserved_0;
	char hint[128];
};
/*! @name xrt_conv_provider_status::flags @{ */
#define XRT_CONV_PROVIDER_FLAG_FALLBACK (1u << 0)  //!< sim provider only; never chosen over a real cap
#define XRT_CONV_PROVIDER_FLAG_REFERENCE (1u << 1) //!< runtime-shipped vendor-neutral reference
/*! @} */

/*
 *
 * Host iface.
 *
 */

/*! @name get_path kinds (xrt_conversion_plugin_host_iface::get_path) @{ */
#define XRT_CONV_PATH_MODEL_STORE 1u //!< shared content-addressed store root (read-only to providers)
#define XRT_CONV_PATH_SCRATCH 2u     //!< per-provider writable dir
#define XRT_CONV_PATH_LOGS 3u        //!< informational
/*! @} */

/*! get_adapter_hint in/out: which adapter the provider should create its device on. */
struct xrt_conv_adapter_hint
{
	uint32_t struct_size;
	uint32_t flags;          //!< bit0: preferred is the panel's render adapter; bit1: preferred is integrated
	uint64_t preferred_luid; //!< 0 = no preference
	uint64_t avoid_luid;     //!< 0 = none
};

/*! model_resolve out: where a model-store entry is, or how far its download got. */
struct xrt_conv_model_location
{
	uint32_t struct_size;
	uint32_t state; //!< READY / DOWNLOADING / ABSENT / FAILED
	float progress;
	uint32_t failure_reason;
	char path[512]; //!< UTF-8 absolute path of the verified blob when READY
};

/*!
 * Runtime → provider services. Valid from negotiate to process end. Every
 * callback is thread-safe and non-blocking. Logging / metrics / debug vars
 * come from the ADR-019 aux surface, NOT from here. Never read at/past
 * struct_size; reserved[] slots are carved out without a major bump
 * (xrt_input_plugin.h rule).
 */
struct xrt_conversion_plugin_host_iface
{
	uint32_t struct_size;
	uint32_t host_api_version;
	uint32_t host_flags; //!< bit0: hosted out of the service (satellite); bit1: headless / CI
	uint32_t reserved_0;

	bool (*get_path)(uint32_t kind, char *buf, uint32_t buf_size);

	/*! env DXR_CONV_<ID>_<KEY>, else HKLM\Software\DisplayXR\ConversionProviders\
	 *  <id>\Settings\<KEY> (Windows) / the manifest "settings" object (POSIX).
	 *  Read in the HOSTING process. */
	bool (*get_setting)(const char *key, char *buf, uint32_t buf_size);

	bool (*get_adapter_hint)(struct xrt_conv_adapter_hint *inout);

	/*! Model store (contract §4). ref = "<pack>/<model>[/<variant>]" or a
	 *  sha256 hex. resolve: non-blocking lookup. request: queue a download
	 *  that the HOST performs, verifies and stamps. */
	bool (*model_resolve)(const char *ref, struct xrt_conv_model_location *out);
	bool (*model_request)(const char *ref);

	/*! Push: cap_id's state changed (0 = re-describe everything). The runtime
	 *  re-reads on its own thread. Replaces 1 Hz polling. */
	void (*notify_changed)(uint32_t cap_id);

	void *reserved[10];
};

/*
 *
 * Provider iface.
 *
 */

/*!
 * THREADING (normative; the discovery spec carries the same table):
 *
 *   Default: ONE CALL IN FLIGHT PER PROVIDER. The runtime (or the satellite
 *   host) serialises every slot marked [S], across all of the provider's
 *   capabilities, streams and jobs. A provider built on a single-threaded
 *   library needs no locks of its own.
 *
 *   [C] get_provider_status, describe_cap, get_generation, get_state,
 *       stream_get_output_desc: may be called from any runtime thread,
 *       concurrently with anything, including an [S] call in progress. They
 *       answer from memory: non-blocking, no model load, no I/O.
 *
 *   Opt-ins (each widens only what it names):
 *     cap.max_concurrent > 1         stream_convert calls on this capability's
 *                                    streams may overlap, up to that number.
 *     XRT_CONV_CAP_LANES_CONCURRENT  job_execute may overlap stream_* calls.
 *     XRT_CONV_CAP_WARM_CONCURRENT   warm may overlap stream_convert and
 *                                    job_execute. Without it a warm (seconds)
 *                                    serialises with converts and the stall is
 *                                    reported in the stream's stats.
 *
 * [S] calls run on runtime-owned worker threads and may block for as long as
 * their slot says. No slot is ever called from a compositor / render /
 * IPC-dispatch thread, or with a runtime render lock held.
 */
struct xrt_conversion_plugin_iface
{
	uint32_t struct_size;
	uint32_t reserved_0;
	const char *id; //!< == registry / manifest <id>
	const char *display_name;
	const char *vendor;  //!< may be NULL
	const char *version; //!< may be NULL

	/*! [C], no instance, ≤ 100 ms. Are the provider's dependencies there? */
	bool (*get_provider_status)(struct xrt_conv_provider_status *out);

	/*! [S], ≤ 100 ms; no model load, no network.
	 *  XRT_ERROR_PROBER_NOT_SUPPORTED = clean decline. */
	xrt_result_t (*probe)(struct xrt_conversion_plugin_instance **out_inst);

	/*! [C], ≤ 100 ms, no model load. ONE capability per call, so the
	 *  capability struct can grow without mis-striding an array.
	 *  - out == NULL: writes the capability count (≤ XRT_CONV_MAX_CAPABILITIES)
	 *    to *out_count and returns XRT_SUCCESS; index is ignored.
	 *  - out != NULL: the runtime has zero-filled *out and set
	 *    out->struct_size; the provider fills capability `index` within
	 *    struct_size. out_count may be NULL; when not, the count is written too.
	 *  - index ≥ count: XRT_ERROR_FEATURE_NOT_SUPPORTED, *out untouched.
	 *  The set is a snapshot of one generation: the runtime reads
	 *  get_generation() before and after the walk and re-walks if it moved. */
	xrt_result_t (*describe_cap)(struct xrt_conversion_plugin_instance *inst,
	                             uint32_t index,
	                             struct xrt_conv_capability *out,
	                             uint32_t *out_count);

	/*! [C]. Changes whenever describe_cap() would answer differently. */
	uint64_t (*get_generation)(struct xrt_conversion_plugin_instance *inst);

	/*! [C]; non-blocking; allocation-free. */
	xrt_result_t (*get_state)(struct xrt_conversion_plugin_instance *inst,
	                          uint32_t cap_id,
	                          struct xrt_conv_state *out);

	/*! [S] (overlaps converts only under XRT_CONV_CAP_WARM_CONCURRENT); MAY
	 *  BLOCK until READY or terminal; publishes progress through get_state
	 *  meanwhile; calls is_cancelled(cancel_ctx) at least every 250 ms and
	 *  returns promptly once it is true (sticky for the call). */
	xrt_result_t (*warm)(struct xrt_conversion_plugin_instance *inst,
	                     uint32_t cap_id,
	                     bool (*is_cancelled)(void *ctx),
	                     void *cancel_ctx);

	/*! [S]; only with no live stream / running job on cap_id; idempotent. */
	xrt_result_t (*evict)(struct xrt_conversion_plugin_instance *inst, uint32_t cap_id);

	/*! [S]; cap READY; ≤ 500 ms. */
	xrt_result_t (*stream_create)(struct xrt_conversion_plugin_instance *inst,
	                              uint32_t cap_id,
	                              const struct xrt_conv_stream_info *info,
	                              uint64_t *out_stream);

	/*! [C]; pure. */
	xrt_result_t (*stream_get_output_desc)(struct xrt_conversion_plugin_instance *inst,
	                                       uint64_t stream,
	                                       uint32_t input_width,
	                                       uint32_t input_height,
	                                       struct xrt_conv_output_desc *out);

	/*! [S] (overlaps up to cap.max_concurrent); blocking for one inference. Failure ⇒ runtime keeps the previous
	 *  result. Device loss ⇒ return XRT_ERROR_DEVICE_LOST-class; the runtime
	 *  marks the cap FAILED(DEVICE_LOST, retryable) and re-warms. */
	xrt_result_t (*stream_convert)(struct xrt_conversion_plugin_instance *inst,
	                               uint64_t stream,
	                               const struct xrt_conv_frame_in *in,
	                               const struct xrt_conv_params *params,
	                               struct xrt_conv_frame_out *out);

	/*! [S]. Frees per-stream state + import caches. */
	void (*stream_destroy)(struct xrt_conversion_plugin_instance *inst, uint64_t stream);

	/*! [S] (overlaps stream_* only under XRT_CONV_CAP_LANES_CONCURRENT);
	 *  blocking, possibly for minutes; polls ctl->is_cancelled; outputs go to
	 *  ctl->sinks. */
	xrt_result_t (*job_execute)(struct xrt_conversion_plugin_instance *inst,
	                            uint32_t cap_id,
	                            const struct xrt_conv_job_in *in,
	                            const struct xrt_conv_params *params,
	                            const struct xrt_conv_job_control *ctl,
	                            struct xrt_conv_job_out *out);

	/*! [S]; MAY NEVER BE CALLED (process exit, satellite kill). */
	void (*destroy)(struct xrt_conversion_plugin_instance *inst);

	/* Append-only below this line, forever (ADR-020). Each appended slot
	 * gets an XRT_CONVERSION_PLUGIN_HAS_<X> macro so the downstream
	 * `features` repin gate sees it. */
};

/*!
 * Signature of the exported XRT_CONVERSION_PLUGIN_ENTRYPOINT_NAME symbol. The
 * provider stores @p host (valid until process end), fills *out_iface (provider-
 * owned storage, valid until process end) and writes its compiled
 * XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT to *out_plugin_api_version; the
 * loader rejects a different major before any dispatch.
 */
typedef xrt_result_t (*xrt_conversion_plugin_negotiate_fn_t)(uint32_t runtime_api_version,
                                                             const struct xrt_conversion_plugin_host_iface *host,
                                                             struct xrt_conversion_plugin_iface **out_iface,
                                                             uint32_t *out_plugin_api_version);

/*
 *
 * Tripwires (ADR-020 rule 4).
 *
 */
// clang-format off
XRT_CONV_ABI_ASSERT(offsetof(struct xrt_conversion_plugin_iface, id) == 8,
                    "conversion iface: 8-byte header, then id");
XRT_CONV_ABI_ASSERT(offsetof(struct xrt_conversion_plugin_iface, get_provider_status) == 8 + 4 * sizeof(void *),
                    "conversion iface: slot base moved - API major bump");
XRT_CONV_ABI_ASSERT(offsetof(struct xrt_conversion_plugin_iface, describe_cap) == 8 + 6 * sizeof(void *),
                    "conversion iface: describe_cap is slot 2");
XRT_CONV_ABI_ASSERT(offsetof(struct xrt_conversion_plugin_iface, warm) == 8 + 9 * sizeof(void *),
                    "conversion iface: warm is slot 5");
XRT_CONV_ABI_ASSERT(offsetof(struct xrt_conversion_plugin_iface, destroy) == 8 + 16 * sizeof(void *),
                    "conversion iface: v1 slot order changed - API major bump");
XRT_CONV_ABI_ASSERT(sizeof(struct xrt_conversion_plugin_iface) == 8 + 17 * sizeof(void *),
                    "conversion iface v1 = 4 strings + 13 slots; append-only past destroy");
XRT_CONV_ABI_ASSERT(sizeof(struct xrt_conversion_plugin_host_iface) == 16 + 16 * sizeof(void *),
                    "conversion host iface size changed - carve from reserved[] instead");
XRT_CONV_ABI_ASSERT(offsetof(struct xrt_conv_capability, struct_size) == 0 &&
                    offsetof(struct xrt_conv_state, struct_size) == 0 &&
                    offsetof(struct xrt_conv_params, struct_size) == 0 &&
                    offsetof(struct xrt_conv_stream_info, struct_size) == 0 &&
                    offsetof(struct xrt_conv_output_desc, struct_size) == 0 &&
                    offsetof(struct xrt_conv_frame_in, struct_size) == 0 &&
                    offsetof(struct xrt_conv_frame_out, struct_size) == 0 &&
                    offsetof(struct xrt_conv_job_in, struct_size) == 0 &&
                    offsetof(struct xrt_conv_job_control, struct_size) == 0 &&
                    offsetof(struct xrt_conv_job_out, struct_size) == 0,
                    "every top-level conversion struct starts with struct_size");
XRT_CONV_ABI_ASSERT(XRT_CONV_STATE_DISABLED == 7u && XRT_CONV_STATE_EVICTED == 10u,
                    "conversion state values are ABI");

/* Frozen embedded structs: no struct_size, explicit padding, fixed size.
 * Every size below is pointer-size independent EXCEPT xrt_conv_image (it
 * holds cpu_ptr), which is asserted on 64-bit targets only (x64 / arm64,
 * LP64 and LLP64 alike: 8-byte pointers, 8-byte-aligned uint64_t). A 32-bit
 * assert is added if the Android APK ships armeabi-v7a (ADR-053 must-verify).
 * Changing one of these sizes is an API major bump. */
XRT_CONV_ABI_ASSERT(sizeof(struct xrt_conv_sync) == 40,
                    "xrt_conv_sync is FROZEN (2 x 4 + 4 x 8)");
XRT_CONV_ABI_ASSERT(sizeof(void *) != 8 || sizeof(struct xrt_conv_image) == 128,
                    "xrt_conv_image is FROZEN (88 + 40-byte sync)");
XRT_CONV_ABI_ASSERT(sizeof(void *) != 8 || offsetof(struct xrt_conv_image, sync) == 88,
                    "xrt_conv_image: sync at 88");
XRT_CONV_ABI_ASSERT(sizeof(struct xrt_conv_depth_info) == 128,
                    "xrt_conv_depth_info is FROZEN (28 x 4 + 2 x 8)");
XRT_CONV_ABI_ASSERT(offsetof(struct xrt_conv_depth_info, source_time_ns) == 112,
                    "xrt_conv_depth_info: no hidden padding before the uint64s");
XRT_CONV_ABI_ASSERT(sizeof(struct xrt_conv_views_info) == 124,
                    "xrt_conv_views_info is FROZEN (3 x 4 + 8 x 3 x 4 + 4 x 4)");
XRT_CONV_ABI_ASSERT(sizeof(struct xrt_conv_splat_info) == 72,
                    "xrt_conv_splat_info is FROZEN (16 x 4 + 8)");
XRT_CONV_ABI_ASSERT(offsetof(struct xrt_conv_splat_info, splat_count) == 64,
                    "xrt_conv_splat_info: splat_count at 64");
XRT_CONV_ABI_ASSERT(sizeof(void *) != 8 || offsetof(struct xrt_conv_frame_out, depth_info) == 648,
                    "xrt_conv_frame_out: explicit reserved_pad, depth_info at 648");
XRT_CONV_ABI_ASSERT(sizeof(void *) != 8 || sizeof(struct xrt_conv_frame_out) == 776,
                    "xrt_conv_frame_out: no hidden padding (8 + 4 x 128 + 124 + 4 + 128)");
XRT_CONV_ABI_ASSERT(offsetof(struct xrt_conv_job_out, views_info) == 208 &&
                    sizeof(struct xrt_conv_job_out) == 336,
                    "xrt_conv_job_out: no hidden padding (8 + 72 + 128 + 124 + 4)");
XRT_CONV_ABI_ASSERT(sizeof(struct xrt_conv_stereo_calibration) == 32,
                    "xrt_conv_stereo_calibration is FROZEN (8 x 4)");
// clang-format on

#ifdef __cplusplus
}
#endif
