// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Packed wire structs of the `conv_*` IPC message family (ADR-053).
 *
 * XR_DXR_lift spec 4 / 5 ride a new IPC family next to the byte-frozen v3
 * `lift_*` messages. This header defines its payloads ONCE, so the bug class
 * that produced G1 (aux-depth caps silently dropped over IPC by a
 * field-by-field copy that forgot two fields) cannot recur:
 *
 *  1. ONE WIRE STRUCT PER MESSAGE FAMILY. Each message struct starts with
 *     `uint32_t struct_size; uint32_t family_version;`, uses fixed-width types
 *     only, has no hidden padding (every gap is an explicit reserved_* field,
 *     and every size is asserted), and embeds NO runtime struct. Where a
 *     payload carries one of the FROZEN plug-in ABI structs of
 *     xrt_conversion_plugin.h (depth / views / splat info, stereo
 *     calibration), it embeds a wire TWIN of it whose layout is asserted
 *     byte-identical, field by field, at the end of this header.
 *  2. ONE COPY. The sender fills the wire struct once; the transport moves it
 *     opaque; the receiver reads it with xrt_conv_wire_receive(), which copies
 *     min(struct_size, sizeof) and zero-extends. Adding a field = appending it
 *     to one struct (never reordering) and reading it where it is used.
 *  3. SIZE DISCIPLINE. Every message fits XRT_CONV_WIRE_MSG_MAX_BYTES (==
 *     IPC_BUF_SIZE; the IPC layer re-asserts the pair when it adopts these).
 *  4. EVERY FIELD IS ACCOUNTED FOR. The pack / unpack helpers below move every
 *     field of the ABI struct they mirror, except the ones each helper lists
 *     as process-local or identity; tests_conv_wire round-trips each helper
 *     with a byte pattern and compares bytewise, so a dropped field fails CI.
 *
 * Images never cross as bytes or as an xrt_conv_image: handles travel out of
 * band (the IPC handle array), CPU pixels in the per-connection scratch shm,
 * and the receiver re-materialises the image. The wire image therefore drops
 * every process-local field (handle, import_key, cpu_ptr, the sync handle and
 * its import_key).
 *
 * Enumerated fields carry the XRT_CONV_* values of xrt_conversion_plugin.h:
 * the sender translates app-facing XR values once, when it fills the struct.
 *
 * Message map (the conv_* table of
 * docs/roadmap/conversion-providers-headers-draft.md §4; P1a-8 wires these
 * into ipc_protocol.h / proto.json):
 *
 * | Message                          | Request                       | Reply                          |
 * |----------------------------------|-------------------------------|--------------------------------|
 * | conv_hint_interest               | xrt_conv_wire_hint_interest   | –                              |
 * | conv_get_generation              | –                             | xrt_conv_wire_generation       |
 * | conv_enum_task                   | task index (u32)              | xrt_conv_wire_task_desc        |
 * | conv_stream_create               | xrt_conv_wire_stream_desc     | xrt_conv_wire_stream_created   |
 * | conv_stream_destroy / _reset     | stream id (u64)               | –                              |
 * | conv_submit                      | xrt_conv_wire_submit          | frame id (u64)                 |
 * | conv_acquire_result              | stream id (u64)               | xrt_conv_wire_result           |
 * | conv_job_submit                  | xrt_conv_wire_job_desc        | job id (u64)                   |
 * | conv_job_status                  | job id (u64)                  | xrt_conv_wire_job_status       |
 * | conv_job_fetch                   | job id (u64)                  | xrt_conv_wire_job_result       |
 * | conv_job_cancel                  | job id (u64)                  | –                              |
 * | conv_warm / conv_evict           | xrt_conv_wire_task_request    | –                              |
 * | events / conv_poll_event         | –                             | xrt_conv_wire_event            |
 *
 * conv_input_ring_get is P5 only and has no struct here yet.
 *
 * Self-contained C11 / C++: only xrt_conversion_plugin.h and the C library.
 *
 * @ingroup ipc
 */

#pragma once

#include "xrt/xrt_conversion_plugin.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif


/*
 *
 * Family constants.
 *
 */

/*!
 * Version of the conv_* wire family. Appending a field at the tail of a
 * struct does NOT bump it (struct_size covers that); an incompatible change
 * (reorder, retype, removal) does, and a receiver rejects any other value.
 */
#define XRT_CONV_WIRE_FAMILY_VERSION_1 1u
#define XRT_CONV_WIRE_FAMILY_VERSION_CURRENT XRT_CONV_WIRE_FAMILY_VERSION_1

/*! Largest message, in bytes. Equal to IPC_BUF_SIZE (ipc_protocol.h). */
#define XRT_CONV_WIRE_MSG_MAX_BYTES 1024u

/*! Bytes of the common header (struct_size + family_version). */
#define XRT_CONV_WIRE_HEADER_BYTES 8u

/*! Input images per stream submit: mono, a stereo pair, or colour + depth. */
#define XRT_CONV_WIRE_MAX_SUBMIT_IMAGES 2u

/*! Bytes of a job's progress stage label, NUL-terminated. */
#define XRT_CONV_WIRE_STAGE_MAX 32u

/*! @name xrt_conv_wire_task_desc::desc_flags, xrt_conv_wire_stream_created::desc_flags @{ */
//! The task is served by a runtime-composed pipeline (a depth task + the
//! runtime's reference view synthesis), not by one provider capability.
#define XRT_CONV_WIRE_DESC_COMPOSED (1u << 0)
/*! @} */

/*! @name xrt_conv_wire_job_result::delivery @{ */
#define XRT_CONV_WIRE_DELIVERY_NONE 0u   //!< nothing to fetch (failed / cancelled / no blob output)
#define XRT_CONV_WIRE_DELIVERY_INLINE 1u //!< ≤ 1 MiB, in the reply's scratch shm section
#define XRT_CONV_WIRE_DELIVERY_SHM 2u    //!< a dedicated shm handle rides the reply
#define XRT_CONV_WIRE_DELIVERY_FILE 3u   //!< written to the caller's file handle
/*! @} */

/*! @name xrt_conv_wire_event::kind @{ */
#define XRT_CONV_WIRE_EVENT_PROVIDERS_CHANGED 1u
#define XRT_CONV_WIRE_EVENT_TASK_STATE_CHANGED 2u
#define XRT_CONV_WIRE_EVENT_JOB_STATE_CHANGED 3u
/*! @} */


/*
 *
 * Frozen sub-blocks: wire twins of the frozen ABI structs.
 *
 * Byte-identical to their xrt_conversion_plugin.h counterparts (asserted at
 * the end of this header). They are separate types so that a message never
 * embeds a runtime struct, and so the pack helpers copy field by field.
 *
 */

//! Twin of xrt_conv_stereo_calibration. sizeof == 32.
struct xrt_conv_wire_stereo_calibration
{
	uint32_t eye_order;
	float fx, fy, cx, cy;
	float baseline_m;
	float infinity_shift_px;
	uint32_t sbs_packed;
};

//! Twin of xrt_conv_depth_info. sizeof == 128.
struct xrt_conv_wire_depth_info
{
	uint32_t kind;
	uint32_t measure;
	uint32_t intrinsics_source;
	float value_scale, value_shift;
	float near_value, far_value;
	float invalid_sample;
	float range_min, range_max;
	float range_p05, range_p95;
	float fx, fy, cx, cy;
	uint32_t source_width, source_height;
	float convergence_value;
	float suggested_convergence;
	float suggested_strength;
	uint32_t suggest_flags;
	uint32_t relief_valid;
	float relief_scale, relief_offset;
	uint32_t temporal;
	uint32_t same_inference;
	uint32_t has_confidence;
	uint64_t source_time_ns;
	uint64_t provider_frame_id;
};

