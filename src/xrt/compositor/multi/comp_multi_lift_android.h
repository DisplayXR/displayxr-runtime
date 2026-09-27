// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_lift (ADR-042) on the Android service: the lift thread, the
 *         vendor module's lift-only Vulkan display processor, and every stream.
 *         The Android twin of d3d11_service/d3d11_lift.h.
 * @ingroup comp_multi
 *
 * ## Shape
 *
 * One @ref comp_multi_lift per service PROCESS (hung off the
 * multi_system_compositor, created lazily on the first lift call). It owns:
 *
 *  - the LIFT THREAD — attached to the JavaVM (a vendor module behind the slots
 *    may call JNI), the only thread that touches the lift display processor;
 *  - the lift display processor, from the plug-in's LIFT-ONLY Vulkan factory
 *    (xrt_plugin_iface::create_dp_vk_lift — or another bundled plug-in's, via
 *    `debug.dxr.lift.plugin`). Its creation is marshalled onto the service
 *    MAIN thread, exactly like the weave engine's DP (#510 M2);
 *  - per stream: a latest-wins MAILBOX (two input slots) and an output RING
 *    (two slots), every slot an AHardwareBuffer-backed VkImage on the service
 *    device, driven by the platform-neutral state machine in
 *    util/u_lift_mailbox.h, unchanged from Windows;
 *  - per stream, for xrAcquireLiftResultDXR: an AHardwareBuffer export image
 *    handed to the caller once per (re)allocation (the weave output pattern).
 *
 * ## Queue and sync (v1)
 *
 * The service device exposes ONE graphics queue (vk_bundle::main_queue); a
 * dedicated lift queue would need a device-creation change, so every lift
 * submission (the lift thread's ring copies, an IPC thread's snapshot /
 * export copy) goes to the main queue under vk_queue_lock and is CPU-waited
 * before its slot changes hands. That is the whole cross-thread GPU sync:
 * a slot is only ever handed on (commit / publish / unpin) after its writer's
 * fence signalled. The weave path records its snapshot and its result blits
 * into the weave's own command buffer, which the weave already CPU-waits.
 *
 * ## Threads and locks
 *
 *  - `mtx` (inside comp_multi_lift) guards the stream table and every mailbox.
 *    Leaf lock: never held across a GPU wait, a queue lock, or a module call.
 *  - Callers may hold `mc->weave.mutex` (the weave path) when calling in; the
 *    lift thread never takes a weave lock. Order: weave.mutex → mtx.
 *  - Producers never wait for the module; consumers pin a ring slot for the
 *    length of one CPU-waited GPU copy.
 */

#pragma once

#include "xrt/xrt_config_os.h"

#ifdef XRT_OS_ANDROID

#include "xrt/xrt_results.h"
#include "xrt/xrt_dp_lift.h"
#include "xrt/xrt_lift.h"
#include "xrt/xrt_display_metrics.h"
#include "xrt/xrt_handles.h"

#include "vk/vk_helpers.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct comp_multi_lift;
struct xrt_system_compositor;
struct xrt_compositor;

/*!
 * The process's lift module, created on first use (never blocks: the lift
 * DP comes up on the lift thread). NULL when @p xsysc has no Vulkan device.
 */
struct comp_multi_lift *
comp_multi_lift_for_system(struct xrt_system_compositor *xsysc);

//! The module if it already exists (never creates it) — for release paths.
struct comp_multi_lift *
comp_multi_lift_peek_system(struct xrt_system_compositor *xsysc);

//! Stop the lift thread, destroy every stream and the lift DP. Before the device goes.
void
comp_multi_lift_destroy(struct comp_multi_lift **lift_ptr);

//! Current caps (cached; never blocks). Kicks activation on first call.
void
comp_multi_lift_get_caps(struct comp_multi_lift *lift, struct xrt_dp_lift_caps *out);

xrt_result_t
comp_multi_lift_stream_create(struct comp_multi_lift *lift,
                              uint64_t owner,
                              const struct xrt_dp_lift_stream_info *info,
                              uint64_t *out_id);

void
comp_multi_lift_stream_destroy(struct comp_multi_lift *lift, uint64_t owner, uint64_t id);

//! Destroy every stream @p owner created (client teardown).
void
comp_multi_lift_release_owner(struct comp_multi_lift *lift, uint64_t owner);

//! The stream's mode bit, 0 when @p id is not @p owner's live stream.
uint32_t
comp_multi_lift_stream_mode(struct comp_multi_lift *lift, uint64_t owner, uint64_t id);

/*!
 * xrSubmitLiftFrameDXR: snapshot the top-left @p w x @p h of the caller's
 * AHardwareBuffer into the stream's mailbox (one CPU-waited blit — never the
 * model). Takes ownership of @p ahb (the IPC receive's reference): it is either
 * adopted into the per-stream import cache or released. The caller's GPU
 * writes must be complete before the call (the weave's input contract; no
 * acquire fence on Android yet). Never capped (the frame size is the app's).
 */
xrt_result_t
comp_multi_lift_submit_ahb(struct comp_multi_lift *lift,
                           uint64_t owner,
                           uint64_t id,
                           xrt_graphics_buffer_handle_t ahb,
                           uint32_t w,
                           uint32_t h,
                           int64_t source_time,
                           const struct xrt_dp_lift_params *params,
                           const float *viewpoints,
                           uint32_t viewpoint_floats,
                           uint64_t *out_frame_id);

