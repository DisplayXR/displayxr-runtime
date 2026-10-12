# Conversion providers — header drafts (ADR-053, pre-implementation)

**Status:** Draft, 2026-10-12. Branch `design/conversion-providers`. Nothing here is implemented or frozen.
**Contract:** [`docs/specs/runtime/conversion-provider-discovery.md`](../specs/runtime/conversion-provider-discovery.md) (discovery, lifetime, hosting, diagnostics). **Decision:** [ADR-053](../adr/ADR-053-conversion-providers.md) (Proposed).

This document publishes the C-level surface of conversion providers **before** implementation, the way [`spatial-workspace-extensions-headers-draft.md`](spatial-workspace-extensions-headers-draft.md) did for the workspace extensions: full enough that an implementer or reviewer can argue with specifics rather than prose.

**What is normative and what is draft.**
- The *rules* — threading, hosting-agnosticism, struct-size and append-only discipline, the consumer contract, the legacy mapping — are normative. They live in the discovery spec and in ADR-053; this file restates them only where a header comment carries them.
- The *headers* below are drafts. Names, field order and values may change until each lands: `xrt_conversion_plugin.h` freezes at P1 (`XRT_CONVERSION_PLUGIN_API_VERSION` 1); `XR_DXR_lift` spec 4 freezes at P3, spec 5 at P6.
- Where a draft here disagrees with the discovery spec, the spec wins.

**Type values (ADR-053).** `1004999340`–`369` (three decades) are proposed for lift spec 4 / 5. `1004999340` (formerly the provider-properties struct) is now **unused**: provider enumeration was cut from the app API (provider identity stays in the CLI and the status snapshot), and the value is left free rather than renumbering the rest. The decades are reserved by a **registry-only PR** (`src/external/openxr_includes/openxr/README.md`) at the start of P3 and re-checked at merge time; if any value has been taken by then, the draft is renumbered to the next three free decades. Spec 4 is unreleased, so renumbering is safe. (The `stereo_camera` and `cursor_depth` decades were both taken while their PRs were in flight.)

**Byte-sync rule.** When these land, every consumer's vendored copy of `XR_DXR_lift.h` (the browser, the web SDK's native path, the engine plug-ins, the demos) is **byte-synced** with the runtime's. Coupled-PR merge order is **runtime → `displayxr-extensions` (auto-synced mirror) → consumers**; a consumer never merges against a header the runtime has not shipped.

**Naming split (ADR-053).** The internal word is "conversion" (`xrt_conversion_plugin.h`, `displayxr-cli conv`, the `conv_*` wire family); the app-facing word is "lift" (`XR_DXR_lift` spec 4 / 5). There is no `XR_DXR_conversion` extension.

---

## 1. `src/xrt/include/xrt/xrt_conversion_plugin.h` (plug-in ABI, P1)

Self-contained (only `xrt_results.h` + `<stdint.h>` / `<stddef.h>` / `<stdbool.h>`), with no graphics-API types: handles are `uint64_t`, so the layout is the same on every OS. Every enumerated field is a `uint32_t` plus `#define`s. Edits from the design draft (ADR-053, after the red-team pass):
- `describe` is now `describe_cap(inst, index, out, out_count)` — one capability per call, so appending a field to `xrt_conv_capability` can never mis-stride an array.
- Every struct embedded **by value** in another (`xrt_conv_sync`, `xrt_conv_image`, `xrt_conv_depth_info`, `xrt_conv_views_info`, `xrt_conv_splat_info`, `xrt_conv_stereo_calibration`) is **frozen**: no `struct_size`, explicit padding, and a `sizeof` tripwire. Growth goes at the tail of the enclosing top-level struct or into a new iface slot. Top-level structs keep `struct_size`.
- `XRT_CONV_MAX_VIEWS` is 8, equal to `XR_LIFT_MAX_VIEWS_DXR`; `XRT_CONV_VIEWS_ATLAS` is reserved.
- Threading is one normative rule (one call in flight per provider) with three opt-ins: `max_concurrent`, `XRT_CONV_CAP_LANES_CONCURRENT` (bit 7) and the new `XRT_CONV_CAP_WARM_CONCURRENT` (bit 8).
- Cancellation is a `bool (*is_cancelled)(void *ctx)` callback, not a `volatile` pointer.
- `xrt_conv_state_coarse()` and `exclusive_group` are deleted. `XRT_CONV_INPUT_ENCODED`, `SPLATS_VIDEO`, `INPAINT` and `SEQUENCE_MANIFEST` are reserved values with no code in v1.
- `XRT_CONV_FMT_R16_UNORM` (10) is appended so the app-facing pixel-format enum can carry the same values.