//! Twin of xrt_conv_views_info. sizeof == 124.
struct xrt_conv_wire_views_info
{
	uint32_t view_count;
	uint32_t layout;
	uint32_t reserved_0;
	float eye_xyz[XRT_CONV_MAX_VIEWS][3];
	float baseline_m;
	float hfov_deg;
	float zero_parallax_disparity;
	float reserved_1;
};

//! Twin of xrt_conv_splat_info. sizeof == 72.
struct xrt_conv_wire_splat_info
{
	uint32_t blob_format;
	uint32_t coords;
	uint32_t color_space;
	uint32_t metric;
	uint32_t intrinsics_source;
	float fx, fy, cx, cy;
	uint32_t grid_width, grid_height;
	float baseline_m;
	float focus_z_m, near_m, far_m;
	uint32_t reserved_0;
	uint64_t splat_count;
};


/*
 *
 * Frozen sub-blocks without an ABI twin.
 *
 */

/*!
 * One input image descriptor: xrt_conv_image minus every process-local field.
 * The handle (and the sync handle) ride the IPC handle array in descriptor
 * order; a MEM_CPU image's bytes sit at @c shm_offset of the connection's
 * scratch shm. FROZEN: sizeof == 96.
 */
struct xrt_conv_wire_image
{
	uint32_t memory_kind; //!< exactly one XRT_CONV_MEM_* bit; 0 = absent
	uint32_t role;        //!< XRT_CONV_IMG_*
	uint32_t width, height;
	uint32_t format;      //!< XRT_CONV_FMT_*
	uint32_t color_space; //!< XRT_CONV_CS_*
	uint32_t row_stride;
	uint32_t plane1_offset;
	uint32_t plane1_stride;
	uint32_t array_layer;
	uint32_t sync_kind; //!< XRT_CONV_SYNC_*
	uint32_t reserved_0;
	uint64_t byte_size;
	uint64_t drm_modifier;
	uint64_t adapter_luid;
	uint64_t sync_acquire_value;
	uint64_t sync_release_value;
	uint64_t shm_offset; //!< XRT_CONV_MEM_CPU only: offset in the scratch shm
};

/*!
 * Every xrt_conv_params field except struct_size, in the same order. Frozen:
 * a field appended to xrt_conv_params goes at the tail of the MESSAGE that
 * carries this block, not in here. sizeof == 216.
 */
struct xrt_conv_wire_params
{
	uint32_t frame_flags; //!< XRT_CONV_FRAME_*
	uint32_t reserved_0;
	uint64_t source_time_ns;
	uint64_t frame_id;
	uint32_t convergence_mode;
	float convergence;
	float strength;
	uint32_t view_count;
	uint32_t content_hint;
	float input_scale;
	float focal_px;
	uint32_t viewpoint_frame;
	uint32_t viewpoint_source;
	uint32_t axis_mode;
	uint32_t viewpoint_count;
	float viewpoints_xyz[XRT_CONV_MAX_VIEWS][3];
	float rect_width_m, rect_height_m;
	float baseline_m;
	float max_offset_m;
	float nominal_z_m;
	struct xrt_conv_wire_stereo_calibration stereo;
};


/*
 *
 * Messages.
 *
 */

//! conv_hint_interest request: the prewarm trigger. sizeof == 16.
struct xrt_conv_wire_hint_interest
{
	uint32_t struct_size;
	uint32_t family_version;
	uint64_t task_mask; //!< bit (1 << XRT_CONV_TASK_*); 0 = the default set
};

//! conv_get_generation reply. sizeof == 24.
struct xrt_conv_wire_generation
{
	uint32_t struct_size;
	uint32_t family_version;
	uint64_t conv_gen;   //!< moves when any task's effective capability changes
	uint64_t status_gen; //!< moves when any task's readiness changes
};

/*!
 * conv_enum_task reply: the EFFECTIVE capability of one task (the selected
 * provider's capability, or the composed pipeline's) plus its readiness. No
 * provider identity crosses here: xrt_conv_capability's cap_id, licence_spdx,
 * model_id, model_version, backend and display_name, and xrt_conv_state's
 * version and detail, stay in the service (CLI / status snapshot only).
 * sizeof == 144.
 */
struct xrt_conv_wire_task_desc
{
	uint32_t struct_size;
	uint32_t family_version;

	/* Readiness (xrt_conv_state). */
	uint32_t task; //!< XRT_CONV_TASK_*
	uint32_t state;
	uint32_t failure_reason;
	uint32_t retryable;
	float progress;
	uint32_t desc_flags; //!< XRT_CONV_WIRE_DESC_*
	uint64_t eta_ns;

	/* Capability (xrt_conv_capability), in its order. */
	uint32_t lanes;
	uint32_t input_kinds;
	uint32_t outputs;
	uint32_t flags;
	uint32_t import_memory_kinds;
	uint32_t import_sync_kinds;
	uint32_t input_formats;
	uint32_t depth_output_format;
	uint32_t min_views, max_views;
	uint32_t max_streams;
	uint32_t max_concurrent;
	uint32_t max_input_edge;
	uint32_t preferred_input_edge;
	uint32_t depth_kind;
	uint32_t depth_measure;
	uint32_t intrinsics_source;
	uint32_t splat_formats;
	uint32_t licence_class;
	uint32_t commercial_ok;
	uint32_t est_warmup_ms;
	uint32_t est_latency_ms;
	uint32_t est_vram_mb;
	uint32_t est_ram_mb;
	uint64_t device_luid;
};

/*!
 * conv_stream_create request: every xrt_conv_stream_info field, plus what the
 * service needs for selection and export. sizeof == 56.
 */
struct xrt_conv_wire_stream_desc
{
	uint32_t struct_size;
	uint32_t family_version;
	uint32_t task;               //!< XRT_CONV_TASK_*
	uint32_t quality_hint;       //!< XrLiftQualityHintDXR value (0 = default)
	uint32_t export_memory_kind; //!< one XRT_CONV_MEM_* bit; 0 = platform default
	/* xrt_conv_stream_info, in its order. */
	uint32_t input_kind;
	uint32_t outputs; //!< XRT_CONV_OUT_* REQUIRED: refuse rather than degrade
	uint32_t content_hint;
	uint32_t view_count; //!< 0 = match the weave / 2
	uint32_t views_layout;
	uint32_t max_input_width, max_input_height;
	uint32_t reserved_0;
	uint32_t reserved_1;
};

//! conv_stream_create reply: the stream and its resolved pipeline. sizeof == 40.
struct xrt_conv_wire_stream_created
{
	uint32_t struct_size;
	uint32_t family_version;
	uint64_t stream_id;
	uint32_t task;
	uint32_t outputs;    //!< XRT_CONV_OUT_* the stream will produce
	uint32_t view_count; //!< resolved (0 in the request is replaced)
	uint32_t views_layout;
	uint32_t desc_flags; //!< XRT_CONV_WIRE_DESC_*
	uint32_t reserved_0;
};

/*!
 * conv_submit request. images[i] for i < image_count; their handles ride the
 * IPC handle array in the same order (image handle, then sync handle when the
 * sync kind has one). depth_in_info describes a XRT_CONV_IMG_DEPTH_IN image.
 * sizeof == 560.
 */
