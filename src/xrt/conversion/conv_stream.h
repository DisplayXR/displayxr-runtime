// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Conversion stream lane (ADR-053 decision 3; roadmap
 *         conversion-providers.md §2.6, §5.2): stream objects and one
 *         dispatcher thread per provider host.
 *
 * A stream keeps ADR-042's shape: a latest-wins u_lift_mailbox (two input
 * slots, two output ring slots, pins, drop accounting) per stream, and the
 * u_lift_sched planner (PAUSED / LOW / NORMAL / HIGH) per host — both reused
 * unchanged. What changes against d3d11_lift's single lift thread:
 *
 *  - one dispatcher per provider HOST, so streams on different providers no
 *    longer serialise behind one another;
 *  - every image is runtime-allocated (conv_image_alloc_ops) and registered
 *    with the host once per slot lifetime; the provider writes straight into
 *    the ring slot's targets (no copy-out of a module-owned texture);
 *  - per-stream temporal state: FRAME_RESET on an app cut / seek or a
 *    rebind, and a `discontinuity` flag on the first result after it;
 *  - frames submitted while the bound capability is not READY wait
 *    (latest-wins); a stream created against describe-time outputs is
 *    PROVISIONAL until READY, then refined;
 *  - rebinding: a live stream moves only when its capability reaches a
 *    terminal state (any FAILED) or its host drains; the move is signalled by
 *    `discontinuity` (roadmap §2.4, the one rebinding rule).
 *
 * Locks (roadmap §5.2): conv.streams_mtx (one, shared by every dispatcher;
 * held between commands only) → registry_mtx / host mutexes (leaves). No
 * conversion lock is held across a provider call, and nothing here takes the
 * compositor's render_mutex.
 *
 * The functions are the core's (conv_core.h); this header holds the types the
 * service and the v3 handlers exchange with it.
 *
 * @ingroup aux_conversion
 */

#pragma once

#include "xrt/xrt_conv_wire.h"
#include "xrt/xrt_conversion_plugin.h"
#include "xrt/xrt_handles.h"

#include "util/u_lift_rig.h"
#include "util/u_lift_viewpoint.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! @name conv_image_request::usage @{ */
#define CONV_IMAGE_USAGE_INPUT (1u << 0)         //!< a producer snapshot target (S0)
#define CONV_IMAGE_USAGE_PROVIDER_OUT (1u << 1)  //!< a provider writes it
#define CONV_IMAGE_USAGE_RENDER_TARGET (1u << 2) //!< the reference view synthesis renders into it
#define CONV_IMAGE_USAGE_EXPORT (1u << 3)        //!< exported to clients (shared + fence)
//! #2030: a GPU mirror of a CPU-memory provider result (conv_image_alloc_ops::upload)
#define CONV_IMAGE_USAGE_MIRROR (1u << 4)
//! ADR-058: a ring slot's copy of the converted input, which the presenter's
//! late view synthesis samples (conv_image_alloc_ops::copy fills it)
#define CONV_IMAGE_USAGE_LATE_SOURCE (1u << 5)
/*! @} */

//! A runtime-allocated image the core asks the GPU glue for.
struct conv_image_request
{
	uint32_t width, height;
	uint32_t format;       //!< XRT_CONV_FMT_*
	uint32_t array_layers; //!< ≥ 1 (ARRAY views layout: view_count)
	uint32_t role;         //!< XRT_CONV_OUT_* for outputs; 0 for inputs
	uint32_t usage;        //!< CONV_IMAGE_USAGE_*
	uint64_t stream_id;
	//! What the bound capability imports (xrt_conv_capability::import_*):
	//! the allocator picks a memory kind / sync the provider accepts (#2003).
	//! 0 = unknown (any).
	uint32_t import_memory_kinds;
	uint32_t import_sync_kinds;
};

/*! Runtime image allocation: the GPU glue (service) or CPU memory (tests,
 *  CPU providers). Called on producer and dispatcher threads, never with a
 *  conversion lock held. */
struct conv_image_alloc_ops
{
	bool (*alloc)(void *ud, const struct conv_image_request *req, struct xrt_conv_image *out);
	void (*free)(void *ud, struct xrt_conv_image *img);
	/*! Optional (#2030): copy CPU-memory image @p src into @p dst, a
	 *  CONV_IMAGE_USAGE_MIRROR image this allocator made with @p src's extent,
	 *  format and layer count. A CPU-only provider's views / depth then reach
	 *  GPU-only consumers (the weave path samples a texture): the dispatcher
	 *  uploads each result into a per-ring-slot mirror and publishes the
	 *  mirror. NULL = CPU results are published as they are. */
	bool (*upload)(void *ud, const struct xrt_conv_image *src, const struct xrt_conv_image *dst);
	/*! Optional (ADR-058): copy @p src (a producer snapshot, CPU or this
	 *  allocator's GPU image, honouring its sync) into @p dst, a
	 *  CONV_IMAGE_USAGE_LATE_SOURCE image this allocator made with @p src's
	 *  extent and format; complete when it returns. NULL = composed results
	 *  keep their views (no late view synthesis). */
	bool (*copy)(void *ud, const struct xrt_conv_image *src, const struct xrt_conv_image *dst);
	void *ud;
};