xrt_result_t
comp_multi_lift_acquire_result(
    struct comp_multi_lift *lift, uint64_t owner, uint64_t id, bool *out_ready, struct xrt_lift_result *out);

/*!
 * The stream's export AHardwareBuffer, as a BORROWED pointer: the generated
 * IPC send only reads it and the receiver gets its own reference (the weave
 * export rule — taking one here would strand the buffer on every realloc).
 */
bool
comp_multi_lift_export_output(struct comp_multi_lift *lift,
                              uint64_t owner,
                              uint64_t id,
                              xrt_graphics_buffer_handle_t *out_handle,
                              uint32_t *out_w,
                              uint32_t *out_h,
                              uint32_t *out_format);

//! xrAcquireLiftBlobDXR with the two-call latch; see d3d11_lift_acquire_blob.
xrt_result_t
comp_multi_lift_acquire_blob(struct comp_multi_lift *lift,
                             uint64_t owner,
                             uint64_t id,
                             uint64_t capacity,
                             bool *out_ready,
                             struct xrt_lift_blob_info *out_info,
                             uint8_t **out_bytes);

xrt_result_t
comp_multi_lift_set_priority(struct comp_multi_lift *lift, uint64_t owner, uint64_t id, uint32_t priority);

xrt_result_t
comp_multi_lift_get_stats(struct comp_multi_lift *lift, uint64_t owner, uint64_t id, struct xrt_lift_stream_stats *out);

/*!
 * The latest predicted eyes of the panel DP, fed by every weave submit — the
 * TRACKED viewpoints the lift thread hands the module (the lift DP has no
 * tracker session of its own). Cheap; any thread.
 */
void
comp_multi_lift_note_eyes(struct comp_multi_lift *lift, const struct xrt_eye_positions *eyes);


/*
 *
 * The weave path (comp_multi_weave_android.c), called with mc->weave.mutex held
 * while the weave's command buffer is being recorded.
 *
 */

/*!
 * Latch the lift-flagged rects of this client's NEXT weave submit (the
 * lift_weave_rects IPC call, immediately before its weave_submit). Only
 * @p owner's live SBS / NVIEW streams are accepted; false = refused (the submit
 * then weaves those rects as drawn). Creates the module outside every weave
 * lock if needed.
 */
bool
comp_multi_lift_set_weave_rects(struct xrt_compositor *xc,
                                uint64_t owner,
                                uint32_t count,
                                const struct xrt_lift_weave_rect *rects);

//! One in-flight weave-rect snapshot (begin at record time, complete after the fence).
struct comp_multi_lift_ticket
{
	struct comp_multi_lift *lift;
	uint64_t stream_id;
	int32_t slot;
	uint32_t w, h;
	bool active;
};

/*!
 * Record, into @p cmd, the snapshot of region (@p x, @p y, @p w, @p h) of
 * @p src (in @p src_layout, which must allow TRANSFER reads: GENERAL or
 * TRANSFER_SRC_OPTIMAL) into the stream's mailbox, downsampled to the
 * `DXR_LIFT_MAX_INPUT_EDGE` / `debug.dxr.lift.max_input_edge` cap (default
 * 1920; u_lift_cap_dims). False = not taken (module not up yet, stream gone,
 * mailbox busy): nothing was recorded. On true the caller MUST call
 * @ref comp_multi_lift_weave_snapshot_done once the command buffer retired.
 */
bool
comp_multi_lift_weave_snapshot(struct comp_multi_lift *lift,
                               uint64_t owner,
                               uint64_t id,
                               VkCommandBuffer cmd,
                               VkImage src,
                               VkImageLayout src_layout,
                               int32_t x,
                               int32_t y,
                               uint32_t w,
                               uint32_t h,
                               const struct xrt_dp_lift_params *params,
                               struct comp_multi_lift_ticket *out_ticket);

//! Commit (@p gpu_ok) or abort the snapshot of @p ticket. Idempotent.
void
comp_multi_lift_weave_snapshot_done(struct comp_multi_lift_ticket *ticket, bool gpu_ok);

//! A pinned result, readable (VK_IMAGE_LAYOUT_GENERAL) on the service device.
struct comp_multi_lift_pin
{
	struct comp_multi_lift *lift;
	uint64_t stream_id;
	int32_t slot;
	VkImage image;
	uint32_t width; //!< whole result (all views)
	uint32_t height;
	uint32_t view_count;
	bool valid;
};

/*!
 * Pin the stream's latest texture result for the weave. The ring slot is not
 * overwritten until @ref comp_multi_lift_unpin, which the caller issues after
 * its command buffer retired. Before recording a read, the caller issues a
 * GENERAL→GENERAL barrier from MEMORY_WRITE (the lift thread's copy).
 */
bool
comp_multi_lift_pin_latest(struct comp_multi_lift *lift, uint64_t owner, uint64_t id, struct comp_multi_lift_pin *out);

void
comp_multi_lift_unpin(struct comp_multi_lift_pin *pin);

#ifdef __cplusplus
}
#endif

#endif // XRT_OS_ANDROID