struct xrt_conv_wire_submit
{
	uint32_t struct_size;
	uint32_t family_version;
	uint64_t stream_id;
	uint32_t image_count;
	uint32_t reserved_0;
	struct xrt_conv_wire_image images[XRT_CONV_WIRE_MAX_SUBMIT_IMAGES];
	struct xrt_conv_wire_params params;
	struct xrt_conv_wire_depth_info depth_in_info;
};

/*!
 * conv_acquire_result reply: one result's metadata. Export handles come from
 * the separate get-output calls, only when @c output_realloc says so (the v3
 * weave-output pattern). Carries the extent and format of each of the four
 * targets of xrt_conv_frame_out, and its views / depth info. sizeof == 352.
 */
struct xrt_conv_wire_result
{
	uint32_t struct_size;
	uint32_t family_version;
	uint64_t stream_id;
	uint64_t frame_id;
	uint64_t source_time_ns; //!< the submit's, verbatim
	uint64_t latency_ns;     //!< submit → result published
	uint32_t written;        //!< XRT_CONV_OUT_* actually written
	uint32_t output_realloc; //!< 1 = an export target changed since the last acquire
	uint32_t views_width, views_height, views_format;
	uint32_t depth_width, depth_height, depth_format;
	uint32_t confidence_width, confidence_height, confidence_format;
	uint32_t disparity_width, disparity_height, disparity_format;
	struct xrt_conv_wire_views_info views_info;
	uint32_t reserved_0;
	struct xrt_conv_wire_depth_info depth_info;
};

/*!
 * conv_job_submit request. Decoded pixels only (XRT_CONV_INPUT_ENCODED is
 * reserved and has no wire field). sizeof == 632.
 */
struct xrt_conv_wire_job_desc
{
	uint32_t struct_size;
	uint32_t family_version;
	uint32_t task;
	uint32_t quality_hint;
	/* xrt_conv_job_in, minus the reserved encoded fields. */
	uint32_t input_kind;
	uint32_t image_count;
	uint32_t requested_blob_format; //!< XRT_CONV_BLOB_*; 0 = provider default
	uint32_t reserved_0;
	struct xrt_conv_wire_image images[XRT_CONV_MAX_FRAME_IMAGES];
	struct xrt_conv_wire_params params;
};

//! conv_job_status reply. sizeof == 64.
struct xrt_conv_wire_job_status
{
	uint32_t struct_size;
	uint32_t family_version;
	uint64_t job_id;
	uint32_t state;          //!< XrLiftJobStateDXR value (spec 5)
	uint32_t failure_reason; //!< XRT_CONV_FAIL_*
	float progress;          //!< [0,1]; < 0 = unknown
	uint32_t reserved_0;
	char stage[XRT_CONV_WIRE_STAGE_MAX]; //!< provider progress label, verbatim, NUL-terminated
};

/*!
 * conv_job_fetch reply: the job's xrt_conv_job_out plus where its blob is.
 * sizeof == 368.
 */
struct xrt_conv_wire_job_result
{
	uint32_t struct_size;
	uint32_t family_version;
	uint64_t job_id;
	uint32_t written;     //!< XRT_CONV_OUT_* delivered
	uint32_t blob_format; //!< XRT_CONV_BLOB_* of the delivered blob
	uint32_t delivery;    //!< XRT_CONV_WIRE_DELIVERY_*
	uint32_t reserved_0;
	uint64_t byte_size;
	struct xrt_conv_wire_splat_info splat_info;
	struct xrt_conv_wire_depth_info depth_info;
	struct xrt_conv_wire_views_info views_info;
	uint32_t reserved_1;
};

//! conv_warm / conv_evict request. sizeof == 16.
struct xrt_conv_wire_task_request
{
	uint32_t struct_size;
	uint32_t family_version;
	uint32_t task; //!< XRT_CONV_TASK_*
	uint32_t reserved_0;
};

//! One conversion event (the per-session queue / conv_poll_event). sizeof == 40.
struct xrt_conv_wire_event
{
	uint32_t struct_size;
	uint32_t family_version;
	uint32_t kind;           //!< XRT_CONV_WIRE_EVENT_*
	uint32_t task;           //!< TASK_STATE_CHANGED; else 0
	uint32_t state;          //!< TASK_STATE_CHANGED: XRT_CONV_STATE_*; JOB_STATE_CHANGED: job state
	uint32_t previous_state; //!< TASK_STATE_CHANGED
	uint32_t failure_reason; //!< XRT_CONV_FAIL_*
	float progress;          //!< < 0 = unknown
	uint64_t job_id;         //!< JOB_STATE_CHANGED; else 0
};


/*
 *
 * Generic header helpers.
 *
 */

/*!
 * Zero @p msg and stamp its header: struct_size = @p size, family_version =
 * current. Call before filling any message.
 */
static inline void
xrt_conv_wire_begin(void *msg, uint32_t size)
{
	const uint32_t hdr[2] = {size, XRT_CONV_WIRE_FAMILY_VERSION_CURRENT};
	memset(msg, 0, size);
	memcpy(msg, hdr, sizeof(hdr));
}

/*!
 * Receive a message of this family: validate the header, copy min(sender's
 * struct_size, @p dst_size) bytes, zero-extend the rest, and leave
 * dst->struct_size = the number of bytes actually covered, so a field past it
 * reads as zero AND XRT_CONV_COVERS-style checks see the truth.
 *
 * @param dst       receiver's struct; @p dst_size = its sizeof
 * @param src       bytes as they arrived
 * @param src_bytes how many arrived
 * @return false (dst zeroed) if the header is malformed, the family version
 *         is not current, or struct_size exceeds what arrived.
 */
static inline bool
xrt_conv_wire_receive(void *dst, uint32_t dst_size, const void *src, uint32_t src_bytes)
{
	uint32_t hdr[2];
	uint32_t n;

	memset(dst, 0, dst_size);
	if (src == NULL || dst_size < XRT_CONV_WIRE_HEADER_BYTES || src_bytes < XRT_CONV_WIRE_HEADER_BYTES) {
		return false;
	}
	memcpy(hdr, src, sizeof(hdr));
	if (hdr[0] < XRT_CONV_WIRE_HEADER_BYTES || hdr[0] > src_bytes ||
	    hdr[1] != XRT_CONV_WIRE_FAMILY_VERSION_CURRENT) {
		return false;
	}
	n = hdr[0] < dst_size ? hdr[0] : dst_size;
	memcpy(dst, src, n);
	memcpy(dst, &n, sizeof(n));
	return true;
}


/*
 *
 * Pack / unpack: frozen twins (every field, both directions).
 *
 */

static inline void
xrt_conv_wire_pack_stereo_calibration(const struct xrt_conv_stereo_calibration *in,
                                      struct xrt_conv_wire_stereo_calibration *out)
{
	out->eye_order = in->eye_order;
	out->fx = in->fx;
	out->fy = in->fy;
	out->cx = in->cx;
	out->cy = in->cy;
	out->baseline_m = in->baseline_m;
	out->infinity_shift_px = in->infinity_shift_px;
	out->sbs_packed = in->sbs_packed;
}

static inline void
xrt_conv_wire_unpack_stereo_calibration(const struct xrt_conv_wire_stereo_calibration *in,
                                        struct xrt_conv_stereo_calibration *out)
{
	out->eye_order = in->eye_order;
	out->fx = in->fx;
	out->fy = in->fy;
	out->cx = in->cx;
	out->cy = in->cy;
	out->baseline_m = in->baseline_m;
	out->infinity_shift_px = in->infinity_shift_px;
	out->sbs_packed = in->sbs_packed;
}