```c
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

/* ───────────────────────── Versioning, entry point ───────────────────────── */

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
#define XRT_CONV_COVERS(PTR, TYPE, FIELD)                                                          \
	((PTR) != NULL && (PTR)->struct_size >= offsetof(TYPE, FIELD) + sizeof(((TYPE *)0)->FIELD))
/*! Coverage + non-NULL, for an appended iface slot. */
#define XRT_CONV_IFACE_HAS(IFACE, FIELD)                                                           \
	(XRT_CONV_COVERS(IFACE, struct xrt_conversion_plugin_iface, FIELD) && (IFACE)->FIELD != NULL)

#ifndef XRT_CONV_ABI_ASSERT
#if defined(__cplusplus)
#define XRT_CONV_ABI_ASSERT(c, m) static_assert(c, m)
#else
#define XRT_CONV_ABI_ASSERT(c, m) _Static_assert(c, m)
#endif
#endif

/* Ceilings (a provider reports its own, smaller, limits). */
#define XRT_CONV_MAX_VIEWS 8          //!< == XR_LIFT_MAX_VIEWS_DXR. Never edited: it sizes frozen
                                      //!< arrays. A higher ceiling is a later slot + tail fields.
#define XRT_CONV_MAX_CAPABILITIES 32  //!< per provider
#define XRT_CONV_MAX_FRAME_IMAGES 4   //!< inputs per stream frame / job
#define XRT_CONV_NAME_MAX 64
#define XRT_CONV_DETAIL_MAX 128

struct xrt_conversion_plugin_instance; //!< opaque, provider-defined (may be NULL)

/* ───────────────────────── Vocabulary (frame D3) ─────────────────────────── */

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
#define XRT_CONV_INPUT_ENCODED (1u << 3)     //!< RESERVED — no code in v1 (job lane, encoded bytes; the runtime sends decoded pixels)
/*! @} */

/*! @name Output bits @{ */
#define XRT_CONV_OUT_VIEWS (1u << 0)
#define XRT_CONV_OUT_DEPTH (1u << 1)      //!< primary for DEPTH_*; same-inference aux for VIEWS_*
#define XRT_CONV_OUT_CONFIDENCE (1u << 2)
#define XRT_CONV_OUT_DISPARITY (1u << 3)  //!< DISPARITY_STEREO primary
#define XRT_CONV_OUT_SPLAT_BLOB (1u << 4) //!< SPLATS_* (blob sink)
/*! @} */

/* ───────────────────────── Image transport (frame D6) ────────────────────── */

/*! @name Memory kinds (bits in import masks; exactly one bit in an image) @{ */
#define XRT_CONV_MEM_CPU (1u << 0)              //!< host memory (shm when hosted out of the service)
#define XRT_CONV_MEM_WIN32_NT_HANDLE (1u << 1)  //!< D3D11/D3D12 shared NT handle
#define XRT_CONV_MEM_WIN32_KMT_HANDLE (1u << 2) //!< legacy D3D11 shared handle (same adapter only)
#define XRT_CONV_MEM_VK_OPAQUE_FD (1u << 3)
#define XRT_CONV_MEM_VK_OPAQUE_WIN32 (1u << 4)
#define XRT_CONV_MEM_DMABUF (1u << 5)           //!< + drm_modifier
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
#define XRT_CONV_CS_SRGB 1u          //!< sRGB-encoded values (in a UNORM or SRGB format)
#define XRT_CONV_CS_LINEAR 2u
#define XRT_CONV_CS_BT709_LIMITED 3u //!< YUV video range
#define XRT_CONV_CS_BT709_FULL 4u
#define XRT_CONV_CS_BT2020_PQ 5u
#define XRT_CONV_CS_DATA 6u          //!< not colour (depth / disparity / confidence)
/*! @} */

/*! A sync primitive attached to an image. The handle is BORROWED for the
 *  call; a provider that needs it longer duplicates it. Equal import_key ⇒
 *  same primitive (cache the import).
 *  FROZEN: growth goes at the tail of the enclosing top-level struct or into
 *  a new slot. sizeof == 40 (asserted). */
struct xrt_conv_sync
{
	uint32_t kind;          //!< XRT_CONV_SYNC_*
	uint32_t reserved_0;    //!< explicit padding; 0
	uint64_t handle;        //!< NT HANDLE / fd / port, widened
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
	uint32_t memory_kind;   //!< exactly one XRT_CONV_MEM_* bit; 0 = "not present / not requested"
	uint32_t reserved_0;    //!< explicit padding; 0
	uint64_t handle;        //!< shareable handle (not CPU), borrowed for the call
	uint64_t import_key;    //!< runtime-unique per allocation; same key ⇒ same memory
	void *cpu_ptr;          //!< XRT_CONV_MEM_CPU only, mapped in the calling process
	uint64_t byte_size;
	uint32_t width, height; //!< texels (plane 0)
	uint32_t format;        //!< XRT_CONV_FMT_*
	uint32_t color_space;   //!< XRT_CONV_CS_*
	uint32_t row_stride;    //!< bytes for CPU / linear; 0 for opaque GPU textures
	uint32_t plane1_offset; //!< NV12 chroma (CPU / linear)
	uint32_t plane1_stride;
	uint32_t array_layer;   //!< layer of an array image (ADR-032); 0 for flat
	uint64_t drm_modifier;  //!< DMABUF only
	uint64_t adapter_luid;  //!< adapter the memory lives on; 0 = CPU / unknown
	struct xrt_conv_sync sync;
};

/* ───────────────────────── Readiness (frame D8) ──────────────────────────── */

/*! @name Capability state. 0..7 are NUMERICALLY IDENTICAL to
 *  XRT_DP_LIFT_MODULE_* (asserted in oxr_plugin_stub.c) so the legacy
 *  adapter copies module_status verbatim. 8.. are new, append-only. @{ */
#define XRT_CONV_STATE_UNKNOWN 0u
#define XRT_CONV_STATE_READY 1u        //!< resident; converts now
#define XRT_CONV_STATE_WARMING 2u      //!< load / licence / compile in progress; progress valid
#define XRT_CONV_STATE_ABSENT 3u       //!< its dependency or model is not installed
#define XRT_CONV_STATE_INCOMPATIBLE 4u //!< installed but unusable (version / OS / GPU / driver)
#define XRT_CONV_STATE_UNLICENSED 5u
#define XRT_CONV_STATE_FAILED 6u       //!< failure_reason says which
#define XRT_CONV_STATE_DISABLED 7u     //!< configuration
#define XRT_CONV_STATE_INSTALLED 8u    //!< present and cold; warm() makes it READY
#define XRT_CONV_STATE_DOWNLOADING 9u  //!< model bytes in flight; progress valid
#define XRT_CONV_STATE_EVICTED 10u     //!< was READY; unloaded (idle)
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
	uint64_t eta_ns;         //!< 0 = unknown
	char version[48];        //!< dependency / model version actually resolved
	char detail[XRT_CONV_DETAIL_MAX]; //!< for a human, verbatim, never parsed
};

/* ───────────────────────── Capability descriptor (frame D2) ──────────────── */

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
#define XRT_CONV_MEASURE_PLANAR 0u //!< to the image plane (z)
#define XRT_CONV_MEASURE_RAY 1u    //!< along the ray

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
#define XRT_CONV_CAP_WARM_CONCURRENT (1u << 8)       //!< warm may overlap converts; else serialised (stall reported in stats)
/*! @} */

/*! One capability = (task × model × backend × device). Top-level: the
 *  runtime zero-fills ONE struct and sets struct_size before each
 *  describe_cap(); the provider writes within struct_size. A process-wide
 *  model choice is one capability whose variant follows the quality hint,
 *  not a group of mutually exclusive capabilities. */
struct xrt_conv_capability
{
	uint32_t struct_size;
	uint32_t cap_id;          //!< provider-chosen, non-zero, stable for the instance's life
	uint32_t task;            //!< XRT_CONV_TASK_*
	uint32_t lanes;           //!< XRT_CONV_LANE_*
	uint32_t input_kinds;     //!< XRT_CONV_INPUT_*
	uint32_t outputs;         //!< XRT_CONV_OUT_* it CAN produce
	uint32_t flags;           //!< XRT_CONV_CAP_*

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
	uint32_t depth_kind;           //!< XRT_CONV_DEPTH_* of the primary / aux depth
	uint32_t depth_measure;        //!< XRT_CONV_MEASURE_*
	uint32_t intrinsics_source;    //!< what results will carry
	uint32_t splat_formats;        //!< 1u << XRT_CONV_BLOB_*

	/* Licence + identity (diagnostics; never shown to apps as names). */
	uint32_t licence_class;        //!< XRT_CONV_LICENCE_*
	uint32_t commercial_ok;        //!< 1 = model AND every dependency permit commercial use
	char licence_spdx[48];         //!< "MIT", "Apache-2.0", "LicenseRef-<owner>"
	char model_id[XRT_CONV_NAME_MAX];
	char model_version[32];
	char backend[32];              //!< e.g. "onnxruntime-dml", "vendor-native"
	char display_name[XRT_CONV_NAME_MAX];
	uint32_t reserved_0;           //!< explicit padding before device_luid; 0

	/* Device + cost estimates (0 = unknown). */
	uint64_t device_luid;          //!< adapter it runs on once warm; 0 = CPU / undecided
	uint32_t est_warmup_ms;
	uint32_t est_latency_ms;       //!< per convert at preferred_input_edge, or per job
	uint32_t est_vram_mb;
	uint32_t est_ram_mb;
};

/* ───────────────────────── Parameters ────────────────────────────────────── */

/* Equal to xrt_dp_lift.h where they overlap (asserted in oxr_plugin_stub.c). */
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
	uint32_t eye_order;       //!< 0 = left first, 1 = right first
	float fx, fy, cx, cy;     //!< px of ONE rectified eye
	float baseline_m;         //!< 0 = UNKNOWN; never defaulted
	float infinity_shift_px;  //!< disparity at infinity after rectification
	uint32_t sbs_packed;      //!< 1 = both eyes in one side-by-side image
};

/*! Everything the runtime decided (policy is runtime-owned, D10). */
struct xrt_conv_params
{
	uint32_t struct_size;
	uint32_t frame_flags;     //!< XRT_CONV_FRAME_*
	uint64_t source_time_ns;  //!< echoed in results
	uint64_t frame_id;        //!< per stream, increasing

	/* Look. */
	uint32_t convergence_mode; //!< XRT_CONV_CONVERGENCE_*
	float convergence;        //!< DECODED mode: the zero-parallax plane in the DECODED units of this
	                          //!< stream's depth output (value = value_scale * sample + value_shift, the
	                          //!< mirror of depth_info.convergence_value). Polarity follows the depth
	                          //!< kind: larger = nearer for *_INVERSE / *_DISPARITY, larger = farther
	                          //!< for METRIC_DEPTH / AFFINE. The runtime maps an app's [0,1]
	                          //!< "relative depth at glass" onto this using the last result's range;
	                          //!< the provider never normalises it again. A given plane also means
	                          //!< "provider auto-convergence OFF".
	float strength;           //!< 1 = provider budget
	uint32_t view_count;      //!< VIEWS_*: validated against min/max_views
	uint32_t content_hint;    //!< XRT_CONV_CONTENT_*
	float input_scale;        //!< ADVISORY; the runtime already applied max_input_edge
	float focal_px;           //!< input px; ≤ 0 = unknown (never invented)

	/* Viewpoint policy (ADR-048), already applied. */
	uint32_t viewpoint_frame;
	uint32_t viewpoint_source;
	uint32_t axis_mode;
	uint32_t viewpoint_count; //!< == view_count for VIEWS_*; else 0
	float viewpoints_xyz[XRT_CONV_MAX_VIEWS][3]; //!< metres, rect-relative, +z toward viewer
	float rect_width_m, rect_height_m;
	float baseline_m;
	float max_offset_m;
	float nominal_z_m;

	/* Stereo input (DISPARITY_STEREO, SPLATS_STEREO). */
	struct xrt_conv_stereo_calibration stereo;
};

/* ───────────────────────── Results (frame D9) ────────────────────────────── */

/*! FROZEN: growth goes at the tail of the enclosing top-level struct or into
 *  a new slot. sizeof == 128 (asserted). */
struct xrt_conv_depth_info
{
	uint32_t kind;                 //!< XRT_CONV_DEPTH_*
	uint32_t measure;              //!< XRT_CONV_MEASURE_*
	uint32_t intrinsics_source;
	float value_scale, value_shift; //!< value = scale * sample + shift (scale 0 read as 1)
	float near_value, far_value;   //!< decoded; far < near ⇒ infinite far; 0,0 = unknown
	float invalid_sample;          //!< raw "no data" value; NaN = none
	float range_min, range_max;    //!< decoded per-frame extent BEFORE any normalisation
	float range_p05, range_p95;    //!< 0,0 = not computed
	float fx, fy, cx, cy;          //!< depth-image texels; ≤ 0 = unknown
	uint32_t source_width, source_height;
	float convergence_value;       //!< decoded value at zero disparity of the paired views; valid iff (suggest_flags & 4)
	float suggested_convergence;   //!< provider hint, DECODED units (see params.convergence); valid iff (suggest_flags & 1)
	float suggested_strength;      //!< provider hint for the disparity budget, 1 = its own; valid iff (suggest_flags & 2).
	                               //!< Both hints are IGNORED by policy unless the provider is validated against the corpus.
	uint32_t suggest_flags;        //!< bit0 suggested_convergence, bit1 suggested_strength, bit2 convergence_value
	uint32_t relief_valid;         //!< ADR-046 A1: z_front_m = relief_scale/value + relief_offset
	float relief_scale, relief_offset;
	uint32_t temporal;             //!< from a temporal model
	uint32_t same_inference;       //!< same inference as this frame's views
	uint32_t has_confidence;
	uint64_t source_time_ns;
	uint64_t provider_frame_id;    //!< informational
};

#define XRT_CONV_VIEWS_ROW 0u   //!< N views side by side, view 0 leftmost (today's SBS / NVIEW)
#define XRT_CONV_VIEWS_ARRAY 1u //!< view i in array layer i (ADR-032)
#define XRT_CONV_VIEWS_ATLAS 2u //!< RESERVED: columns × rows, row-major. The runtime picks ATLAS or
                                //!< ARRAY when N × w exceeds the texture limit (not reachable at 8 views)

/*! FROZEN: growth goes at the tail of the enclosing top-level struct or into
 *  a new slot. sizeof == 124 (asserted; XRT_CONV_MAX_VIEWS 8). */
struct xrt_conv_views_info
{
	uint32_t view_count;
	uint32_t layout;               //!< must equal the target's layout
	uint32_t reserved_0;           //!< 0
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
#define XRT_CONV_COORDS_OPENCV 1u //!< +x right, +y down, +z forward, metres
#define XRT_CONV_COORDS_RUB 2u    //!< +x right, +y up, +z back

/*! FROZEN: growth goes at the tail of the enclosing top-level struct or into
 *  a new slot. sizeof == 72 (asserted). */
struct xrt_conv_splat_info
{
	uint32_t blob_format;
	uint32_t coords;              //!< XRT_CONV_COORDS_*
	uint32_t color_space;         //!< tag honestly
	uint32_t metric;              //!< 1 = metres are real
	uint32_t intrinsics_source;
	float fx, fy, cx, cy;         //!< of the grid actually unprojected through
	uint32_t grid_width, grid_height;
	float baseline_m;             //!< 0 = unknown; never defaulted
	float focus_z_m, near_m, far_m; //!< 0 = unknown
	uint32_t reserved_0;          //!< explicit padding before splat_count; 0
	uint64_t splat_count;
};

/* ───────────────────────── Stream lane ───────────────────────────────────── */

struct xrt_conv_stream_info
{
	uint32_t struct_size;
	uint32_t input_kind;          //!< one XRT_CONV_INPUT_* bit
	uint32_t outputs;             //!< subset of cap.outputs the runtime will request
	uint32_t content_hint;
	uint32_t view_count;          //!< VIEWS_*: fixed for the stream
	uint32_t views_layout;        //!< XRT_CONV_VIEWS_*
	uint32_t max_input_width, max_input_height;
};

/*! Output extents for a given input extent — pure, cheap, any thread. */
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

#define XRT_CONV_IMG_COLOR 0u
#define XRT_CONV_IMG_COLOR_RIGHT 1u //!< second eye when not sbs_packed
#define XRT_CONV_IMG_DEPTH_IN 2u    //!< VIEWS_FROM_DEPTH (meaning in depth_in_info)

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
	uint32_t written;             //!< XRT_CONV_OUT_* actually written
	struct xrt_conv_image views;
	struct xrt_conv_image depth;
	struct xrt_conv_image confidence;
	struct xrt_conv_image disparity;
	struct xrt_conv_views_info views_info;
	uint32_t reserved_pad;         //!< explicit padding before depth_info (8-aligned); 0
	struct xrt_conv_depth_info depth_info;
};

/* ───────────────────────── Job lane ──────────────────────────────────────── */

/*! Runtime-owned byte sink (shm, file or IPC behind it). false = stop. */
struct xrt_conv_blob_sink
{
	uint32_t struct_size;
	uint32_t role;                //!< XRT_CONV_OUT_* this sink receives
	void *ctx;
	bool (*begin)(void *ctx, uint32_t blob_format, uint64_t size_hint);
	bool (*write)(void *ctx, const void *bytes, uint64_t count);
};

struct xrt_conv_job_in
{
	uint32_t struct_size;
	uint32_t input_kind;
	uint32_t image_count;
	uint32_t encoded_count;       //!< RESERVED (XRT_CONV_INPUT_ENCODED, no code in v1): 0
	uint32_t image_role[XRT_CONV_MAX_FRAME_IMAGES];
	struct xrt_conv_image images[XRT_CONV_MAX_FRAME_IMAGES];
	const void *encoded[2];       //!< RESERVED: NULL in v1
	uint64_t encoded_size[2];     //!< RESERVED: 0 in v1
	uint32_t requested_blob_format; //!< XRT_CONV_BLOB_*; 0 = provider default
	uint32_t reserved_0;
};

struct xrt_conv_job_control
{
	uint32_t struct_size;
	uint32_t sink_count;
	struct xrt_conv_blob_sink *sinks;
	void *ctx;                    //!< passed to progress and is_cancelled
	void (*progress)(void *ctx, float fraction, const char *stage); //!< runtime throttles
	/*! Poll at least every 250 ms; true ⇒ stop and return. Thread-safe,
	 *  non-blocking, cheap; once true it stays true for the call. In a
	 *  satellite the host's pipe thread is what sets the state it reads. */
	bool (*is_cancelled)(void *ctx);
};

struct xrt_conv_job_out
{
	uint32_t struct_size;
	uint32_t written;             //!< XRT_CONV_OUT_* delivered to sinks
	struct xrt_conv_splat_info splat_info;
	struct xrt_conv_depth_info depth_info;
	struct xrt_conv_views_info views_info;
	uint32_t reserved_pad;         //!< explicit tail padding (8-aligned size); 0
};

/* ───────────────────────── Provider status (ADR-045 shape) ───────────────── */

/*! Same layout and state values as xrt_plugin_platform_status (asserted in
 *  oxr_plugin_stub.c), restated so this header stays self-contained.
 *  "Platform" = the provider's own dependencies (a vendor conversion
 *  runtime, an inference runtime, a driver), never a display. */
struct xrt_conv_provider_status
{
	uint32_t struct_size;
	uint32_t state;   //!< xrt_plugin_platform_state values
	uint32_t flags;   //!< XRT_CONV_PROVIDER_FLAG_*
	uint32_t reserved_0;
	char hint[128];
};
#define XRT_CONV_PROVIDER_FLAG_FALLBACK (1u << 0)  //!< sim provider only; never chosen over a real cap
#define XRT_CONV_PROVIDER_FLAG_REFERENCE (1u << 1) //!< runtime-shipped vendor-neutral reference

/* ───────────────────────── Host iface ────────────────────────────────────── */

#define XRT_CONV_PATH_MODEL_STORE 1u //!< shared content-addressed store root (read-only to providers)
#define XRT_CONV_PATH_SCRATCH 2u     //!< per-provider writable dir
#define XRT_CONV_PATH_LOGS 3u        //!< informational

struct xrt_conv_adapter_hint
{
	uint32_t struct_size;
	uint32_t flags;          //!< bit0: preferred is the panel's render adapter; bit1: preferred is integrated
	uint64_t preferred_luid; //!< 0 = no preference
	uint64_t avoid_luid;     //!< 0 = none
};

struct xrt_conv_model_location
{
	uint32_t struct_size;
	uint32_t state;          //!< READY / DOWNLOADING / ABSENT / FAILED
	float progress;
	uint32_t failure_reason;
	char path[512];          //!< UTF-8 absolute path of the verified blob when READY
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
	uint32_t host_flags;     //!< bit0: hosted out of the service (satellite); bit1: headless / CI
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

/* ───────────────────────── Provider iface ────────────────────────────────── */

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
	const char *id;           //!< == registry / manifest <id>
	const char *display_name;
	const char *vendor;       //!< may be NULL
	const char *version;      //!< may be NULL

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

typedef xrt_result_t (*xrt_conversion_plugin_negotiate_fn_t)(uint32_t runtime_api_version,
                                                             const struct xrt_conversion_plugin_host_iface *host,
                                                             struct xrt_conversion_plugin_iface **out_iface,
                                                             uint32_t *out_plugin_api_version);

/* ───────────────────────── Tripwires (ADR-020 rule 4) ────────────────────── */
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
```