//! Images a producer writes for one submit (≤ XRT_CONV_WIRE_MAX_SUBMIT_IMAGES).
#define CONV_STREAM_MAX_SUBMIT_IMAGES 2u

//! What a producer will snapshot (conv_core_stream_begin_submit).
struct conv_submit_layout
{
	uint32_t image_count;
	uint32_t width[CONV_STREAM_MAX_SUBMIT_IMAGES];
	uint32_t height[CONV_STREAM_MAX_SUBMIT_IMAGES];
	uint32_t format[CONV_STREAM_MAX_SUBMIT_IMAGES];      //!< XRT_CONV_FMT_*
	uint32_t color_space[CONV_STREAM_MAX_SUBMIT_IMAGES]; //!< XRT_CONV_CS_*
	uint32_t role[CONV_STREAM_MAX_SUBMIT_IMAGES];        //!< XRT_CONV_IMG_*
};

//! Where to snapshot (valid until commit / abort).
struct conv_submit_slot
{
	int32_t slot;
	uint32_t image_count;
	struct xrt_conv_image images[CONV_STREAM_MAX_SUBMIT_IMAGES];
};

/*! Everything a producer hands with one frame: the app's inputs to the
 *  runtime's policy stages (never the policy's result). */
struct conv_frame_submit
{
	int64_t source_time;
	uint32_t frame_flags; //!< XRT_CONV_FRAME_* (RESET = an app cut / seek)
	struct xrt_conv_depth_info depth_in_info;

	/* S3 inputs. */
	uint32_t explicit_count; //!< app viewpoints (display space); 0 = tracked
	float explicit_xyz[XRT_CONV_MAX_VIEWS][3];
	bool has_view_control; //!< false = the ADR-048 defaults
	struct u_lift_view_control vc;
	struct u_lift_rig rig;
	bool rect_valid;
	float rect_center[3];
	float rect_w_m, rect_h_m;
	float active[4];         //!< letterbox active area x0,y0,x1,y1 in [0,1]; all 0 = whole frame
	bool convergence_set;    //!< the app gave a plane
	float convergence_rel01; //!< [0,1] relative depth at the glass
	float strength;          //!< ≤ 0 = 1
	float focal_px;          //!< ≤ 0 = unknown
	struct xrt_conv_stereo_calibration stereo;

	/*! Still-image dedupe (PHOTO streams): the producer's identity of this
	 *  input's content (shared-texture identity combined with a cheap content
	 *  check); equal keys mean an unchanged picture. 0 = unknown: the core
	 *  hashes CPU-memory inputs itself, and a GPU input without a key always
	 *  converts. */
	uint64_t content_key;
	/*! Rebind the stream's eyes to the DP that owns the rect this frame was
	 *  taken from (conv_core_stream_bind_eyes); false = keep the binding. */
	bool eyes_dp_set;
	uint64_t eyes_dp;
	/*! ADR-058: a lifted weave rect's snapshot (the presenter weaves this
	 *  frame's result itself): its views may be synthesized late. */
	bool weave_rect;
};

//! A pinned result (conv_core_stream_acquire); release with its ring slot.
struct conv_result
{
	uint64_t frame_id;
	int64_t source_time;
	uint64_t latency_ns;
	uint32_t ring_slot;
	uint32_t written; //!< XRT_CONV_OUT_* the app asked for (what the stream publishes)
	/*! XRT_CONV_OUT_* the provider wrote this frame, before the S8 mask: a
	 *  superset of `written`. The service's own consumers (the lifted-rect
	 *  depth cursor, ADR-046 Amendment 1) read the observer-only depth
	 *  (#2003) through it; the app-facing export follows `written` only. */
	uint32_t provider_written;
	struct xrt_conv_image views, depth, confidence, disparity;
	struct xrt_conv_views_info views_info;
	struct xrt_conv_depth_info depth_info;
	float active[4];
	//! ADR-048 echo (#2003): the lifted rect the viewpoints were resolved
	//! against, as submitted (display space, metres; 0 = unknown).
	float rect_center[3];
	float rect_size[2];
	bool discontinuity;  //!< first result after FRAME_RESET or a rebind
	bool provisional;    //!< the stream's outputs were not refined yet
	bool output_realloc; //!< an output target changed since this consumer's last acquire
	/*! ADR-058: a composed result kept as its source (no views written): the
	 *  presenter synthesizes the views late (conv_core_stream_late_prepare). */
	bool late;
};