static inline void
xrt_conv_wire_pack_depth_info(const struct xrt_conv_depth_info *in, struct xrt_conv_wire_depth_info *out)
{
	out->kind = in->kind;
	out->measure = in->measure;
	out->intrinsics_source = in->intrinsics_source;
	out->value_scale = in->value_scale;
	out->value_shift = in->value_shift;
	out->near_value = in->near_value;
	out->far_value = in->far_value;
	out->invalid_sample = in->invalid_sample;
	out->range_min = in->range_min;
	out->range_max = in->range_max;
	out->range_p05 = in->range_p05;
	out->range_p95 = in->range_p95;
	out->fx = in->fx;
	out->fy = in->fy;
	out->cx = in->cx;
	out->cy = in->cy;
	out->source_width = in->source_width;
	out->source_height = in->source_height;
	out->convergence_value = in->convergence_value;
	out->suggested_convergence = in->suggested_convergence;
	out->suggested_strength = in->suggested_strength;
	out->suggest_flags = in->suggest_flags;
	out->relief_valid = in->relief_valid;
	out->relief_scale = in->relief_scale;
	out->relief_offset = in->relief_offset;
	out->temporal = in->temporal;
	out->same_inference = in->same_inference;
	out->has_confidence = in->has_confidence;
	out->source_time_ns = in->source_time_ns;
	out->provider_frame_id = in->provider_frame_id;
}

static inline void
xrt_conv_wire_unpack_depth_info(const struct xrt_conv_wire_depth_info *in, struct xrt_conv_depth_info *out)
{
	out->kind = in->kind;
	out->measure = in->measure;
	out->intrinsics_source = in->intrinsics_source;
	out->value_scale = in->value_scale;
	out->value_shift = in->value_shift;
	out->near_value = in->near_value;
	out->far_value = in->far_value;
	out->invalid_sample = in->invalid_sample;
	out->range_min = in->range_min;
	out->range_max = in->range_max;
	out->range_p05 = in->range_p05;
	out->range_p95 = in->range_p95;
	out->fx = in->fx;
	out->fy = in->fy;
	out->cx = in->cx;
	out->cy = in->cy;
	out->source_width = in->source_width;
	out->source_height = in->source_height;
	out->convergence_value = in->convergence_value;
	out->suggested_convergence = in->suggested_convergence;
	out->suggested_strength = in->suggested_strength;
	out->suggest_flags = in->suggest_flags;
	out->relief_valid = in->relief_valid;
	out->relief_scale = in->relief_scale;
	out->relief_offset = in->relief_offset;
	out->temporal = in->temporal;
	out->same_inference = in->same_inference;
	out->has_confidence = in->has_confidence;
	out->source_time_ns = in->source_time_ns;
	out->provider_frame_id = in->provider_frame_id;
}

static inline void
xrt_conv_wire_pack_views_info(const struct xrt_conv_views_info *in, struct xrt_conv_wire_views_info *out)
{
	uint32_t v;
	out->view_count = in->view_count;
	out->layout = in->layout;
	out->reserved_0 = in->reserved_0;
	for (v = 0; v < XRT_CONV_MAX_VIEWS; v++) {
		out->eye_xyz[v][0] = in->eye_xyz[v][0];
		out->eye_xyz[v][1] = in->eye_xyz[v][1];
		out->eye_xyz[v][2] = in->eye_xyz[v][2];
	}
	out->baseline_m = in->baseline_m;
	out->hfov_deg = in->hfov_deg;
	out->zero_parallax_disparity = in->zero_parallax_disparity;
	out->reserved_1 = in->reserved_1;
}

static inline void
xrt_conv_wire_unpack_views_info(const struct xrt_conv_wire_views_info *in, struct xrt_conv_views_info *out)
{
	uint32_t v;
	out->view_count = in->view_count;
	out->layout = in->layout;
	out->reserved_0 = in->reserved_0;
	for (v = 0; v < XRT_CONV_MAX_VIEWS; v++) {
		out->eye_xyz[v][0] = in->eye_xyz[v][0];
		out->eye_xyz[v][1] = in->eye_xyz[v][1];
		out->eye_xyz[v][2] = in->eye_xyz[v][2];
	}
	out->baseline_m = in->baseline_m;
	out->hfov_deg = in->hfov_deg;
	out->zero_parallax_disparity = in->zero_parallax_disparity;
	out->reserved_1 = in->reserved_1;
}

static inline void
xrt_conv_wire_pack_splat_info(const struct xrt_conv_splat_info *in, struct xrt_conv_wire_splat_info *out)
{
	out->blob_format = in->blob_format;
	out->coords = in->coords;
	out->color_space = in->color_space;
	out->metric = in->metric;
	out->intrinsics_source = in->intrinsics_source;
	out->fx = in->fx;
	out->fy = in->fy;
	out->cx = in->cx;
	out->cy = in->cy;
	out->grid_width = in->grid_width;
	out->grid_height = in->grid_height;
	out->baseline_m = in->baseline_m;
	out->focus_z_m = in->focus_z_m;
	out->near_m = in->near_m;
	out->far_m = in->far_m;
	out->reserved_0 = in->reserved_0;
	out->splat_count = in->splat_count;
}

static inline void
xrt_conv_wire_unpack_splat_info(const struct xrt_conv_wire_splat_info *in, struct xrt_conv_splat_info *out)
{
	out->blob_format = in->blob_format;
	out->coords = in->coords;
	out->color_space = in->color_space;
	out->metric = in->metric;
	out->intrinsics_source = in->intrinsics_source;
	out->fx = in->fx;
	out->fy = in->fy;
	out->cx = in->cx;
	out->cy = in->cy;
	out->grid_width = in->grid_width;
	out->grid_height = in->grid_height;
	out->baseline_m = in->baseline_m;
	out->focus_z_m = in->focus_z_m;
	out->near_m = in->near_m;
	out->far_m = in->far_m;
	out->reserved_0 = in->reserved_0;
	out->splat_count = in->splat_count;
}


/*
 *
 * Pack / unpack: image and params.
 *
 */

/*!
 * xrt_conv_image → wire. NOT carried (process-local, re-materialised by the
 * receiver): handle, import_key, cpu_ptr, sync.handle, sync.import_key,
 * reserved_0, sync.reserved_0. @p shm_offset is the CPU image's place in the
 * scratch shm (0 otherwise).
 */
static inline void
xrt_conv_wire_pack_image(const struct xrt_conv_image *in,
                         uint32_t role,
                         uint64_t shm_offset,
                         struct xrt_conv_wire_image *out)
{
	out->memory_kind = in->memory_kind;
	out->role = role;
	out->width = in->width;
	out->height = in->height;
	out->format = in->format;
	out->color_space = in->color_space;
	out->row_stride = in->row_stride;
	out->plane1_offset = in->plane1_offset;
	out->plane1_stride = in->plane1_stride;
	out->array_layer = in->array_layer;
	out->sync_kind = in->sync.kind;
	out->reserved_0 = 0;
	out->byte_size = in->byte_size;
	out->drm_modifier = in->drm_modifier;
	out->adapter_luid = in->adapter_luid;
	out->sync_acquire_value = in->sync.acquire_value;
	out->sync_release_value = in->sync.release_value;
	out->shm_offset = shm_offset;
}

/*!
 * wire → xrt_conv_image. Zeroes the process-local fields; the receiver sets
 * handle / import_key / cpu_ptr / sync.handle / sync.import_key itself.
 */