The threading contract, the hosting-agnosticism rules and the semantics the header cannot carry (honesty, viewpoints, convergence, per-stream temporal state) are in the discovery spec §2.3–2.5.

**Frozen-struct sizes (64-bit), for review.**

| Struct | Layout (byte offsets) | `sizeof` |
|---|---|---|
| `xrt_conv_sync` | 0 `kind`, 4 `reserved_0`, 8 `handle`, 16 `import_key`, 24 `acquire_value`, 32 `release_value` | 40 |
| `xrt_conv_image` | 0 `memory_kind`, 4 `reserved_0`, 8 `handle`, 16 `import_key`, 24 `cpu_ptr`, 32 `byte_size`, 40–68 eight `uint32_t` (`width` … `array_layer`), 72 `drm_modifier`, 80 `adapter_luid`, 88 `sync` (40) | 128 |
| `xrt_conv_depth_info` | 0–108 twenty-eight 4-byte fields (`kind` … `has_confidence`), 112 `source_time_ns`, 120 `provider_frame_id` | 128 |
| `xrt_conv_views_info` | 0 `view_count`, 4 `layout`, 8 `reserved_0`, 12 `eye_xyz[8][3]` (96), 108 `baseline_m`, 112 `hfov_deg`, 116 `zero_parallax_disparity`, 120 `reserved_1` | 124 |
| `xrt_conv_splat_info` | 0–56 fifteen 4-byte fields (`blob_format` … `far_m`), 60 `reserved_0`, 64 `splat_count` | 72 |
| `xrt_conv_stereo_calibration` | 0 `eye_order`, 4–16 `fx fy cx cy`, 20 `baseline_m`, 24 `infinity_shift_px`, 28 `sbs_packed` | 32 |

`xrt_conv_capability` (top-level) gains an explicit `reserved_0` before `device_luid`: removing `exclusive_group` would otherwise leave 4 bytes of hidden padding there.

The two top-level out-structs that embed frozen structs carry explicit padding too, so every byte is accounted for and both are pinned (they still grow at the tail, under `struct_size`):

| Struct | Layout (byte offsets, 64-bit) | `sizeof` |
|---|---|---|
| `xrt_conv_frame_out` | 0 `struct_size`, 4 `written`, 8 / 136 / 264 / 392 `views` / `depth` / `confidence` / `disparity` (128 each), 520 `views_info` (124), 644 `reserved_pad`, 648 `depth_info` (128) | 776 |
| `xrt_conv_job_out` | 0 `struct_size`, 4 `written`, 8 `splat_info` (72), 80 `depth_info` (128), 208 `views_info` (124), 332 `reserved_pad` | 336 |