//! Stream statistics (INFO-logged, throttled; status snapshot).
struct conv_stream_stats
{
	uint64_t submitted, converted, dropped, failed;
	//! PHOTO frames answered by republishing the last result (unchanged input
	//! and policy): they never reach the provider and never count as converted.
	uint64_t deduped;
	uint64_t blocked_by_job;  //!< converts that waited behind a non-concurrent job
	uint64_t blocked_by_warm; //!< converts that waited behind a non-concurrent warm
	uint64_t rebinds;
	uint64_t reopens; //!< provider stream re-opened after a satellite host restart
	uint64_t lat_last_ns, lat_ema_ns;
	float rate_hz;
	uint32_t row_state; //!< XRT_CONV_STATE_* of the bound capability
	bool provisional;
	bool unsatisfied; //!< READY but lacking a required output: frames are dropped
	bool composed;
	char provider_id[64]; //!< diagnostics only, never the app API
	uint32_t cap_id;
	/*! ADR-058 decision 4, INTERNAL until the append-only XrLiftStreamStatsDXR
	 *  change lands with the ADR (not on the wire): the latest result's content
	 *  age (now - its frame's submit) and placement age (now - the eyes its
	 *  views were placed for). Equal-ish today (placement happens at
	 *  conversion); late placement makes the second one a display-frame age.
	 *  0 / 0 before the first result. */
	uint64_t content_age_ns;
	uint64_t placement_age_ns;
	//! ADR-058: presenter frames that placed a late source, and how many of
	//! them re-synthesized (the cache rule reused the views for the rest).
	uint64_t late_prepares;
	uint64_t late_reruns;
};

//! Per-stream options the v3 handlers / service add to the wire descriptor.
struct conv_stream_options
{
	uint32_t priority;      //!< enum u_lift_priority; 0 → NORMAL
	bool weave_rect_source; //!< S1 letterbox + input cap
	bool auto_convergence;  //!< S7 runtime auto-convergence (needs a configured observer)
	uint64_t eyes_dp;       //!< DP whose eyes snapshot to read: a conv_core_eyes_publish key, CONV_EYES_DP_ANY or _NONE
	uint32_t source_format; //!< XRT_CONV_FMT_* the producer snapshots; 0 = RGBA8
	/*! Per-stream preference HINT (lift spec 5, #2178): "<provider-id>[/<cap_id
	 *  or model_id>]", "" = none. Reorders the non-terminal candidates at
	 *  create (and at a rebind of this stream); never the user's default. */
	char preferred_provider[XRT_CONV_WIRE_PREFERENCE_MAX];
};

/*! @name conv_input_import::kind @{ */
#define CONV_INPUT_NT 1u   //!< a D3D11 or D3D12 shared NT texture handle (keyed mutex if it has one)
#define CONV_INPUT_DXGI 2u //!< a legacy global DXGI shared handle (process-independent)
#define CONV_INPUT_CPU 3u  //!< a file mapping holding the bytes (RGBA8 / BGRA8, cpu_row_pitch)
//! ADR-053 P5 step E, the POSIX GPU kinds (a GPU glue imports them; img_* fields):
#define CONV_INPUT_IOSURFACE 4u //!< a global IOSurface (macOS)
#define CONV_INPUT_OPAQUE_FD 5u //!< a Vulkan OPAQUE_FD allocation (Linux; the transport contract's image)
#define CONV_INPUT_DMABUF 6u    //!< a single-plane dma-buf, plane 0 at offset 0 (Linux)
/*! @} */

/*!
 * One spec-4 input image as the service's GPU glue imports it (#2054): the
 * handle(s) the transport delivered plus the region to convert. The glue
 * takes ownership of both handles whatever the outcome.
 */
struct conv_input_import
{
	uint32_t kind; //!< CONV_INPUT_*
	xrt_graphics_buffer_handle_t handle;
	uint32_t sync_kind; //!< XRT_CONV_SYNC_NONE / KEYED_MUTEX / FENCE_NT
	xrt_graphics_buffer_handle_t sync_handle; //!< FENCE_NT: a shared D3D fence; else invalid
	uint64_t sync_value;                      //!< FENCE_NT: wait for this value
	uint32_t x, y, w, h;                      //!< the region of the image to convert
	uint32_t cpu_row_pitch;                   //!< CPU: bytes per row of the mapping; DMABUF: plane 0 stride
	bool cpu_bgra;                            //!< CPU: BGRA8 rather than RGBA8
	//! @name GPU kinds (CONV_INPUT_IOSURFACE / _OPAQUE_FD / _DMABUF)
	//! @{
	uint32_t img_w, img_h; //!< the whole image (the region lies inside it)
	uint32_t img_format;   //!< XRT_CONV_FMT_*
	uint64_t drm_modifier; //!< DMABUF
	                       //! @}
};

#ifdef __cplusplus
}
#endif