static inline void
xrt_conv_wire_unpack_image(const struct xrt_conv_wire_image *in,
                           struct xrt_conv_image *out,
                           uint32_t *out_role,
                           uint64_t *out_shm_offset)
{
	memset(out, 0, sizeof(*out));
	out->memory_kind = in->memory_kind;
	out->width = in->width;
	out->height = in->height;
	out->format = in->format;
	out->color_space = in->color_space;
	out->row_stride = in->row_stride;
	out->plane1_offset = in->plane1_offset;
	out->plane1_stride = in->plane1_stride;
	out->array_layer = in->array_layer;
	out->sync.kind = in->sync_kind;
	out->byte_size = in->byte_size;
	out->drm_modifier = in->drm_modifier;
	out->adapter_luid = in->adapter_luid;
	out->sync.acquire_value = in->sync_acquire_value;
	out->sync.release_value = in->sync_release_value;
	if (out_role != NULL) {
		*out_role = in->role;
	}
	if (out_shm_offset != NULL) {
		*out_shm_offset = in->shm_offset;
	}
}

//! xrt_conv_params → wire: every field except struct_size.
static inline void
xrt_conv_wire_pack_params(const struct xrt_conv_params *in, struct xrt_conv_wire_params *out)
{
	uint32_t v;
	out->frame_flags = in->frame_flags;
	out->reserved_0 = 0;
	out->source_time_ns = in->source_time_ns;
	out->frame_id = in->frame_id;
	out->convergence_mode = in->convergence_mode;
	out->convergence = in->convergence;
	out->strength = in->strength;
	out->view_count = in->view_count;
	out->content_hint = in->content_hint;
	out->input_scale = in->input_scale;
	out->focal_px = in->focal_px;
	out->viewpoint_frame = in->viewpoint_frame;
	out->viewpoint_source = in->viewpoint_source;
	out->axis_mode = in->axis_mode;
	out->viewpoint_count = in->viewpoint_count;
	for (v = 0; v < XRT_CONV_MAX_VIEWS; v++) {
		out->viewpoints_xyz[v][0] = in->viewpoints_xyz[v][0];
		out->viewpoints_xyz[v][1] = in->viewpoints_xyz[v][1];
		out->viewpoints_xyz[v][2] = in->viewpoints_xyz[v][2];
	}
	out->rect_width_m = in->rect_width_m;
	out->rect_height_m = in->rect_height_m;
	out->baseline_m = in->baseline_m;
	out->max_offset_m = in->max_offset_m;
	out->nominal_z_m = in->nominal_z_m;
	xrt_conv_wire_pack_stereo_calibration(&in->stereo, &out->stereo);
}

//! wire → xrt_conv_params; sets struct_size = sizeof(struct xrt_conv_params).
static inline void
xrt_conv_wire_unpack_params(const struct xrt_conv_wire_params *in, struct xrt_conv_params *out)
{
	uint32_t v;
	memset(out, 0, sizeof(*out));
	out->struct_size = (uint32_t)sizeof(*out);
	out->frame_flags = in->frame_flags;
	out->source_time_ns = in->source_time_ns;
	out->frame_id = in->frame_id;
	out->convergence_mode = in->convergence_mode;
	out->convergence = in->convergence;
	out->strength = in->strength;
	out->view_count = in->view_count;
	out->content_hint = in->content_hint;
	out->input_scale = in->input_scale;
	out->focal_px = in->focal_px;
	out->viewpoint_frame = in->viewpoint_frame;
	out->viewpoint_source = in->viewpoint_source;
	out->axis_mode = in->axis_mode;
	out->viewpoint_count = in->viewpoint_count;
	for (v = 0; v < XRT_CONV_MAX_VIEWS; v++) {
		out->viewpoints_xyz[v][0] = in->viewpoints_xyz[v][0];
		out->viewpoints_xyz[v][1] = in->viewpoints_xyz[v][1];
		out->viewpoints_xyz[v][2] = in->viewpoints_xyz[v][2];
	}
	out->rect_width_m = in->rect_width_m;
	out->rect_height_m = in->rect_height_m;
	out->baseline_m = in->baseline_m;
	out->max_offset_m = in->max_offset_m;
	out->nominal_z_m = in->nominal_z_m;
	xrt_conv_wire_unpack_stereo_calibration(&in->stereo, &out->stereo);
}


/*
 *
 * Pack / unpack: messages with an ABI counterpart.
 *
 */

/*!
 * Capability + readiness → conv_enum_task reply. NOT carried (identity, kept
 * in the service): cap->struct_size, cap_id, licence_spdx, model_id,
 * model_version, backend, display_name, reserved_0; state->struct_size,
 * reserved_0, version, detail.
 */
static inline void
xrt_conv_wire_pack_task_desc(const struct xrt_conv_capability *cap,
                             const struct xrt_conv_state *st,
                             uint32_t desc_flags,
                             struct xrt_conv_wire_task_desc *out)
{
	xrt_conv_wire_begin(out, (uint32_t)sizeof(*out));
	out->task = cap->task;
	out->state = st->state;
	out->failure_reason = st->failure_reason;
	out->retryable = st->retryable;
	out->progress = st->progress;
	out->desc_flags = desc_flags;
	out->eta_ns = st->eta_ns;
	out->lanes = cap->lanes;
	out->input_kinds = cap->input_kinds;
	out->outputs = cap->outputs;
	out->flags = cap->flags;
	out->import_memory_kinds = cap->import_memory_kinds;
	out->import_sync_kinds = cap->import_sync_kinds;
	out->input_formats = cap->input_formats;
	out->depth_output_format = cap->depth_output_format;
	out->min_views = cap->min_views;
	out->max_views = cap->max_views;
	out->max_streams = cap->max_streams;
	out->max_concurrent = cap->max_concurrent;
	out->max_input_edge = cap->max_input_edge;
	out->preferred_input_edge = cap->preferred_input_edge;
	out->depth_kind = cap->depth_kind;
	out->depth_measure = cap->depth_measure;
	out->intrinsics_source = cap->intrinsics_source;
	out->splat_formats = cap->splat_formats;
	out->licence_class = cap->licence_class;
	out->commercial_ok = cap->commercial_ok;
	out->est_warmup_ms = cap->est_warmup_ms;
	out->est_latency_ms = cap->est_latency_ms;
	out->est_vram_mb = cap->est_vram_mb;
	out->est_ram_mb = cap->est_ram_mb;
	out->device_luid = cap->device_luid;
}

/*!
 * conv_enum_task reply → capability + readiness. Zeroes both, sets their
 * struct_size to their sizeof; identity fields stay zero.
 */