**As landed (P1a-1, #1998).** `src/xrt/include/xrt/xrt_conversion_plugin.h` is this block verbatim apart from house-style section separators, Doxygen groups, clang-format layout (three long trailing comments moved above their `#define`) and the two `reserved_pad` fields above; the `xrt_conv_depth_info` comment that read `sizeof == 120` was a typo for the asserted 128. Pinned by `tests/tests_conversion_abi.cpp`.

---

## 2. `XR_DXR_lift.h` spec 4 additions (P3)

Spec 4 adds the task model, task enumeration, per-task readiness, a warm request, state events, task-typed stream creation, a platform-neutral image submit, a result export descriptor, `XrLiftDepthInfoDXR` / `XrLiftViewsInfoDXR`, and frame-discontinuity control. Every v3 struct, enum value and entry point stays byte-identical; apps detect spec 4 with `extensionVersion >= 4`. Every new enum that has a plug-in ABI counterpart (task state, licence class, depth kind, depth measure, pixel format, colour space, views layout, intrinsics source, coordinate system, blob format) uses **the plug-in ABI's values**, so no boundary needs a hand-written mapping; `oxr_plugin_stub.c` static-asserts every pair. Provider identity is not app-visible: there is no provider enumeration (it lives in `displayxr-cli conv` and the status snapshot). Non-D3D transports (P5) need no version bump: platform availability is capability data (`imageKinds` / `exportKinds`).

### 2.1 Type values (spec 4 and spec 5)

```c
// ---- Spec v4 (conversion providers: tasks, readiness, neutral transport) ----
// Reserved 1004999340..369 (three decades). Registry: README.md in this dir.
// 1004999340 unused: was XR_TYPE_LIFT_PROVIDER_PROPERTIES_DXR (provider enumeration
// cut from the app API). Left free; the rest of the decade is not renumbered.
#define XR_TYPE_LIFT_TASK_PROPERTIES_DXR               ((XrStructureType)1004999341)
#define XR_TYPE_LIFT_TASK_STREAM_CREATE_INFO_DXR       ((XrStructureType)1004999342)
#define XR_TYPE_LIFT_FRAME_IMAGES_DXR                  ((XrStructureType)1004999343)
#define XR_TYPE_LIFT_IMAGE_DXR                         ((XrStructureType)1004999344)
#define XR_TYPE_LIFT_IMAGE_DMABUF_DESC_DXR             ((XrStructureType)1004999345)
#define XR_TYPE_LIFT_RESULT_EXPORT_DXR                 ((XrStructureType)1004999346)
#define XR_TYPE_LIFT_DEPTH_INFO_DXR                    ((XrStructureType)1004999347)
#define XR_TYPE_LIFT_VIEWS_INFO_DXR                    ((XrStructureType)1004999348)
#define XR_TYPE_LIFT_FRAME_CONTROL_DXR                 ((XrStructureType)1004999349)
#define XR_TYPE_EVENT_DATA_LIFT_TASK_STATE_CHANGED_DXR ((XrStructureType)1004999350)
#define XR_TYPE_EVENT_DATA_LIFT_PROVIDERS_CHANGED_DXR  ((XrStructureType)1004999351)
// 352..359 free for v4 follow-ups (e.g. a confidence-plane result).
// ---- Spec v5 (jobs, splats, stereo + depth input) ----
#define XR_TYPE_LIFT_JOB_CREATE_INFO_DXR               ((XrStructureType)1004999360)
#define XR_TYPE_LIFT_JOB_STATE_DXR                     ((XrStructureType)1004999361)
#define XR_TYPE_LIFT_JOB_RESULT_DXR                    ((XrStructureType)1004999362)
#define XR_TYPE_LIFT_JOB_FILE_OUTPUT_DXR               ((XrStructureType)1004999363)
#define XR_TYPE_LIFT_SPLATS_INFO_DXR                   ((XrStructureType)1004999364)
#define XR_TYPE_LIFT_STEREO_INPUT_DXR                  ((XrStructureType)1004999365)
#define XR_TYPE_LIFT_STREAM_STEREO_SOURCE_DXR          ((XrStructureType)1004999366) // RESERVED — no code in v1
#define XR_TYPE_EVENT_DATA_LIFT_JOB_STATE_CHANGED_DXR  ((XrStructureType)1004999367)
//! XrObjectType of an XrLiftJobDXR (debug-utils naming).
#define XR_OBJECT_TYPE_LIFT_JOB_DXR                    ((XrObjectType)1004999368)
// 369 free.
```

### 2.2 Task vocabulary, states, licence

```c
/*!
 * WHAT to compute (frame D3). Fixed vocabulary; values are ABI. A task is
 * served by at most one provider at a time, chosen by the runtime, never by
 * the app. Reserved values are never accepted by spec 4/5 runtimes.
 */
typedef enum XrLiftTaskDXR {
    XR_LIFT_TASK_DEPTH_MONO_DXR        = 1,  //!< one image -> depth (stills; no temporal state)
    XR_LIFT_TASK_DEPTH_VIDEO_DXR       = 2,  //!< frames -> depth, temporally stable; reset on discontinuity
    XR_LIFT_TASK_DISPARITY_STEREO_DXR  = 3,  //!< rectified pair + calibration -> disparity / depth (spec 5)
    XR_LIFT_TASK_VIEWS_MONO_DXR        = 4,  //!< image/frames -> N >= 2 views (+ same-inference aux depth)
    XR_LIFT_TASK_VIEWS_FROM_DEPTH_DXR  = 5,  //!< image + depth -> N views (spec 5)
    XR_LIFT_TASK_SPLATS_MONO_DXR       = 6,  //!< photo -> 3D Gaussian splats (job lane)
    XR_LIFT_TASK_SPLATS_STEREO_DXR     = 7,  //!< pair + calibration -> metric splats (spec 5, job lane)
    XR_LIFT_TASK_SPLATS_VIDEO_DXR      = 8,  //!< RESERVED — no code in v1 (sequence -> splat sequence)
    XR_LIFT_TASK_INPAINT_DXR           = 9,  //!< RESERVED — no code in v1 (provider sub-stage, never app-creatable)
    XR_LIFT_TASK_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftTaskDXR;

/*! The shape of the input a task stream / job consumes. */
typedef enum XrLiftInputKindDXR {
    XR_LIFT_INPUT_KIND_MONO_DXR            = 1, //!< one colour image
    XR_LIFT_INPUT_KIND_STEREO_PAIR_DXR     = 2, //!< rectified L/R pair (+ XrLiftStereoInputDXR), spec 5
    XR_LIFT_INPUT_KIND_IMAGE_AND_DEPTH_DXR = 3, //!< colour + depth image (+ XrLiftDepthInfoDXR), spec 5
    XR_LIFT_INPUT_KIND_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftInputKindDXR;
typedef XrFlags64 XrLiftInputKindFlagsDXR;     //!< bit (1 << XrLiftInputKindDXR)

/*!
 * Readiness of a task's selected provider (frame D8). Values equal the plug-in
 * ABI's (XRT_CONV_STATE_*; 0..7 also equal XRT_DP_LIFT_MODULE_*); asserted in
 * oxr_plugin_stub.c.
 *
 * Waited for (a create succeeds and its frames wait, latest-wins):
 * INSTALLED, DOWNLOADING, WARMING, EVICTED. Terminal for NEW creates
 * (selection falls through to the next provider, else the create is refused):
 * ABSENT, INCOMPATIBLE, UNLICENSED, FAILED (any reason), DISABLED. UNKNOWN is
 * reported only before the runtime has described any provider for the task.
 */
typedef enum XrLiftTaskStateDXR {
    XR_LIFT_TASK_STATE_UNKNOWN_DXR      = 0,  //!< discovery not settled; treat as waiting
    XR_LIFT_TASK_STATE_READY_DXR        = 1,  //!< conversions run now
    XR_LIFT_TASK_STATE_WARMING_DXR      = 2,  //!< loading / compiling / first inference; progress may be valid
    XR_LIFT_TASK_STATE_ABSENT_DXR       = 3,  //!< no provider can serve this task (or a download is forbidden by policy)
    XR_LIFT_TASK_STATE_INCOMPATIBLE_DXR = 4,  //!< installed but unusable here (version / OS / GPU / driver)
    XR_LIFT_TASK_STATE_UNLICENSED_DXR   = 5,  //!< its licence check refused it
    XR_LIFT_TASK_STATE_FAILED_DXR       = 6,  //!< see XrLiftFailureReasonDXR; may recover later (event)
    XR_LIFT_TASK_STATE_DISABLED_DXR     = 7,  //!< switched off by user / policy
    XR_LIFT_TASK_STATE_INSTALLED_DXR    = 8,  //!< provider present, model cold; a stream / job / warm request starts warming
    XR_LIFT_TASK_STATE_DOWNLOADING_DXR  = 9,  //!< model being fetched; progress valid
    XR_LIFT_TASK_STATE_EVICTED_DXR      = 10, //!< was READY, unloaded after long idle; a reload reports WARMING
    XR_LIFT_TASK_STATE_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftTaskStateDXR;

typedef enum XrLiftFailureReasonDXR {
    XR_LIFT_FAILURE_REASON_NONE_DXR     = 0,
    XR_LIFT_FAILURE_REASON_LICENCE_DXR  = 1, //!< licence missing / expired / not opted in
    XR_LIFT_FAILURE_REASON_LOAD_DXR     = 2, //!< provider or model failed to load
    XR_LIFT_FAILURE_REASON_PLATFORM_DXR = 3, //!< GPU / OS / backend not supported
    XR_LIFT_FAILURE_REASON_NETWORK_DXR  = 4, //!< download failed
    XR_LIFT_FAILURE_REASON_VERSION_DXR  = 5, //!< provider or its engine outside the supported range
    XR_LIFT_FAILURE_REASON_RESOURCE_DXR = 6, //!< not enough memory / VRAM
    XR_LIFT_FAILURE_REASON_MODEL_DXR    = 7, //!< model missing, bad hash or unsupported
    XR_LIFT_FAILURE_REASON_DEVICE_LOST_DXR = 8, //!< GPU device lost; the runtime re-warms (events follow)
    XR_LIFT_FAILURE_REASON_CRASHED_DXR  = 9, //!< provider host crash-loop cutoff; a single crash reads as WARMING
    // values equal XRT_CONV_FAIL_*; asserted in oxr_plugin_stub.c
    XR_LIFT_FAILURE_REASON_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftFailureReasonDXR;

/*! Values equal the plug-in ABI's (XRT_CONV_LICENCE_*); asserted in oxr_plugin_stub.c. */
typedef enum XrLiftLicenceClassDXR {
    XR_LIFT_LICENCE_CLASS_UNKNOWN_DXR       = 0, //!< selection treats it as RESEARCH_ONLY
    XR_LIFT_LICENCE_CLASS_PERMISSIVE_DXR    = 1, //!< open licence, commercial use permitted
    XR_LIFT_LICENCE_CLASS_COMMERCIAL_DXR    = 2, //!< proprietary, licensed for commercial use
    XR_LIFT_LICENCE_CLASS_RESEARCH_ONLY_DXR = 3, //!< research / evaluation only; present only when the USER opted in
    XR_LIFT_LICENCE_CLASS_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftLicenceClassDXR;

/*! Generic quality / latency preference: the ONLY lever an app has on which variant runs. */
typedef enum XrLiftQualityHintDXR {
    XR_LIFT_QUALITY_HINT_DEFAULT_DXR  = 0,
    XR_LIFT_QUALITY_HINT_LATENCY_DXR  = 1,
    XR_LIFT_QUALITY_HINT_BALANCED_DXR = 2,
    XR_LIFT_QUALITY_HINT_QUALITY_DXR  = 3,
    XR_LIFT_QUALITY_HINT_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftQualityHintDXR;

typedef XrFlags64 XrLiftLaneFlagsDXR;
static const XrLiftLaneFlagsDXR XR_LIFT_LANE_STREAM_BIT_DXR = 0x00000001; //!< latest-wins streams
static const XrLiftLaneFlagsDXR XR_LIFT_LANE_JOB_BIT_DXR    = 0x00000002; //!< one-shot jobs (spec 5)

typedef XrFlags64 XrLiftOutputFlagsDXR;
static const XrLiftOutputFlagsDXR XR_LIFT_OUTPUT_AUX_DEPTH_BIT_DXR           = 0x00000001; //!< views + same-inference depth
static const XrLiftOutputFlagsDXR XR_LIFT_OUTPUT_METRIC_BIT_DXR              = 0x00000002; //!< depth in metres (or metric splats)
static const XrLiftOutputFlagsDXR XR_LIFT_OUTPUT_INTRINSICS_BIT_DXR          = 0x00000004; //!< reports estimated intrinsics
static const XrLiftOutputFlagsDXR XR_LIFT_OUTPUT_TEMPORAL_BIT_DXR            = 0x00000008; //!< temporally consistent output
static const XrLiftOutputFlagsDXR XR_LIFT_OUTPUT_EXPLICIT_VIEWPOINTS_BIT_DXR = 0x00000010; //!< honours explicit viewpoints exactly
static const XrLiftOutputFlagsDXR XR_LIFT_OUTPUT_CONVERGENCE_HINT_BIT_DXR    = 0x00000020; //!< emits a suggested convergence (runtime decides)
static const XrLiftOutputFlagsDXR XR_LIFT_OUTPUT_CONFIDENCE_BIT_DXR          = 0x00000040; //!< RESERVED (confidence plane)
static const XrLiftOutputFlagsDXR XR_LIFT_OUTPUT_COMPOSED_BIT_DXR            = 0x00000080; //!< served by a runtime-composed pipeline (a depth task + the runtime's reference view synthesis)
```

### 2.3 Tasks

Provider enumeration (`xrEnumerateLiftProvidersDXR`, `XrLiftProviderPropertiesDXR`) is **cut** from the app API: it was diagnostics only, the CLI and the status snapshot already carry provider identity, and it was a fingerprinting surface. An app sees tasks, never providers.

```c
/*!
 * The EFFECTIVE capability of one task on this session: the provider the
 * runtime selected for it (frame D2), its readiness, its output contract.
 * xrEnumerateLiftTasksDXR returns one element per task that is not ABSENT, in
 * ascending task order. Array element: never grows — extend via @c next.
 *
 * @c imageKinds / @c exportKinds are what THIS session can hand in / take out
 * (they depend on its graphics API and platform; 0 = this task is not
 * reachable from this session yet).
 *
 * Consumer contract (spec §2.8): anything but READY means WAIT (poll <= 2 Hz,
 * or take the event) — never a session-long fallback.
 *
 * Selection and rebinding (ADR-053): selection never reorders on readiness;
 * a waiting state is waited for, a terminal state falls through. The provider
 * is chosen when a stream is CREATED. A live stream never moves to another
 * provider, except on a terminal failure of its provider or a drain of the
 * provider's host binary (update / uninstall); the move is signalled by
 * XrLiftDepthInfoDXR::discontinuity on the next result plus a
 * XrEventDataLiftTaskStateChangedDXR. A preference or install change applies
 * to the NEXT create only. There is no per-stream provider-changed event.
 */
typedef struct XrLiftTaskPropertiesDXR {
    XrStructureType          type;            //!< XR_TYPE_LIFT_TASK_PROPERTIES_DXR
    void* XR_MAY_ALIAS       next;
    XrLiftTaskDXR            task;
    XrLiftTaskStateDXR       state;
    XrLiftFailureReasonDXR   failureReason;   //!< NONE unless FAILED / DISABLED
    float                    progress;        //!< [0,1] while DOWNLOADING / WARMING; < 0 = unknown
    XrLiftLaneFlagsDXR       lanes;
    XrLiftInputKindFlagsDXR  inputKinds;
    XrLiftOutputFlagsDXR     outputs;
    XrFlags64                depthKinds;      //!< bit (1 << XrLiftDepthKindDXR) the task can return
    XrFlags64                blobFormats;     //!< bit (1 << XrLiftBlobFormatDXR) (splat / job outputs)
    XrFlags64                imageKinds;      //!< bit (1 << XrLiftImageKindDXR) accepted as input here
    XrFlags64                exportKinds;     //!< bit (1 << XrLiftImageKindDXR) offered as result export here
    uint32_t                 maxStreams;      //!< for this task's provider, runtime-wide; 0 = no stream lane
    uint32_t                 maxViews;        //!< VIEWS_* tasks (enforced at create); else 0
    XrExtent2Di              maxInputExtent;  //!< larger inputs are downscaled by the runtime; 0 = none
    XrDuration               typicalLatency;  //!< per conversion (stream) or per job, ns; 0 = unknown
    XrDuration               warmEstimate;    //!< INSTALLED/EVICTED -> READY, ns; 0 = unknown
    XrLiftLicenceClassDXR    licenceClass;
} XrLiftTaskPropertiesDXR;

/*!
 * The provider set behind the tasks changed (install / uninstall / user
 * preference). Re-enumerate TASKS (xrEnumerateLiftTasksDXR): their state,
 * outputs and limits may have changed. Live streams do not move (§2.3).
 */
typedef struct XrEventDataLiftProvidersChangedDXR {
    XrStructureType          type;            //!< XR_TYPE_EVENT_DATA_LIFT_PROVIDERS_CHANGED_DXR
    const void* XR_MAY_ALIAS next;
    XrSession                session;
} XrEventDataLiftProvidersChangedDXR;

/*!
 * A task's effective state changed, or its progress moved by >= 5 % (rate-
 * limited to 2 Hz). Delivered only to sessions that made a v4 call (§2.1-4).
 */
typedef struct XrEventDataLiftTaskStateChangedDXR {
    XrStructureType          type;            //!< XR_TYPE_EVENT_DATA_LIFT_TASK_STATE_CHANGED_DXR
    const void* XR_MAY_ALIAS next;
    XrSession                session;
    XrLiftTaskDXR            task;
    XrLiftTaskStateDXR       state;
    XrLiftTaskStateDXR       previousState;
    XrLiftFailureReasonDXR   failureReason;
    float                    progress;        //!< < 0 = unknown
} XrEventDataLiftTaskStateChangedDXR;

typedef XrResult (XRAPI_PTR *PFN_xrEnumerateLiftTasksDXR)(XrSession session,
    uint32_t capacityInput, uint32_t* countOutput, XrLiftTaskPropertiesDXR* tasks);
typedef XrResult (XRAPI_PTR *PFN_xrRequestLiftTaskWarmDXR)(XrSession session, XrLiftTaskDXR task);
```

### 2.4 Task-typed stream creation and neutral images

```c
/*!
 * Chain on XrLiftStreamCreateInfoDXR::next to create a stream for a TASK
 * (spec 4). XrLiftStreamCreateInfoDXR::mode is then either 0 or the legacy
 * mode matching the task (DEPTH for DEPTH_*, SBS / NVIEW for VIEWS_MONO,
 * GAUSSIANS for SPLATS_MONO); anything else is XR_ERROR_VALIDATION_FAILURE.
 * Setting the matching legacy mode makes the create degrade gracefully on a
 * spec-3 runtime, which ignores this struct. contentHint and inputScale keep
 * their v3 meaning.
 *
 * @c viewCount (VIEWS_* tasks): 2..maxViews, or 0 = "match the weave" (on a
 * lifted weave rect, the active mode's view count; on a plain stream, 2).
 * Validated against the task's maxViews (fixes v3's unenforced maxViews).
 *
 * @c exportKind: how results leave the runtime (XrLiftImageKindDXR; 0 = the
 * platform default, D3D11_NT on Windows, i.e. exactly v3). Must be a bit of
 * the task's exportKinds.
 *
 * Creation rules (§2.8), after selection has fallen through every terminal
 * candidate: XR_ERROR_FEATURE_UNSUPPORTED only when the task is ABSENT,
 * DISABLED or FAILED (any reason; INCOMPATIBLE / UNLICENSED likewise), or when
 * a @c requiredOutputs bit is missing from a provider that is READY. Before
 * READY, @c requiredOutputs is checked against the provider's DECLARED
 * outputs, which may be provisional (a module that reports its aux outputs
 * only once loaded is not refused while it warms). In every waiting state
 * (INSTALLED, DOWNLOADING, WARMING, EVICTED, UNKNOWN) the stream is created
 * and its frames wait, latest-wins, until READY.
 */
typedef struct XrLiftTaskStreamCreateInfoDXR {
    XrStructureType          type;            //!< XR_TYPE_LIFT_TASK_STREAM_CREATE_INFO_DXR
    const void* XR_MAY_ALIAS next;
    XrLiftTaskDXR            task;
    XrLiftInputKindDXR       inputKind;       //!< must be a bit of the task's inputKinds
    XrLiftQualityHintDXR     qualityHint;
    uint32_t                 viewCount;       //!< VIEWS_*: 0 = match the weave / 2
    XrLiftOutputFlagsDXR     requiredOutputs; //!< e.g. METRIC | AUX_DEPTH: refuse rather than degrade
    XrLiftImageKindDXR       exportKind;      //!< 0 = platform default
} XrLiftTaskStreamCreateInfoDXR;

/*!
 * Image handle kinds. Values 1..6 equal XrWeaveHandleKindDXR on purpose
 * (same meaning, no dependency on XR_DXR_weave).
 */
typedef enum XrLiftImageKindDXR {
    XR_LIFT_IMAGE_KIND_PLATFORM_DEFAULT_DXR = 0,
    XR_LIFT_IMAGE_KIND_D3D11_NT_DXR         = 1, //!< NT handle, keyed mutex key 0 (= v3 inputTexture)
    XR_LIFT_IMAGE_KIND_D3D11_DXGI_DXR       = 2, //!< legacy global DXGI handle (= v3 inputIsDxgi)
    XR_LIFT_IMAGE_KIND_IOSURFACE_DXR        = 3,
    XR_LIFT_IMAGE_KIND_AHARDWAREBUFFER_DXR  = 4,
    XR_LIFT_IMAGE_KIND_DMABUF_DXR           = 5, //!< + XrLiftImageDmabufDescDXR on the image's next
    XR_LIFT_IMAGE_KIND_OPAQUE_FD_DXR        = 6, //!< Vulkan OPAQUE_FD, same driver + device only
    XR_LIFT_IMAGE_KIND_D3D12_NT_DXR         = 7, //!< ID3D12Device::CreateSharedHandle, no keyed mutex
    XR_LIFT_IMAGE_KIND_VK_OPAQUE_WIN32_DXR  = 8, //!< Vulkan OPAQUE_WIN32 NT handle
    XR_LIFT_IMAGE_KIND_CPU_DXR              = 16,//!< bytes in caller memory; copied before return
    XR_LIFT_IMAGE_KIND_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftImageKindDXR;

typedef enum XrLiftSyncKindDXR {
    XR_LIFT_SYNC_KIND_NONE_DXR         = 0, //!< the kind's default (KEYED_MUTEX for D3D11_*, CPU_COMPLETE otherwise)
    XR_LIFT_SYNC_KIND_KEYED_MUTEX_DXR  = 1, //!< key 0 = caller done writing (v3)
    XR_LIFT_SYNC_KIND_D3D_FENCE_DXR    = 2, //!< shared ID3D11Fence / ID3D12Fence NT handle + value (closes ADR-049's submit-fence follow-up)
    XR_LIFT_SYNC_KIND_SYNC_FILE_DXR    = 3, //!< sync_file fd (Linux / Android)
    XR_LIFT_SYNC_KIND_VK_TIMELINE_DXR  = 4, //!< exported timeline semaphore (fd or NT handle) + value
    XR_LIFT_SYNC_KIND_SHARED_EVENT_DXR = 5, //!< macOS shared event + value
    XR_LIFT_SYNC_KIND_CPU_COMPLETE_DXR = 6, //!< writes complete before the call (the v3 D3D12 rule)
    XR_LIFT_SYNC_KIND_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftSyncKindDXR;

/*! Values equal the plug-in ABI's (XRT_CONV_FMT_*); asserted in oxr_plugin_stub.c. */
typedef enum XrLiftPixelFormatDXR {
    XR_LIFT_PIXEL_FORMAT_UNKNOWN_DXR      = 0,
    XR_LIFT_PIXEL_FORMAT_RGBA8_UNORM_DXR  = 1,
    XR_LIFT_PIXEL_FORMAT_RGBA8_SRGB_DXR   = 2,
    XR_LIFT_PIXEL_FORMAT_BGRA8_UNORM_DXR  = 3,
    XR_LIFT_PIXEL_FORMAT_BGRA8_SRGB_DXR   = 4,
    XR_LIFT_PIXEL_FORMAT_NV12_DXR         = 5,
    XR_LIFT_PIXEL_FORMAT_R8_UNORM_DXR     = 6,
    XR_LIFT_PIXEL_FORMAT_R16_FLOAT_DXR    = 7,
    XR_LIFT_PIXEL_FORMAT_R32_FLOAT_DXR    = 8,
    XR_LIFT_PIXEL_FORMAT_RGBA16_FLOAT_DXR = 9,
    XR_LIFT_PIXEL_FORMAT_R16_UNORM_DXR    = 10,
    XR_LIFT_PIXEL_FORMAT_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftPixelFormatDXR;

/*! Values equal the plug-in ABI's (XRT_CONV_CS_* 0..4); asserted in oxr_plugin_stub.c. */
typedef enum XrLiftColorSpaceDXR {
    XR_LIFT_COLOR_SPACE_UNKNOWN_DXR       = 0, //!< treated as sRGB-encoded
    XR_LIFT_COLOR_SPACE_SRGB_DXR          = 1,
    XR_LIFT_COLOR_SPACE_LINEAR_SRGB_DXR   = 2,
    XR_LIFT_COLOR_SPACE_BT709_LIMITED_DXR = 3, //!< video-range YUV (the NV12 default)
    XR_LIFT_COLOR_SPACE_BT709_FULL_DXR    = 4,
    XR_LIFT_COLOR_SPACE_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftColorSpaceDXR;

typedef enum XrLiftImageRoleDXR {
    XR_LIFT_IMAGE_ROLE_COLOR_DXR       = 1, //!< mono input, or the first/left eye, or a whole SBS pair
    XR_LIFT_IMAGE_ROLE_COLOR_RIGHT_DXR = 2, //!< second eye when the pair is two images (spec 5)
    XR_LIFT_IMAGE_ROLE_DEPTH_DXR       = 3, //!< depth input (VIEWS_FROM_DEPTH; + XrLiftDepthInfoDXR on next), spec 5
    XR_LIFT_IMAGE_ROLE_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftImageRoleDXR;

/*!
 * One input image, any platform. Handles are BORROWED: the runtime duplicates
 * whatever it keeps. (This differs from XR_DXR_weave's runtime-owned dma-buf
 * fd, because a lift input is often re-submitted.) CPU images are copied
 * before the call returns. As in v3, a GPU image is SNAPSHOTTED before return
 * (one copy on the runtime's GPU), honouring @c syncKind, so the caller may
 * rewrite it afterwards. Exception: CPU_COMPLETE on a kind without a keyed
 * mutex, where the v3 D3D12 rule holds (do not rewrite until a later frame;
 * double-buffer).
 */
typedef struct XrLiftImageDXR {
    XrStructureType          type;            //!< XR_TYPE_LIFT_IMAGE_DXR
    const void* XR_MAY_ALIAS next;            //!< XrLiftImageDmabufDescDXR / XrLiftDepthInfoDXR (DEPTH role)
    XrLiftImageRoleDXR       role;
    XrLiftImageKindDXR       kind;
    void*                    handle;          //!< HANDLE / IOSurfaceRef / AHardwareBuffer* / (void*)(intptr_t)fd; NULL for CPU and DMABUF
    const void*              cpuBytes;        //!< CPU only
    uint32_t                 cpuRowPitch;     //!< CPU only, bytes
    XrLiftPixelFormatDXR     format;
    XrLiftColorSpaceDXR      colorSpace;
    XrRect2Di                region;          //!< sub-rect to convert (v3 = {0,0, extent})
    XrLiftSyncKindDXR        syncKind;
    void*                    syncHandle;      //!< per syncKind; NULL for NONE / KEYED_MUTEX / CPU_COMPLETE
    uint64_t                 syncValue;       //!< fence / timeline value to wait on
} XrLiftImageDXR;

/*! Field-for-field XrWeaveDmabufDescDXR (separate type: no XR_DXR_weave dependency). fd BORROWED. */
typedef struct XrLiftImageDmabufDescDXR {
    XrStructureType          type;            //!< XR_TYPE_LIFT_IMAGE_DMABUF_DESC_DXR
    const void* XR_MAY_ALIAS next;
    int32_t                  fd;
    uint32_t                 width;
    uint32_t                 height;
    uint32_t                 drmFourcc;
    uint64_t                 drmModifier;
    uint32_t                 planeCount;
    uint32_t                 offsets[4];
    uint32_t                 strides[4];
    uint64_t                 bufferId;        //!< stable caller identity for the import cache; 0 = none
} XrLiftImageDmabufDescDXR;

/*!
 * Chain on XrLiftFrameSubmitInfoDXR::next (spec 4). When present it REPLACES
 * the base struct's inputTexture / inputIsDxgi / extent. inputTexture may then
 * be NULL: a spec-4 runtime relaxes v3's non-NULL check in this case only.
 * 1..3 images, at most one per role.
 */
typedef struct XrLiftFrameImagesDXR {
    XrStructureType          type;            //!< XR_TYPE_LIFT_FRAME_IMAGES_DXR
    const void* XR_MAY_ALIAS next;
    uint32_t                 imageCount;
    const XrLiftImageDXR*    images;
} XrLiftFrameImagesDXR;

typedef XrFlags64 XrLiftFrameControlFlagsDXR;
//! The source jumped (seek, cut, source switch): drop temporal state before this frame.
static const XrLiftFrameControlFlagsDXR XR_LIFT_FRAME_CONTROL_DISCONTINUITY_BIT_DXR = 0x00000001;

/*! Chain on XrLiftFrameSubmitInfoDXR::next or XrWeaveRectLiftDXR::next (spec 4). Per frame, not sticky. */
typedef struct XrLiftFrameControlDXR {
    XrStructureType            type;          //!< XR_TYPE_LIFT_FRAME_CONTROL_DXR
    const void* XR_MAY_ALIAS   next;
    XrLiftFrameControlFlagsDXR flags;
} XrLiftFrameControlDXR;

/*!
 * Chain on XrLiftResultDXR::next (spec 4). Says how outputTexture / fence are
 * to be read when the stream's exportKind is not D3D11_NT; XrLiftResultDXR::
 * format is then 0 and these fields are authoritative. Handles arrive under
 * the v3 rule: only on the first acquire and on reallocation.
 */
typedef struct XrLiftResultExportDXR {
    XrStructureType          type;            //!< XR_TYPE_LIFT_RESULT_EXPORT_DXR
    void* XR_MAY_ALIAS       next;
    XrLiftImageKindDXR       kind;            //!< of XrLiftResultDXR::outputTexture (and of the aux depth texture)
    XrLiftPixelFormatDXR     format;
    XrLiftColorSpaceDXR      colorSpace;
    XrLiftSyncKindDXR        syncKind;        //!< of XrLiftResultDXR::fence; value = fenceValue
} XrLiftResultExportDXR;
```

**Where things chain (spec 4):**

| Struct | Chained on | Notes |
|---|---|---|
| `XrLiftTaskStreamCreateInfoDXR` | `XrLiftStreamCreateInfoDXR` | `XrLiftDepthRequestDXR` may still sit beside it (VIEWS_MONO only); equivalent to `requiredOutputs \|= AUX_DEPTH`, but advisory |
| `XrLiftFrameImagesDXR` → `XrLiftImageDXR[]` | `XrLiftFrameSubmitInfoDXR` | `XrLiftOptionsDXR` stays on the same chain |
| `XrLiftFrameControlDXR` | `XrLiftFrameSubmitInfoDXR`, `XrWeaveRectLiftDXR` | per frame, not sticky |
| `XrLiftResultExportDXR` | `XrLiftResultDXR` | untouched on a D3D11_NT stream |
| `XrLiftDepthInfoDXR` | `XrLiftResultDXR` (DEPTH_* stream), `XrLiftDepthResultDXR` (aux depth), `XrLiftImageDXR` (DEPTH-role input), `XrLiftJobResultDXR` (spec 5) | one struct describes depth wherever depth appears |
| `XrLiftViewsInfoDXR` | `XrLiftResultDXR` (VIEWS_* stream) | |

### 2.5 Result contracts: depth and views

```c
/*!
 * What a decoded depth sample means (frame D9; prior-art §3). Values equal the
 * plug-in ABI's (XRT_CONV_DEPTH_*); asserted in oxr_plugin_stub.c.
 */
typedef enum XrLiftDepthKindDXR {
    XR_LIFT_DEPTH_KIND_METRIC_DEPTH_DXR         = 1, //!< metres (v3: METRIC + LINEAR)
    XR_LIFT_DEPTH_KIND_METRIC_INVERSE_DXR       = 2, //!< 1/metres (v3: METRIC + INVERSE)
    XR_LIFT_DEPTH_KIND_AFFINE_INVERSE_DEPTH_DXR = 3, //!< unknown scale+shift of 1/depth, larger = nearer (v3: RELATIVE + INVERSE)
    XR_LIFT_DEPTH_KIND_AFFINE_DISPARITY_DXR     = 4, //!< disparity up to scale+shift (monocular "disparity" heads; v3: RELATIVE + INVERSE)
    XR_LIFT_DEPTH_KIND_AFFINE_DEPTH_DXR         = 5, //!< unknown scale+shift, larger = farther (v3: RELATIVE + LINEAR)
    XR_LIFT_DEPTH_KIND_PIXEL_DISPARITY_DXR      = 6, //!< exact disparity in pixels of the rectified pair (DISPARITY_STEREO)
    XR_LIFT_DEPTH_KIND_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftDepthKindDXR;

/*! Values equal the plug-in ABI's (XRT_CONV_MEASURE_*); asserted in oxr_plugin_stub.c. */
typedef enum XrLiftDepthMeasureDXR {
    XR_LIFT_DEPTH_MEASURE_PLANAR_DXR = 0,  //!< distance to the camera plane (optical-axis z)
    XR_LIFT_DEPTH_MEASURE_RAY_DXR    = 1,  //!< distance along the pixel ray
    XR_LIFT_DEPTH_MEASURE_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftDepthMeasureDXR;

/*! Values equal the plug-in ABI's (XRT_CONV_INTRINSICS_*). */
typedef enum XrLiftIntrinsicsSourceDXR {
    XR_LIFT_INTRINSICS_SOURCE_UNKNOWN_DXR   = 0, //!< no intrinsics; fields are 0
    XR_LIFT_INTRINSICS_SOURCE_GIVEN_DXR     = 1, //!< from the caller (focalPx, calibration)
    XR_LIFT_INTRINSICS_SOURCE_ESTIMATED_DXR = 2, //!< estimated by the model from the pixels
    XR_LIFT_INTRINSICS_SOURCE_ASSUMED_DXR   = 3, //!< a provider default (e.g. a fixed FOV); not evidence
    XR_LIFT_INTRINSICS_SOURCE_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftIntrinsicsSourceDXR;

/*! Values equal the plug-in ABI's (XRT_CONV_COORDS_*). */
typedef enum XrLiftCoordinateSystemDXR {
    XR_LIFT_COORDINATE_SYSTEM_UNKNOWN_DXR    = 0,
    XR_LIFT_COORDINATE_SYSTEM_OPENCV_RDF_DXR = 1, //!< +x right, +y down, +z forward, metres
    XR_LIFT_COORDINATE_SYSTEM_RUB_DXR        = 2, //!< +x right, +y up, +z back (OpenXR / glTF)
    XR_LIFT_COORDINATE_SYSTEM_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftCoordinateSystemDXR;

/*!
 * Everything needed to interpret a depth image (frame D9). Decode a sample s
 * as v = rawScale * s + rawShift, then interpret v by @c kind.
 *
 * Units are never invented (frame D10): with an AFFINE_* kind, near/far and
 * the intrinsics are as reported, or zero. A provider that does not know a
 * field leaves it zero / invalid. farZ < nearZ means an infinite far plane
 * (the OpenXR convention); both 0 = unknown.
 *
 * The range fields are THIS frame's statistics, in decoded units. They survive
 * per-frame relative normalisation, which is what lets the runtime's
 * depth-budget and convergence policy work on affine depth.
 */
typedef struct XrLiftDepthInfoDXR {
    XrStructureType           type;               //!< XR_TYPE_LIFT_DEPTH_INFO_DXR
    void* XR_MAY_ALIAS        next;
    XrLiftDepthKindDXR        kind;
    XrLiftDepthMeasureDXR     measure;
    float                     rawScale;
    float                     rawShift;
    XrBool32                  invalidValueValid;
    float                     invalidValue;       //!< raw sample meaning "no depth"
    float                     nearZ;              //!< decoded units; 0 = unknown
    float                     farZ;               //!< decoded units; < nearZ = infinite; 0 = unknown
    XrBool32                  rangeValid;
    float                     rangeMin;           //!< decoded, this frame
    float                     rangeMax;
    float                     rangeP05;           //!< decoded, this frame (robust)
    float                     rangeP95;
    XrLiftIntrinsicsSourceDXR intrinsicsSource;
    XrVector2f                focalLengthPx;      //!< fx, fy in pixels of THIS image
    XrVector2f                principalPointPx;   //!< cx, cy
    XrLiftCoordinateSystemDXR convention;         //!< of back-projection with these intrinsics
    XrBool32                  temporallyConsistent; //!< produced by a temporal (video) model
    XrBool32                  discontinuity;      //!< temporal state was reset before this result
    float                     baselineMeters;     //!< PIXEL_DISPARITY: the pair's baseline (given, never defaulted); else 0
} XrLiftDepthInfoDXR;

/*!
 * Values equal the plug-in ABI's (XRT_CONV_VIEWS_*); asserted in
 * oxr_plugin_stub.c. An OpenXR enum may start at 0. The view ceiling is 8 at
 * both levels (XR_LIFT_MAX_VIEWS_DXR == XRT_CONV_MAX_VIEWS); at 8 views ROW
 * always fits the texture limit, so ATLAS stays reserved until a later spec
 * raises the ceiling (the runtime then picks ATLAS or ARRAY_LAYERS when
 * N x viewWidth exceeds the limit).
 */
typedef enum XrLiftViewsLayoutDXR {
    XR_LIFT_VIEWS_LAYOUT_ROW_DXR          = 0, //!< v3: side by side, view 0 leftmost
    XR_LIFT_VIEWS_LAYOUT_ARRAY_LAYERS_DXR = 1, //!< one array layer per view (ADR-032)
    XR_LIFT_VIEWS_LAYOUT_ATLAS_DXR        = 2, //!< RESERVED: columns x rows, row-major
    XR_LIFT_VIEWS_LAYOUT_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftViewsLayoutDXR;

typedef enum XrLiftCameraModelDXR {
    XR_LIFT_CAMERA_MODEL_UNKNOWN_DXR        = 0,
    XR_LIFT_CAMERA_MODEL_OFF_AXIS_DXR       = 1, //!< asymmetric frusta onto the lifted rect (Kooima)
    XR_LIFT_CAMERA_MODEL_PARALLEL_SHIFT_DXR = 2, //!< parallel cameras + horizontal image shift
    XR_LIFT_CAMERA_MODEL_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftCameraModelDXR;

typedef enum XrLiftConvergenceSourceDXR {
    XR_LIFT_CONVERGENCE_SOURCE_UNKNOWN_DXR      = 0,
    XR_LIFT_CONVERGENCE_SOURCE_APP_DXR          = 1, //!< XrLiftOptionsDXR::convergence
    XR_LIFT_CONVERGENCE_SOURCE_RUNTIME_AUTO_DXR = 2, //!< runtime policy (AUTO)
    XR_LIFT_CONVERGENCE_SOURCE_PROVIDER_DXR     = 3, //!< the provider's own (legacy adapter only)
    XR_LIFT_CONVERGENCE_SOURCE_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftConvergenceSourceDXR;

/*!
 * How a views result was made (frame D9). It round-trips to stereo-video
 * metadata: baseline, hFOV, horizontal disparity adjustment. Chain on
 * XrLiftResultDXR::next of a VIEWS_* (SBS / NVIEW) stream. Distances are in
 * metres in the SOURCE camera's frame. Disparities are fractions of one
 * view's width (positive = uncrossed, i.e. behind the screen).
 */
typedef struct XrLiftViewsInfoDXR {
    XrStructureType            type;                //!< XR_TYPE_LIFT_VIEWS_INFO_DXR
    void* XR_MAY_ALIAS         next;
    XrLiftViewsLayoutDXR       layout;
    uint32_t                   viewCount;
    uint32_t                   columns;             //!< ROW: viewCount
    uint32_t                   rows;                //!< ROW: 1
    XrExtent2Di                viewExtent;          //!< one tile
    XrBool32                   eyeOffsetsValid;
    float                      eyeOffsetMeters[XR_LIFT_MAX_VIEWS_DXR]; //!< x of each view from the source camera (8 == XRT_CONV_MAX_VIEWS)
    float                      baselineMeters;      //!< first-to-last view distance actually rendered; 0 = unknown
    float                      horizontalFovRadians;//!< of each synthesized view; 0 = unknown
    float                      disparityAdjust;     //!< horizontal shift that placed the zero-parallax plane, fraction of width
    XrBool32                   disparityRangeValid;
    float                      disparityMin;        //!< rendered disparity range, fraction of width
    float                      disparityMax;        //!<   (the depth budget actually used)
    XrLiftCameraModelDXR       cameraModel;
    XrLiftConvergenceSourceDXR convergenceSource;
    float                      convergenceApplied;  //!< in v3 convergence units [0,1]; < 0 = unknown
    float                      suggestedConvergence;//!< provider hint, same units; < 0 = none. Diagnostics: policy ignores it unless the provider is corpus-validated
    float                      suggestedStrength;   //!< provider hint for the disparity budget, 1 = its own; < 0 = none (same rule)
    XrBool32                   reliefValid;         //!< display z of a depth d: z = reliefScale / d + reliefOffset
    float                      reliefScale;
    float                      reliefOffset;
} XrLiftViewsInfoDXR;
```

### 2.6 The legacy mapping (a v3 caller served unchanged)

| v3 request | Served by | Notes |
|---|---|---|
| `DEPTH` + VIDEO hint | `DEPTH_VIDEO`, MONO | temporal reset by the runtime's own cut detector when no `XrLiftFrameControlDXR` is sent |
| `DEPTH` + PHOTO | `DEPTH_MONO`, MONO | |
| `SBS` | `VIEWS_MONO`, viewCount 2 | VIDEO keeps temporal state; PHOTO drops it |
| `NVIEW` + `XrLiftOptionsDXR::viewCount` | `VIEWS_MONO`, viewCount N | N above the task's maxViews is now `XR_ERROR_VALIDATION_FAILURE` (a fix) |
| `GAUSSIANS` (PHOTO) | `SPLATS_MONO`, on the **job worker** behind the stream facade | a latest-wins photo mailbox keeps v3 semantics; the blob is SOG (camera block embedded) when the transcoder is present, else PLY — v3 formats only |
| `XrLiftDepthRequestDXR` on SBS / NVIEW | `VIEWS_MONO` + AUX_DEPTH, advisory | as v3 |
| `focalPx > 0` | intrinsics source GIVEN | |
| `XrLiftDepthResultDXR` (v3 fields) | filled from the same `XrLiftDepthInfoDXR` | `AFFINE_DISPARITY` is reported to v3 as RELATIVE + INVERSE, so `auxDepthSupported` stays true; `PIXEL_DISPARITY` never reaches a v3 aux result |

**`XrLiftPropertiesDXR` for a v3 caller (ADR-053).** This table is the only spec-3 mapping (the plug-in ABI carries no coarse-state helper). Backing tasks: DEPTH → DEPTH_VIDEO (else DEPTH_MONO); SBS and NVIEW → VIEWS_MONO; GAUSSIANS → SPLATS_MONO.
- `supportedModes` = every mode whose backing task is not terminal (ABSENT, INCOMPATIBLE, UNLICENSED, FAILED, DISABLED), waiting ones included. NVIEW only when VIEWS_MONO's `maxViews > 2`.
- `state` = READY when **any** advertised mode's backing task is READY; ACTIVATING when none is READY but some mode is advertised, **and also while discovery has not settled** (some registered provider is neither described nor terminal) or any backing task is non-terminal, even before any mode bit is set; UNAVAILABLE otherwise. "Any", not "all": a splat model downloading must not hold back live video lift. The service persists the last describe result per provider binary (path, size, mtime) and reloads it at start, so the table is never empty after the first run; "properties before the first describe ⇒ ACTIVATING" is a golden test (shipped browsers never re-poll UNAVAILABLE).
- `maxStreams`, `maxViews`, `backend` and `typicalLatency` come from VIEWS_MONO (else DEPTH); `depthSemantics` is METRIC iff the depth task's preferred kind is metric; `XrLiftDepthPropertiesDXR` comes from VIEWS_MONO's AUX_DEPTH — now correct over IPC.
- The mode is checked at create against "advertised". A stream created while its task was warming stays valid if the task later fails (frames count in `framesFailed`; the next properties query drops the bit) and resumes if it recovers.

---

## 3. `XR_DXR_lift.h` spec 5 additions (P6)

Spec 5 adds jobs (create / poll / cancel / chunked read / destroy / events), `XrLiftSplatsInfoDXR`, stereo input, image + depth input (`VIEWS_FROM_DEPTH`), and the SPZ blob format. Stereo-camera source binding (`XrLiftStreamStereoSourceDXR`) keeps its type value but is RESERVED, with no code in v1. Type values: §2.1. Jobs take decoded pixels (a CPU or GPU image), never a path; results come back as chunked blobs or into a caller-opened file handle (ADR-053).

### 3.1 Blob formats and the splats camera block

```c
// Appended to XrLiftBlobFormatDXR (v3 values 1 = PLY_3DGS and 2 = SOG unchanged).
// Values equal the plug-in ABI's (XRT_CONV_BLOB_*); asserted in oxr_plugin_stub.c.
//   XR_LIFT_BLOB_FORMAT_SPZ_DXR            = 3, //!< SPZ v4: the canonical splat output
//   XR_LIFT_BLOB_FORMAT_RAW_IMAGE_DXR      = 4, //!< tightly packed pixels; see XrLiftJobResultDXR::extent / pixelFormat
//   XR_LIFT_BLOB_FORMAT_SPLAT_SEQUENCE_DXR = 5, //!< RESERVED — no code in v1 (manifest of per-frame SPZ + timestamps + one camera block)

typedef enum XrLiftMetricSourceDXR {
    XR_LIFT_METRIC_SOURCE_NONE_DXR   = 0, //!< not metric (in SOG terms: no "metric" block)
    XR_LIFT_METRIC_SOURCE_MODEL_DXR  = 1, //!< a metric monocular model
    XR_LIFT_METRIC_SOURCE_STEREO_DXR = 2, //!< fitted from a calibrated stereo pair (baseline given)
    XR_LIFT_METRIC_SOURCE_GIVEN_DXR  = 3, //!< caller-supplied scale
    XR_LIFT_METRIC_SOURCE_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftMetricSourceDXR;

/*!
 * The camera block of a splat result (spec 5). It mirrors the SOG meta.json
 * `camera` block so the two round-trip, and it is authoritative. When the blob
 * format has a place for it (SOG meta.json `camera`, an SPZ extension record,
 * a PLY comment line), the runtime ALSO embeds the same data there
 * (@c cameraBlockEmbedded).
 *
 * Two rules: a baseline is NEVER defaulted (@c baselineValid only when given,
 * or derived from a calibrated pair), and metric scale is never invented
 * (@c metricSource NONE = a relative scene).
 */
typedef struct XrLiftSplatsInfoDXR {
    XrStructureType           type;               //!< XR_TYPE_LIFT_SPLATS_INFO_DXR
    void* XR_MAY_ALIAS        next;
    XrLiftBlobFormatDXR       format;
    uint64_t                  splatCount;
    uint32_t                  shDegree;           //!< 0..4
    XrLiftCoordinateSystemDXR coordinateSystem;   //!< of the splat positions in the blob
    XrLiftColorSpaceDXR       colorSpace;         //!< of SH0 / base colour (tagged, measured)
    XrLiftMetricSourceDXR     metricSource;
    float                     metricScale;        //!< applied scene scale; 1 when not metric
    XrLiftIntrinsicsSourceDXR intrinsicsSource;
    XrVector2f                focalLengthPx;      //!< of the grid actually unprojected through
    XrVector2f                principalPointPx;
    XrExtent2Di               intrinsicsExtent;   //!< that grid (one eye for stereo)
    XrBool32                  baselineValid;
    float                     baselineMeters;
    XrPosef                   restPose;           //!< camera -> world; origin = the source (left) camera
    XrBool32                  focusValid;
    XrVector3f                focusPoint;         //!< in @c coordinateSystem
    float                     subjectDistanceMeters;
    float                     nearMeters;         //!< 0 = unknown
    float                     farMeters;          //!< 0 = unknown
    XrBool32                  cameraBlockEmbedded;
} XrLiftSplatsInfoDXR;
```

### 3.2 Stereo input and jobs

```c
typedef enum XrLiftStereoLayoutDXR {
    XR_LIFT_STEREO_LAYOUT_SBS_DXR        = 1, //!< one COLOR image, eyes side by side (full or half width)
    XR_LIFT_STEREO_LAYOUT_TOP_BOTTOM_DXR = 2,
    XR_LIFT_STEREO_LAYOUT_TWO_IMAGES_DXR = 3, //!< COLOR + COLOR_RIGHT
    XR_LIFT_STEREO_LAYOUT_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftStereoLayoutDXR;

/*!
 * A RECTIFIED stereo input, in the XR_DXR_stereo_camera shape (frame D9).
 * Chain on XrLiftFrameSubmitInfoDXR::next or XrLiftJobCreateInfoDXR::next.
 *
 * - Per-eye intrinsics are those of the rectified image (equal fy, cy; no
 *   distortion).
 * - @c baselineMeters 0 = UNKNOWN: the result is then never metric. There is
 *   no default.
 * - @c infinityShiftPx: the disparity of points at infinity (0 for a parallel
 *   rig; non-zero for a converged or shifted capture).
 * - @c eyesSwapped: the first image (or the left half) is the RIGHT eye.
 *
 * Interop: when XR_DXR_stereo_camera is ENABLED, an
 * XrStereoCameraCalibrationDXR (output RECTIFIED) may be chained instead; the
 * runtime reads it identically.
 */
typedef struct XrLiftStereoInputDXR {
    XrStructureType          type;            //!< XR_TYPE_LIFT_STEREO_INPUT_DXR
    const void* XR_MAY_ALIAS next;
    XrLiftStereoLayoutDXR    layout;
    XrBool32                 eyesSwapped;
    XrVector2f               focalLengthPx[2];
    XrVector2f               principalPointPx[2];
    XrExtent2Di              eyeExtent;
    float                    baselineMeters;
    float                    infinityShiftPx;
} XrLiftStereoInputDXR;

/*!
 * RESERVED — no code in v1. The type value is kept; a v1 runtime rejects
 * this struct on the chain (XR_ERROR_FEATURE_UNSUPPORTED).
 *
 * Feed a stream DIRECTLY from a runtime stereo camera, with no app round trip.
 * Chain on XrLiftStreamCreateInfoDXR::next with a STEREO_PAIR task.
 *
 * - Needs XR_DXR_stereo_camera enabled and the camera's consent for this
 *   client; that extension's errors are returned verbatim.
 * - Submits on such a stream are XR_ERROR_VALIDATION_FAILURE.
 * - Frames arrive at the camera's rate, latest-wins.
 */
typedef struct XrLiftStreamStereoSourceDXR {
    XrStructureType          type;            //!< XR_TYPE_LIFT_STREAM_STEREO_SOURCE_DXR
    const void* XR_MAY_ALIAS next;
    uint64_t                 cameraId;        //!< from the stereo-camera enumeration
    float                    maxFrameRate;    //!< <= 0 = the camera's rate
} XrLiftStreamStereoSourceDXR;

XR_DEFINE_HANDLE(XrLiftJobDXR)

typedef enum XrLiftJobStatusDXR {
    XR_LIFT_JOB_STATUS_QUEUED_DXR               = 1,
    XR_LIFT_JOB_STATUS_WAITING_FOR_PROVIDER_DXR = 2, //!< task not READY yet; the job waits (it never fails for that alone)
    XR_LIFT_JOB_STATUS_RUNNING_DXR              = 3,
    XR_LIFT_JOB_STATUS_SUCCEEDED_DXR            = 4,
    XR_LIFT_JOB_STATUS_FAILED_DXR               = 5,
    XR_LIFT_JOB_STATUS_CANCELLED_DXR            = 6,
    XR_LIFT_JOB_STATUS_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftJobStatusDXR;

typedef enum XrLiftJobPriorityDXR {
    XR_LIFT_JOB_PRIORITY_BACKGROUND_DXR  = 1,
    XR_LIFT_JOB_PRIORITY_NORMAL_DXR      = 2,
    XR_LIFT_JOB_PRIORITY_INTERACTIVE_DXR = 3, //!< a user is waiting on it (e.g. "pause -> explore")
    XR_LIFT_JOB_PRIORITY_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftJobPriorityDXR;

/*!
 * Create AND submit a one-shot conversion (spec 5). Inputs are snapshotted
 * (GPU) or copied (CPU) before the call returns. The job then runs on the job
 * worker, never on the stream scheduler (frame D4).
 *
 * Chain XrLiftOptionsDXR (focalPx, viewCount, explicit viewpoints),
 * XrLiftStereoInputDXR and/or XrLiftJobFileOutputDXR.
 *
 * The runtime NEVER parses encoded image files: pass decoded pixels (CPU kind)
 * or a GPU image.
 */
typedef struct XrLiftJobCreateInfoDXR {
    XrStructureType          type;            //!< XR_TYPE_LIFT_JOB_CREATE_INFO_DXR
    const void* XR_MAY_ALIAS next;
    XrLiftTaskDXR            task;            //!< must have XR_LIFT_LANE_JOB_BIT_DXR
    XrLiftInputKindDXR       inputKind;
    XrLiftQualityHintDXR     qualityHint;
    XrLiftJobPriorityDXR     priority;
    XrLiftBlobFormatDXR      preferredFormat; //!< 0 = canonical (SPZ for splats, RAW_IMAGE for depth / views)
    XrLiftOutputFlagsDXR     requiredOutputs; //!< e.g. METRIC; AUX_DEPTH on SPLATS_* also returns the depth used
    uint32_t                 imageCount;
    const XrLiftImageDXR*    images;
    XrTime                   sourceTime;      //!< echoed on every output
} XrLiftJobCreateInfoDXR;

/*!
 * Write the primary output into a FILE the CALLER opened for writing (spec 5):
 * an OS file handle / fd, borrowed and duplicated. Never a path: the service
 * must not open paths on a client's behalf (confused deputy). Refused
 * (XR_ERROR_VALIDATION_FAILURE) from low-integrity / sandboxed clients.
 */
typedef struct XrLiftJobFileOutputDXR {
    XrStructureType          type;            //!< XR_TYPE_LIFT_JOB_FILE_OUTPUT_DXR
    const void* XR_MAY_ALIAS next;
    uint64_t                 fileHandle;      //!< HANDLE (Windows) / fd (POSIX)
} XrLiftJobFileOutputDXR;

typedef struct XrLiftJobStateDXR {
    XrStructureType          type;            //!< XR_TYPE_LIFT_JOB_STATE_DXR
    void* XR_MAY_ALIAS       next;
    XrLiftJobStatusDXR       status;
    XrLiftFailureReasonDXR   failureReason;
    float                    progress;        //!< [0,1]; < 0 = unknown (indeterminate)
    XrDuration               elapsed;
    XrDuration               estimatedRemaining; //!< 0 = unknown
    uint32_t                 outputCount;     //!< valid once SUCCEEDED (excludes the primary when the file sink took it)
} XrLiftJobStateDXR;

/*!
 * Read one output of a SUCCEEDED job, in CHUNKS (spec 5). No transport size
 * cap applies (a 1 M-splat PLY exceeds v3's 256 MiB blob limit).
 *
 * Idiom: call once with byteCapacityInput 0 to get totalByteCount and the
 * metadata; then read [byteOffset, byteOffset + capacity) repeatedly.
 * Chain XrLiftSplatsInfoDXR / XrLiftDepthInfoDXR / XrLiftViewsInfoDXR to
 * receive the output's metadata. Outputs persist until xrDestroyLiftJobDXR.
 */
typedef struct XrLiftJobResultDXR {
    XrStructureType          type;            //!< XR_TYPE_LIFT_JOB_RESULT_DXR
    void* XR_MAY_ALIAS       next;
    uint32_t                 outputIndex;     //!< 0 = primary; 1.. = auxiliary (e.g. the depth used)
    XrLiftTaskDXR            task;            //!< what this output is (e.g. DEPTH_MONO for an aux depth)
    XrLiftBlobFormatDXR      format;
    XrExtent2Di              extent;          //!< RAW_IMAGE only (all views, for a views output)
    XrLiftPixelFormatDXR     pixelFormat;     //!< RAW_IMAGE only
    uint64_t                 totalByteCount;
    uint64_t                 byteOffset;      //!< IN
    uint32_t                 byteCapacityInput;
    uint32_t                 byteCountOutput;
    uint8_t*                 bytes;
    XrTime                   sourceTime;
} XrLiftJobResultDXR;

typedef struct XrEventDataLiftJobStateChangedDXR {
    XrStructureType          type;            //!< XR_TYPE_EVENT_DATA_LIFT_JOB_STATE_CHANGED_DXR
    const void* XR_MAY_ALIAS next;
    XrSession                session;
    XrLiftJobDXR             job;
    XrLiftJobStatusDXR       status;
    float                    progress;        //!< rate-limited to 2 Hz / 5 % steps
} XrEventDataLiftJobStateChangedDXR;

typedef XrResult (XRAPI_PTR *PFN_xrCreateLiftJobDXR)(XrSession session,
    const XrLiftJobCreateInfoDXR* createInfo, XrLiftJobDXR* job);
typedef XrResult (XRAPI_PTR *PFN_xrGetLiftJobStateDXR)(XrLiftJobDXR job, XrLiftJobStateDXR* state);
typedef XrResult (XRAPI_PTR *PFN_xrCancelLiftJobDXR)(XrLiftJobDXR job);
typedef XrResult (XRAPI_PTR *PFN_xrReadLiftJobResultDXR)(XrLiftJobDXR job, XrLiftJobResultDXR* result);
typedef XrResult (XRAPI_PTR *PFN_xrDestroyLiftJobDXR)(XrLiftJobDXR job);
```

---

## 4. The `conv_*` IPC wire family

The v3 `lift_*` messages (`proto.json:838-960`) stay **byte-for-byte**, because the browser vendors the client side. Their handlers become thin adapters onto the conversion core, using the §2.6 `XrLiftPropertiesDXR` mapping (the only spec-3 mapping). The G1 fix ships in passing: `ipc_client_lift_get_properties` (`ipc_client_lift.c:44-70`) copies `aux_outputs` and `aux_depth_semantics`, pinned by a round-trip test.

Spec 4 / 5 ride a new family. Its payloads are defined once, in `src/xrt/include/xrt/xrt_conv_wire.h` (landed in P1a-1, #1998; P1a-8 wires them into `ipc_protocol.h` / `proto.json`). Sizes are exact and asserted (every gap is an explicit `reserved_*` field); `xrt_conv_wire_` is omitted from the struct names below.

| Message | Request | Reply | Handles |
|---|---|---|---|
| `conv_hint_interest` | `hint_interest` (16): `task_mask` u64, bit `1 << XRT_CONV_TASK_*`, 0 = the default set | – | – |
| `conv_get_generation` | – | `generation` (24): `conv_gen`, `status_gen` | – |
| `conv_enum_task` | task index (u32) | `task_desc` (144): the selected provider's capability (or the composed pipeline's) + readiness; no provider identity | – |
| `conv_stream_create` | `stream_desc` (56): task, quality hint, export memory kind + every `xrt_conv_stream_info` field | `stream_created` (40): stream id, task, granted outputs, resolved view count + layout, `desc_flags` | – |
| `conv_stream_destroy` / `conv_stream_reset` (cut / seek) | stream id (u64) | – | – |
| `conv_input_ring_get` | id | ring geometry | out: N image handles + fence. **P5 only**: no struct before a real consumer |
| `conv_submit` | `submit` (560): stream id, ≤ 2 `image` descriptors, `params`, `depth_in_info` twin | frame id (u64) | in: image handle (+ sync handle when its kind has one) per descriptor, in descriptor order |
| `conv_acquire_result` | stream id (u64) | `result` (352): frame id, source time, latency, `written`, `output_realloc`, width / height / format of the views / depth / confidence / disparity targets, `views_info` + `depth_info` twins | none here; export handles come from generalised `lift_get_output`-style calls, only when `output_realloc` says so |
| `conv_job_submit` | `job_desc` (632): task, quality hint, input kind, ≤ 4 `image` descriptors, requested blob format, `params` | job id (u64) | in: image handles; CPU pixels in the scratch shm |
| `conv_job_status` | job id (u64) | `job_status` (64): state (`XrLiftJobStateDXR`, spec 5), failure reason, progress, 32-byte stage label | – |
| `conv_job_fetch` | job id (u64) | `job_result` (368): `written`, blob format, delivery (`NONE` / `INLINE` ≤ 1 MiB in the scratch shm / `SHM` / `FILE`), byte size, `splat_info` + `depth_info` + `views_info` twins | out: shm handle (`SHM`) |
| `conv_job_cancel` | job id (u64) | – | – |
| `conv_warm` / `conv_evict` | `task_request` (16) | – | – (`conv_warm` also open to DIAG; `conv_evict` only on the CLI's own DIAG connection) |
| events | the per-session event queue, plus a sessionless per-connection queue drained by `conv_poll_event` | `event` (40): kind (`PROVIDERS_CHANGED` / `TASK_STATE_CHANGED` / `JOB_STATE_CHANGED`), task, state, previous state, failure reason, progress, job id | – |

There is **no `conv_enum_provider`** on the wire: an app-facing connection sees tasks only, through `conv_enum_task`. Provider identity reaches the CLI and the status snapshot over the existing DIAG / snapshot path, not through this family.

**Frozen sub-blocks** (embedded by value; never grow — a field appended to the ABI struct goes at the tail of the message instead):
- Wire **twins** of the frozen ABI structs — `depth_info` (128), `views_info` (124), `splat_info` (72), `stereo_calibration` (32) — asserted byte-identical to their `xrt_conv_*` counterparts field by field. They are separate types so a message never embeds a runtime struct, and the pack helpers copy field by field.
- `image` (96): `xrt_conv_image` minus every process-local field (`handle`, `import_key`, `cpu_ptr`, the sync handle and its `import_key`), plus `role` and `shm_offset` (a CPU image's place in the per-connection scratch shm). The receiver re-materialises the image; an `xrt_conv_image` never crosses.
- `params` (216): every `xrt_conv_params` field except `struct_size`, in the same order, with the stereo calibration twin at 184.

**Decisions.**
1. **Internal vocabulary on the wire.** Enumerated fields carry `XRT_CONV_*` values; the state tracker translates the app's `XrLift*DXR` values once, when it fills the struct (that is the one copy).
2. **No provider identity to apps.** `task_desc` drops `cap_id`, `licence_spdx`, `model_id`, `model_version`, `backend`, `display_name` and the state's `version` / `detail`; licence class and `commercial_ok` do cross.
3. **Image counts.** A submit carries at most 2 images (mono, a stereo pair, or colour + depth); a job at most 4 (`XRT_CONV_MAX_FRAME_IMAGES`). More is refused by the pack helper, never truncated. The reserved encoded-input fields of `xrt_conv_job_in` have no wire field.
4. **Small additions.** `XRT_CONV_WIRE_DESC_COMPOSED` (a task served by a runtime-composed pipeline), `XRT_CONV_WIRE_DELIVERY_*` for job results, `XRT_CONV_WIRE_EVENT_*` for the event kinds.

**Receive rule.** `xrt_conv_wire_receive` rejects a header whose `family_version` is not current or whose `struct_size` is below 8 or above what arrived; otherwise it copies `min(struct_size, sizeof)`, zero-extends, and leaves `struct_size` = the bytes actually covered, so `XRT_CONV_COVERS`-style checks see the truth. `family_version` bumps only on an incompatible change; appending at a message's tail does not bump it.

**`XRT_CONV_WIRE_MSG_MAX_BYTES` (1024) must equal `IPC_BUF_SIZE`**; P1a-8 asserts the pair when it adopts these structs.

**Regression test (G1 shape):** `tests/tests_conv_wire.cpp` round-trips every pack / unpack helper from a byte pattern and compares bytewise after zeroing only the fields each helper documents as not carried, and packs every message from two all-bytes-different sources that must differ in every non-reserved wire byte. A C11 translation unit compiles both headers as C.

**`conv_hint_interest` (the prewarm trigger).** The client sends it when an instance enables `XR_DXR_lift`: IPC sessions at `xrCreateInstance`, in-process sessions when their lift link opens. The task list defaults to `views-mono` + `depth-video`. What the service does with it follows the `PrewarmOn` policy: `interest` (default on mains power and on desktops), `first-create` (default on battery), `start` (kiosk opt-in, warms at service start). Readiness stays honestly ACTIVATING / WARMING meanwhile; a hint never blocks the caller.

Auth: the existing `require_lift_client` set (PRESENT_OWNER / APP / DIAG, `ipc_server_handler.c:8904-8914`). Per-connection quotas: ≤ 32 streams, ≤ 16 queued jobs. The 256 MiB `IPC_LIFT_BLOB_MAX_BYTES` cap stays for v3 only.

**One packed struct per family** — this ends the field-by-field copy trap that produced G1:
1. **One wire struct per message**, defined once in `src/xrt/include/xrt/xrt_conv_wire.h`. Each starts `uint32_t struct_size; uint32_t family_version;`, uses fixed-width types only, and embeds **no runtime structs** (today `xrt_lift_view_control`, `xrt_lift_rig` and `xrt_lift_depth_info` ride inside wire structs, `ipc_protocol.h:1160-1228`, so any internal change is a wire change).
2. **One copy.** The state tracker fills the wire struct straight from the XR struct (`oxr_conv.c`); the client sends it opaque; the server reads `memcpy(min(struct_size, sizeof))` with zero-extension. Adding a field = appending it to one struct and reading it where it is used.
3. **Size discipline.** `static_assert(sizeof(msg) <= IPC_BUF_SIZE)` on every message (the pattern at `ipc_server_handler.c:9325-9330`). Variable-sized data (viewpoints × rects, stereo calibration) goes into a per-connection conversion scratch shm, retiring the staged-viewpoints trick (`ipc_protocol.h:1208-1212`).
4. **Generator lint.** Every field of a `conv` wire struct is either read by the server unpack or listed as reserved — a CI check, not a convention.

The service ↔ conversion-host protocol (about ten fixed messages) is **hand-written**, not generated: one packed struct per message under the same rules 1–3 (fixed-width types, `struct_size` + `family_version` header, no embedded runtime structs, `static_assert` on every size), in a header shared by the service and the host. There is no second proto-generator spec. Images cross that boundary as handles plus a re-materialised descriptor, never as a copied `xrt_conv_image`.
