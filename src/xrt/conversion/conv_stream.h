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

#include "xrt/xrt_conversion_plugin.h"

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
};

//! A pinned result (conv_core_stream_acquire); release with its ring slot.
struct conv_result
{
	uint64_t frame_id;
	int64_t source_time;
	uint64_t latency_ns;
	uint32_t ring_slot;
	uint32_t written; //!< XRT_CONV_OUT_*
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
	uint64_t lat_last_ns, lat_ema_ns;
	float rate_hz;
	uint32_t row_state; //!< XRT_CONV_STATE_* of the bound capability
	bool provisional;
	bool unsatisfied; //!< READY but lacking a required output: frames are dropped
	bool composed;
	char provider_id[64]; //!< diagnostics only, never the app API
	uint32_t cap_id;
};

//! Per-stream options the v3 handlers / service add to the wire descriptor.
struct conv_stream_options
{
	uint32_t priority;      //!< enum u_lift_priority; 0 → NORMAL
	bool weave_rect_source; //!< S1 letterbox + input cap
	bool auto_convergence;  //!< S7 runtime auto-convergence (needs a configured observer)
	uint64_t eyes_dp;       //!< DP whose eyes snapshot to read: a conv_core_eyes_publish key, CONV_EYES_DP_ANY or _NONE
	uint32_t source_format; //!< XRT_CONV_FMT_* the producer snapshots; 0 = RGBA8
};

#ifdef __cplusplus
}
#endif