static inline void
xrt_conv_wire_unpack_task_desc(const struct xrt_conv_wire_task_desc *in,
                               struct xrt_conv_capability *cap,
                               struct xrt_conv_state *st,
                               uint32_t *out_desc_flags)
{
	memset(cap, 0, sizeof(*cap));
	memset(st, 0, sizeof(*st));
	cap->struct_size = (uint32_t)sizeof(*cap);
	st->struct_size = (uint32_t)sizeof(*st);
	cap->task = in->task;
	st->state = in->state;
	st->failure_reason = in->failure_reason;
	st->retryable = in->retryable;
	st->progress = in->progress;
	st->eta_ns = in->eta_ns;
	cap->lanes = in->lanes;
	cap->input_kinds = in->input_kinds;
	cap->outputs = in->outputs;
	cap->flags = in->flags;
	cap->import_memory_kinds = in->import_memory_kinds;
	cap->import_sync_kinds = in->import_sync_kinds;
	cap->input_formats = in->input_formats;
	cap->depth_output_format = in->depth_output_format;
	cap->min_views = in->min_views;
	cap->max_views = in->max_views;
	cap->max_streams = in->max_streams;
	cap->max_concurrent = in->max_concurrent;
	cap->max_input_edge = in->max_input_edge;
	cap->preferred_input_edge = in->preferred_input_edge;
	cap->depth_kind = in->depth_kind;
	cap->depth_measure = in->depth_measure;
	cap->intrinsics_source = in->intrinsics_source;
	cap->splat_formats = in->splat_formats;
	cap->licence_class = in->licence_class;
	cap->commercial_ok = in->commercial_ok;
	cap->est_warmup_ms = in->est_warmup_ms;
	cap->est_latency_ms = in->est_latency_ms;
	cap->est_vram_mb = in->est_vram_mb;
	cap->est_ram_mb = in->est_ram_mb;
	cap->device_luid = in->device_luid;
	if (out_desc_flags != NULL) {
		*out_desc_flags = in->desc_flags;
	}
}

//! Stream info + selection inputs → conv_stream_create request (every field but struct_size).
static inline void
xrt_conv_wire_pack_stream_desc(uint32_t task,
                               uint32_t quality_hint,
                               uint32_t export_memory_kind,
                               const struct xrt_conv_stream_info *info,
                               struct xrt_conv_wire_stream_desc *out)
{
	xrt_conv_wire_begin(out, (uint32_t)sizeof(*out));
	out->task = task;
	out->quality_hint = quality_hint;
	out->export_memory_kind = export_memory_kind;
	out->input_kind = info->input_kind;
	out->outputs = info->outputs;
	out->content_hint = info->content_hint;
	out->view_count = info->view_count;
	out->views_layout = info->views_layout;
	out->max_input_width = info->max_input_width;
	out->max_input_height = info->max_input_height;
}

//! conv_stream_create request → stream info (struct_size = its sizeof) + selection inputs.
static inline void
xrt_conv_wire_unpack_stream_desc(const struct xrt_conv_wire_stream_desc *in,
                                 uint32_t *out_task,
                                 uint32_t *out_quality_hint,
                                 uint32_t *out_export_memory_kind,
                                 struct xrt_conv_stream_info *info)
{
	memset(info, 0, sizeof(*info));
	info->struct_size = (uint32_t)sizeof(*info);
	info->input_kind = in->input_kind;
	info->outputs = in->outputs;
	info->content_hint = in->content_hint;
	info->view_count = in->view_count;
	info->views_layout = in->views_layout;
	info->max_input_width = in->max_input_width;
	info->max_input_height = in->max_input_height;
	*out_task = in->task;
	*out_quality_hint = in->quality_hint;
	*out_export_memory_kind = in->export_memory_kind;
}

/*!
 * Frame inputs + params → conv_submit request. At most
 * XRT_CONV_WIRE_MAX_SUBMIT_IMAGES images; @p shm_offsets may be NULL (no CPU
 * images). Returns false if frame_in holds more images than the wire takes.
 */
static inline bool
xrt_conv_wire_pack_submit(uint64_t stream_id,
                          const struct xrt_conv_frame_in *in,
                          const uint64_t *shm_offsets,
                          const struct xrt_conv_params *params,
                          struct xrt_conv_wire_submit *out)
{
	uint32_t i;
	xrt_conv_wire_begin(out, (uint32_t)sizeof(*out));
	if (in->image_count > XRT_CONV_WIRE_MAX_SUBMIT_IMAGES) {
		return false;
	}
	out->stream_id = stream_id;
	out->image_count = in->image_count;
	for (i = 0; i < in->image_count; i++) {
		xrt_conv_wire_pack_image(&in->images[i], in->image_role[i], shm_offsets != NULL ? shm_offsets[i] : 0,
		                         &out->images[i]);
	}
	xrt_conv_wire_pack_params(params, &out->params);
	xrt_conv_wire_pack_depth_info(&in->depth_in_info, &out->depth_in_info);
	return true;
}

/*!
 * conv_submit request → frame inputs (struct_size = its sizeof; images'
 * process-local fields zero) + params. @p out_shm_offsets may be NULL.
 * Returns false if image_count exceeds the wire's array.
 */
static inline bool
xrt_conv_wire_unpack_submit(const struct xrt_conv_wire_submit *in,
                            uint64_t *out_stream_id,
                            struct xrt_conv_frame_in *frame,
                            uint64_t *out_shm_offsets,
                            struct xrt_conv_params *params)
{
	uint32_t i;
	memset(frame, 0, sizeof(*frame));
	frame->struct_size = (uint32_t)sizeof(*frame);
	if (in->image_count > XRT_CONV_WIRE_MAX_SUBMIT_IMAGES) {
		return false;
	}
	*out_stream_id = in->stream_id;
	frame->image_count = in->image_count;
	for (i = 0; i < in->image_count; i++) {
		xrt_conv_wire_unpack_image(&in->images[i], &frame->images[i], &frame->image_role[i],
		                           out_shm_offsets != NULL ? &out_shm_offsets[i] : NULL);
	}
	xrt_conv_wire_unpack_params(&in->params, params);
	xrt_conv_wire_unpack_depth_info(&in->depth_in_info, &frame->depth_in_info);
	return true;
}

/*!
 * A finished frame → conv_acquire_result reply. Carries written, each
 * target's width / height / format, views_info and depth_info; NOT the
 * targets' process-local fields (handles travel via get-output calls).
 */
static inline void
xrt_conv_wire_pack_result(uint64_t stream_id,
                          uint64_t frame_id,
                          uint64_t source_time_ns,
                          uint64_t latency_ns,
                          uint32_t output_realloc,
                          const struct xrt_conv_frame_out *fo,
                          struct xrt_conv_wire_result *out)
{
	xrt_conv_wire_begin(out, (uint32_t)sizeof(*out));
	out->stream_id = stream_id;
	out->frame_id = frame_id;
	out->source_time_ns = source_time_ns;
	out->latency_ns = latency_ns;
	out->written = fo->written;
	out->output_realloc = output_realloc;
	out->views_width = fo->views.width;
	out->views_height = fo->views.height;
	out->views_format = fo->views.format;
	out->depth_width = fo->depth.width;
	out->depth_height = fo->depth.height;
	out->depth_format = fo->depth.format;
	out->confidence_width = fo->confidence.width;
	out->confidence_height = fo->confidence.height;
	out->confidence_format = fo->confidence.format;
	out->disparity_width = fo->disparity.width;
	out->disparity_height = fo->disparity.height;
	out->disparity_format = fo->disparity.format;
	xrt_conv_wire_pack_views_info(&fo->views_info, &out->views_info);
	xrt_conv_wire_pack_depth_info(&fo->depth_info, &out->depth_info);
}

/*!
 * conv_acquire_result reply → frame out (zeroed, struct_size = its sizeof,
 * each target carrying only width / height / format) + the frame metadata.
 * Any out pointer for metadata may be NULL.
 */
static inline void
xrt_conv_wire_unpack_result(const struct xrt_conv_wire_result *in,
                            uint64_t *out_stream_id,
                            uint64_t *out_frame_id,
                            uint64_t *out_source_time_ns,
                            uint64_t *out_latency_ns,
                            uint32_t *out_output_realloc,
                            struct xrt_conv_frame_out *fo)
{
	memset(fo, 0, sizeof(*fo));
	fo->struct_size = (uint32_t)sizeof(*fo);
	fo->written = in->written;
	fo->views.width = in->views_width;
	fo->views.height = in->views_height;
	fo->views.format = in->views_format;
	fo->depth.width = in->depth_width;
	fo->depth.height = in->depth_height;
	fo->depth.format = in->depth_format;
	fo->confidence.width = in->confidence_width;
	fo->confidence.height = in->confidence_height;
	fo->confidence.format = in->confidence_format;
	fo->disparity.width = in->disparity_width;
	fo->disparity.height = in->disparity_height;
	fo->disparity.format = in->disparity_format;
	xrt_conv_wire_unpack_views_info(&in->views_info, &fo->views_info);
	xrt_conv_wire_unpack_depth_info(&in->depth_info, &fo->depth_info);
	if (out_stream_id != NULL) {
		*out_stream_id = in->stream_id;
	}
	if (out_frame_id != NULL) {
		*out_frame_id = in->frame_id;
	}
	if (out_source_time_ns != NULL) {
		*out_source_time_ns = in->source_time_ns;
	}
	if (out_latency_ns != NULL) {
		*out_latency_ns = in->latency_ns;
	}
	if (out_output_realloc != NULL) {
		*out_output_realloc = in->output_realloc;
	}
}

/*!
 * Job inputs + params → conv_job_submit request. NOT carried: the reserved
 * encoded fields (encoded_count, encoded, encoded_size) and reserved_0.
 * @p shm_offsets may be NULL. Returns false on image_count > the array.
 */
static inline bool
xrt_conv_wire_pack_job_desc(uint32_t task,
                            uint32_t quality_hint,
                            const struct xrt_conv_job_in *in,
                            const uint64_t *shm_offsets,
                            const struct xrt_conv_params *params,
                            struct xrt_conv_wire_job_desc *out)
{
	uint32_t i;
	xrt_conv_wire_begin(out, (uint32_t)sizeof(*out));
	if (in->image_count > XRT_CONV_MAX_FRAME_IMAGES) {
		return false;
	}
	out->task = task;
	out->quality_hint = quality_hint;
	out->input_kind = in->input_kind;
	out->image_count = in->image_count;
	out->requested_blob_format = in->requested_blob_format;
	for (i = 0; i < in->image_count; i++) {
		xrt_conv_wire_pack_image(&in->images[i], in->image_role[i], shm_offsets != NULL ? shm_offsets[i] : 0,
		                         &out->images[i]);
	}
	xrt_conv_wire_pack_params(params, &out->params);
	return true;
}

//! conv_job_submit request → job inputs (struct_size = its sizeof) + params.
static inline bool
xrt_conv_wire_unpack_job_desc(const struct xrt_conv_wire_job_desc *in,
                              uint32_t *out_task,
                              uint32_t *out_quality_hint,
                              struct xrt_conv_job_in *job,
                              uint64_t *out_shm_offsets,
                              struct xrt_conv_params *params)
{
	uint32_t i;
	memset(job, 0, sizeof(*job));
	job->struct_size = (uint32_t)sizeof(*job);
	if (in->image_count > XRT_CONV_MAX_FRAME_IMAGES) {
		return false;
	}
	*out_task = in->task;
	*out_quality_hint = in->quality_hint;
	job->input_kind = in->input_kind;
	job->image_count = in->image_count;
	job->requested_blob_format = in->requested_blob_format;
	for (i = 0; i < in->image_count; i++) {
		xrt_conv_wire_unpack_image(&in->images[i], &job->images[i], &job->image_role[i],
		                           out_shm_offsets != NULL ? &out_shm_offsets[i] : NULL);
	}
	xrt_conv_wire_unpack_params(&in->params, params);
	return true;
}

//! A finished job → conv_job_fetch reply.
static inline void
xrt_conv_wire_pack_job_result(uint64_t job_id,
                              uint32_t blob_format,
                              uint32_t delivery,
                              uint64_t byte_size,
                              const struct xrt_conv_job_out *jo,
                              struct xrt_conv_wire_job_result *out)
{
	xrt_conv_wire_begin(out, (uint32_t)sizeof(*out));
	out->job_id = job_id;
	out->written = jo->written;
	out->blob_format = blob_format;
	out->delivery = delivery;
	out->byte_size = byte_size;
	xrt_conv_wire_pack_splat_info(&jo->splat_info, &out->splat_info);
	xrt_conv_wire_pack_depth_info(&jo->depth_info, &out->depth_info);
	xrt_conv_wire_pack_views_info(&jo->views_info, &out->views_info);
}

//! conv_job_fetch reply → job out (zeroed, struct_size = its sizeof) + blob metadata (out pointers may be NULL).
static inline void
xrt_conv_wire_unpack_job_result(const struct xrt_conv_wire_job_result *in,
                                uint64_t *out_job_id,
                                uint32_t *out_blob_format,
                                uint32_t *out_delivery,
                                uint64_t *out_byte_size,
                                struct xrt_conv_job_out *jo)
{
	memset(jo, 0, sizeof(*jo));
	jo->struct_size = (uint32_t)sizeof(*jo);
	jo->written = in->written;
	xrt_conv_wire_unpack_splat_info(&in->splat_info, &jo->splat_info);
	xrt_conv_wire_unpack_depth_info(&in->depth_info, &jo->depth_info);
	xrt_conv_wire_unpack_views_info(&in->views_info, &jo->views_info);
	if (out_job_id != NULL) {
		*out_job_id = in->job_id;
	}
	if (out_blob_format != NULL) {
		*out_blob_format = in->blob_format;
	}
	if (out_delivery != NULL) {
		*out_delivery = in->delivery;
	}
	if (out_byte_size != NULL) {
		*out_byte_size = in->byte_size;
	}
}


/*
 *
 * Tripwires.
 *
 */
// clang-format off
#define XRT_CONV_WIRE_TWIN(W, A, F)                                                                \
	XRT_CONV_ABI_ASSERT(offsetof(struct W, F) == offsetof(struct A, F) &&                         \
	                    sizeof(((struct W *)0)->F) == sizeof(((struct A *)0)->F),                 \
	                    #W "." #F " must match " #A "." #F)

/* Twins: byte-identical to the frozen ABI structs, field by field. */
XRT_CONV_ABI_ASSERT(sizeof(struct xrt_conv_wire_stereo_calibration) == sizeof(struct xrt_conv_stereo_calibration), "stereo twin size");
XRT_CONV_WIRE_TWIN(xrt_conv_wire_stereo_calibration, xrt_conv_stereo_calibration, eye_order);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_stereo_calibration, xrt_conv_stereo_calibration, fx);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_stereo_calibration, xrt_conv_stereo_calibration, fy);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_stereo_calibration, xrt_conv_stereo_calibration, cx);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_stereo_calibration, xrt_conv_stereo_calibration, cy);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_stereo_calibration, xrt_conv_stereo_calibration, baseline_m);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_stereo_calibration, xrt_conv_stereo_calibration, infinity_shift_px);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_stereo_calibration, xrt_conv_stereo_calibration, sbs_packed);

XRT_CONV_ABI_ASSERT(sizeof(struct xrt_conv_wire_depth_info) == sizeof(struct xrt_conv_depth_info), "depth twin size");
XRT_CONV_WIRE_TWIN(xrt_conv_wire_depth_info, xrt_conv_depth_info, kind);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_depth_info, xrt_conv_depth_info, measure);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_depth_info, xrt_conv_depth_info, intrinsics_source);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_depth_info, xrt_conv_depth_info, value_scale);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_depth_info, xrt_conv_depth_info, value_shift);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_depth_info, xrt_conv_depth_info, near_value);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_depth_info, xrt_conv_depth_info, far_value);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_depth_info, xrt_conv_depth_info, invalid_sample);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_depth_info, xrt_conv_depth_info, range_min);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_depth_info, xrt_conv_depth_info, range_max);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_depth_info, xrt_conv_depth_info, range_p05);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_depth_info, xrt_conv_depth_info, range_p95);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_depth_info, xrt_conv_depth_info, fx);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_depth_info, xrt_conv_depth_info, fy);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_depth_info, xrt_conv_depth_info, cx);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_depth_info, xrt_conv_depth_info, cy);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_depth_info, xrt_conv_depth_info, source_width);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_depth_info, xrt_conv_depth_info, source_height);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_depth_info, xrt_conv_depth_info, convergence_value);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_depth_info, xrt_conv_depth_info, suggested_convergence);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_depth_info, xrt_conv_depth_info, suggested_strength);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_depth_info, xrt_conv_depth_info, suggest_flags);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_depth_info, xrt_conv_depth_info, relief_valid);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_depth_info, xrt_conv_depth_info, relief_scale);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_depth_info, xrt_conv_depth_info, relief_offset);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_depth_info, xrt_conv_depth_info, temporal);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_depth_info, xrt_conv_depth_info, same_inference);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_depth_info, xrt_conv_depth_info, has_confidence);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_depth_info, xrt_conv_depth_info, source_time_ns);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_depth_info, xrt_conv_depth_info, provider_frame_id);

XRT_CONV_ABI_ASSERT(sizeof(struct xrt_conv_wire_views_info) == sizeof(struct xrt_conv_views_info), "views twin size");
XRT_CONV_WIRE_TWIN(xrt_conv_wire_views_info, xrt_conv_views_info, view_count);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_views_info, xrt_conv_views_info, layout);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_views_info, xrt_conv_views_info, reserved_0);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_views_info, xrt_conv_views_info, eye_xyz);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_views_info, xrt_conv_views_info, baseline_m);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_views_info, xrt_conv_views_info, hfov_deg);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_views_info, xrt_conv_views_info, zero_parallax_disparity);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_views_info, xrt_conv_views_info, reserved_1);

XRT_CONV_ABI_ASSERT(sizeof(struct xrt_conv_wire_splat_info) == sizeof(struct xrt_conv_splat_info), "splat twin size");
XRT_CONV_WIRE_TWIN(xrt_conv_wire_splat_info, xrt_conv_splat_info, blob_format);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_splat_info, xrt_conv_splat_info, coords);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_splat_info, xrt_conv_splat_info, color_space);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_splat_info, xrt_conv_splat_info, metric);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_splat_info, xrt_conv_splat_info, intrinsics_source);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_splat_info, xrt_conv_splat_info, fx);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_splat_info, xrt_conv_splat_info, fy);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_splat_info, xrt_conv_splat_info, cx);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_splat_info, xrt_conv_splat_info, cy);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_splat_info, xrt_conv_splat_info, grid_width);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_splat_info, xrt_conv_splat_info, grid_height);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_splat_info, xrt_conv_splat_info, baseline_m);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_splat_info, xrt_conv_splat_info, focus_z_m);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_splat_info, xrt_conv_splat_info, near_m);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_splat_info, xrt_conv_splat_info, far_m);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_splat_info, xrt_conv_splat_info, reserved_0);
XRT_CONV_WIRE_TWIN(xrt_conv_wire_splat_info, xrt_conv_splat_info, splat_count);

/* Exact sizes: no hidden padding (every gap is a reserved_* field). Changing
 * one is a family_version bump unless it is an append at a message's tail. */
XRT_CONV_ABI_ASSERT(sizeof(struct xrt_conv_wire_image) == 96, "xrt_conv_wire_image is FROZEN (12 x 4 + 6 x 8)");
XRT_CONV_ABI_ASSERT(sizeof(struct xrt_conv_wire_params) == 216, "xrt_conv_wire_params is FROZEN");
XRT_CONV_ABI_ASSERT(offsetof(struct xrt_conv_wire_params, stereo) == 184, "xrt_conv_wire_params: stereo at 184");
XRT_CONV_ABI_ASSERT(sizeof(struct xrt_conv_wire_hint_interest) == 16, "conv_hint_interest size");
XRT_CONV_ABI_ASSERT(sizeof(struct xrt_conv_wire_generation) == 24, "conv_get_generation size");
XRT_CONV_ABI_ASSERT(sizeof(struct xrt_conv_wire_task_desc) == 144, "conv_enum_task size");
XRT_CONV_ABI_ASSERT(sizeof(struct xrt_conv_wire_stream_desc) == 56, "conv_stream_create request size");
XRT_CONV_ABI_ASSERT(sizeof(struct xrt_conv_wire_stream_created) == 40, "conv_stream_create reply size");
XRT_CONV_ABI_ASSERT(sizeof(struct xrt_conv_wire_submit) == 560, "conv_submit size");
XRT_CONV_ABI_ASSERT(offsetof(struct xrt_conv_wire_submit, depth_in_info) == 432, "conv_submit: depth_in_info at 432");
XRT_CONV_ABI_ASSERT(sizeof(struct xrt_conv_wire_result) == 352, "conv_acquire_result size");
XRT_CONV_ABI_ASSERT(offsetof(struct xrt_conv_wire_result, depth_info) == 224, "conv_acquire_result: depth_info at 224");
XRT_CONV_ABI_ASSERT(sizeof(struct xrt_conv_wire_job_desc) == 632, "conv_job_submit size");
XRT_CONV_ABI_ASSERT(sizeof(struct xrt_conv_wire_job_status) == 64, "conv_job_status size");
XRT_CONV_ABI_ASSERT(sizeof(struct xrt_conv_wire_job_result) == 368, "conv_job_fetch size");
XRT_CONV_ABI_ASSERT(offsetof(struct xrt_conv_wire_job_result, views_info) == 240, "conv_job_fetch: views_info at 240");
XRT_CONV_ABI_ASSERT(sizeof(struct xrt_conv_wire_task_request) == 16, "conv_warm / conv_evict size");
XRT_CONV_ABI_ASSERT(sizeof(struct xrt_conv_wire_event) == 40, "conv event size");

/* Rule 3: every message fits one IPC buffer. */
XRT_CONV_ABI_ASSERT(sizeof(struct xrt_conv_wire_submit) <= XRT_CONV_WIRE_MSG_MAX_BYTES, "conv_submit > IPC_BUF_SIZE");
XRT_CONV_ABI_ASSERT(sizeof(struct xrt_conv_wire_job_desc) <= XRT_CONV_WIRE_MSG_MAX_BYTES, "conv_job_submit > IPC_BUF_SIZE");
XRT_CONV_ABI_ASSERT(sizeof(struct xrt_conv_wire_result) <= XRT_CONV_WIRE_MSG_MAX_BYTES, "conv_acquire_result > IPC_BUF_SIZE");
XRT_CONV_ABI_ASSERT(sizeof(struct xrt_conv_wire_job_result) <= XRT_CONV_WIRE_MSG_MAX_BYTES, "conv_job_fetch > IPC_BUF_SIZE");
XRT_CONV_ABI_ASSERT(sizeof(struct xrt_conv_wire_task_desc) <= XRT_CONV_WIRE_MSG_MAX_BYTES, "conv_enum_task > IPC_BUF_SIZE");
// clang-format on

#ifdef __cplusplus
}
#endif
