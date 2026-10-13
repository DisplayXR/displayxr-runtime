// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_lift v3 handlers -> the conversion core (ADR-053, #2005).
 *
 * The service owns ONE conv_core (targets/service/service_conversion.c builds
 * it from the conversion-provider loader and registers it here with
 * ipc_server_set_conv()). The v3 `lift_*` IPC handlers in
 * ipc_server_handler.c are thin adapters: they ask this module whether a call
 * ROUTES TO THE CORE, and if not they fall through, untouched, to the
 * shipping svc_lift path (comp_d3d11_service_lift_*).
 *
 * The wire is unchanged: every `lift_*` message keeps its struct, the result
 * texture and fence are still exported by the separate lift_get_output /
 * lift_get_depth_output / lift_get_fence calls, only when `output_realloc`
 * says so (roadmap conversion-providers.md §6.6, §2.8).
 *
 * THE ROUTING RULE (precisely):
 *
 *   A v3 call routes to the core iff ALL of
 *     (a) a conversion object is registered (ipc_server_set_conv), and
 *     (b) its GPU I/O glue is installed (ipc_server_conv_set_io with every
 *         slot non-NULL) -- the glue imports the client's shared input
 *         texture into the core's slot images and publishes results into the
 *         per-stream export texture + shared fence; and
 *     (c) per call kind:
 *         - lift_get_properties: some v3 mode the core serves (DEPTH, SBS,
 *           NVIEW) has a selection candidate, OR provider discovery has not
 *           settled yet (cold boot: the honest answer is ACTIVATING);
 *         - lift_stream_create(mode): mode is DEPTH, SBS or NVIEW and the
 *           mode's backing task (conv_legacy_mode_task) has a selection
 *           candidate right now -- in any non-terminal state, so a create
 *           while ACTIVATING is accepted (G3) and served once READY;
 *         - every per-stream call: the stream id is one this module minted
 *           (IPC_SERVER_CONV_STREAM_TAG set and present in its table) and
 *           the glue is still installed.
 *   GAUSSIANS routes only through the latest-wins PHOTO FACADE over the job
 *   lane (#2003, G8): when the glue also carries import_input_cpu and
 *   SPLATS_MONO has a non-terminal candidate. The legacy `dp-lift` adapter
 *   declares no SPLATS capability, so on a box whose only splat source is the
 *   display plug-in GAUSSIANS stays on svc_lift.
 *
 * #2003 registers the legacy module as the `dp-lift` core row and installs
 * the glue at service init (ipc_server_conv_install_gpu); from then on every
 * DEPTH / SBS / NVIEW call routes here and the svc_lift fall-through for them
 * is dead code (deleted in P1b; the handlers WARN once if it is ever taken).
 *
 * Threading: every function is callable from any IPC client thread. One
 * internal mutex guards the stream table (a leaf, never held across a core
 * or glue call that can block on a provider: the core's calls never block on
 * a provider, the glue's must not either).
 *
 * @ingroup ipc_server
 */

#pragma once

#include "xrt/xrt_handles.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_dp_lift.h"
#include "xrt/xrt_lift.h"

#include "conversion/conv_pipeline.h"
#include "conversion/conv_provider_legacy_dp.h"
#include "conversion/conv_stream.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct conv_core;
struct conv_result;
struct conv_submit_slot;
struct ipc_arg_lift_submit;
struct ipc_lift_result;
struct ipc_server_conv;

//! Bit set in every v3 stream id minted by the core route (svc_lift ids are small counters).
#define IPC_SERVER_CONV_STREAM_TAG (1ull << 62)

//! v3 streams the core route tracks at once (all connections).
#define IPC_SERVER_CONV_MAX_STREAMS 64u

//! What the glue reports after publishing one result into a stream's export targets.
struct ipc_server_conv_published
{
	uint64_t fence_value; //!< value the export fence reaches when the copy is done
	uint32_t width;       //!< export texture extent
	uint32_t height;
	uint32_t format; //!< DXGI_FORMAT (the v3 wire carries the graphics-API format)
	bool realloc;    //!< the views export texture was (re)created: the client re-opens it
	//! Depth export (aux depth / DEPTH mode): extent, DXGI format, realloc.
	uint32_t depth_width;
	uint32_t depth_height;
	uint32_t depth_format;
	bool depth_realloc;
};

/*!
 * GPU I/O glue: HOOK for #2003 (the D3D11 service fills it, d3d11_conv_gpu.cpp
 * after the P1b split). Tests install a CPU implementation. Every slot is
 * required for the core route to engage. None may block on a provider or take
 * the compositor's render_mutex for longer than one copy.
 */
struct ipc_server_conv_io_ops
{
	/*!
	 * Snapshot the client's shared input (the lift_submit_frame in_handle,
	 * legacy-DXGI low-bit tag already stripped into @p is_dxgi) of extent
	 * @p width x @p height into @p slot's images (S0). Takes ownership of
	 * @p handle: closes / caches it whatever the outcome.
	 */
	xrt_result_t (*import_input)(void *ud,
	                             uint64_t stream_id,
	                             xrt_graphics_buffer_handle_t handle,
	                             bool is_dxgi,
	                             uint32_t width,
	                             uint32_t height,
	                             const struct conv_submit_slot *slot);

	//! Dispose of an input handle the core route did not import (refused submit).
	void (*release_input)(void *ud, xrt_graphics_buffer_handle_t handle, bool is_dxgi);

	/*!
	 * Optional (#2030): the content key of the input import_input just put in
	 * @p slot (conv_frame_submit::content_key; still-image dedupe of PHOTO
	 * streams). 0 = unknown: the frame converts. NULL = never asked.
	 */
	uint64_t (*input_content_key)(void *ud, uint64_t stream_id, const struct conv_submit_slot *slot);

	/*!
	 * Copy the pinned result @p res into stream @p stream_id's persistent
	 * export texture(s) (created / resized as needed, SHARED_NTHANDLE, no
	 * keyed mutex) and signal its shared export fence. The core unpins @p res
	 * right after this returns.
	 */
	xrt_result_t (*publish_result)(void *ud,
	                               uint64_t stream_id,
	                               const struct conv_result *res,
	                               struct ipc_server_conv_published *out);

	/*! The stream's current export texture (@p depth: the depth one) as a
	 *  service-owned handle the transport duplicates into the caller. */
	bool (*export_output)(void *ud,
	                      uint64_t stream_id,
	                      bool depth,
	                      xrt_graphics_buffer_handle_t *out_handle,
	                      uint32_t *out_width,
	                      uint32_t *out_height,
	                      uint32_t *out_format);

	//! The stream's shared export fence (service-owned handle).
	bool (*export_fence)(void *ud, uint64_t stream_id, xrt_graphics_sync_handle_t *out_handle);

	//! The stream is gone: free its export targets.
	void (*stream_gone)(void *ud, uint64_t stream_id);

	void *ud;

	/*!
	 * OPTIONAL (#2003, the GAUSSIANS facade): the client's shared input read
	 * back as tightly packed RGBA8 CPU pixels (malloc'd, the caller frees), a
	 * job's decoded input. Takes ownership of @p handle. NULL = GAUSSIANS
	 * never routes.
	 */
	xrt_result_t (*import_input_cpu)(void *ud,
	                                 uint64_t stream_id,
	                                 xrt_graphics_buffer_handle_t handle,
	                                 bool is_dxgi,
	                                 uint32_t width,
	                                 uint32_t height,
	                                 uint8_t **out_pixels);
};

/*!
 * Everything the service's GPU side hands the conversion core once its device
 * exists (#2003): image allocation (the service's allocator trampoline swaps
 * to it), the S5 / S6 stages (conv_core_set_gpu), the v3 route's I/O, and
 * the `dp-lift` provider's backend (registered by the service as a core row).
 */
struct ipc_server_conv_gpu
{
	struct conv_image_alloc_ops alloc;
	struct conv_viewsyn_ops viewsyn;
	struct conv_depth_readback_ops readback;
	struct conv_dp_lift_backend dp_lift;
	struct ipc_server_conv_io_ops io;
	float nominal_z_m; //!< the display's nominal viewing distance (S3); ≤ 0 = unknown
};

/*!
 * The service target's half of an install (it owns the allocator and the
 * provider registration): called with the gpu on install, NULL on removal,
 * BEFORE the I/O glue is installed / AFTER it is removed.
 */
typedef void (*ipc_server_conv_gpu_hook_fn)(void *ud, const struct ipc_server_conv_gpu *gpu);

//! Register the service target's hook (set before ipc_server_main).
void
ipc_server_conv_set_gpu_hook(struct ipc_server_conv *sc, ipc_server_conv_gpu_hook_fn fn, void *ud);

/*!
 * Install (or, with NULL, remove) the GPU side: conv_core_set_gpu, the
 * service hook (allocator + `dp-lift`), then ipc_server_conv_set_io — the
 * route engages last. Removal runs in the reverse order and destroys every
 * core-routed stream first (their images belong to the GPU side).
 */
void
ipc_server_conv_install_gpu(struct ipc_server_conv *sc, const struct ipc_server_conv_gpu *gpu);

/*!
 * The weave path's resolver (#2003): a v3 stream id this module minted for
 * @p client_id -> its core stream and v3 mode. False = not ours.
 */
bool
ipc_server_conv_resolve_stream(
    struct ipc_server_conv *sc, uint64_t client_id, uint64_t stream_id, uint64_t *out_core_id, uint32_t *out_mode);


/*
 *
 * Lifetime + registration (service target).
 *
 */

/*! Wrap @p core (not owned: the service destroys it after this object). */
xrt_result_t
ipc_server_conv_create(struct conv_core *core, struct ipc_server_conv **out_sc);

/*! Destroy every stream still tracked, then the object. NULL-safe. */
void
ipc_server_conv_destroy(struct ipc_server_conv **sc_ptr);

/*! Install (or, with NULL, remove) the GPU I/O glue. Copied. */
void
ipc_server_conv_set_io(struct ipc_server_conv *sc, const struct ipc_server_conv_io_ops *ops);

struct conv_core *
ipc_server_conv_get_core(struct ipc_server_conv *sc);

/*! Register (or, with NULL, clear) the process-wide object the handlers use.
 *  Same lifetime model as the other ipc_server_set_* hooks: set before
 *  ipc_server_main, cleared after it returns. */
void
ipc_server_set_conv(struct ipc_server_conv *sc);

struct ipc_server_conv *
ipc_server_get_conv(void);


/*
 *
 * Routing (see the rule above).
 *
 */

bool
ipc_server_conv_routes_properties(struct ipc_server_conv *sc);

bool
ipc_server_conv_routes_create(struct ipc_server_conv *sc, uint32_t mode, uint32_t content_hint);

bool
ipc_server_conv_owns_stream(struct ipc_server_conv *sc, uint64_t stream_id);


/*
 *
 * The v3 adapter. @p client_id is the connection's lift owner token.
 *
 */

/*!
 * XrLiftPropertiesDXR for a v3 caller, from the core (roadmap §7.3):
 *  - state: conv_core_v3_state over DEPTH | SBS | NVIEW (ACTIVATING while
 *    discovery is unsettled or any backing task is non-terminal,
 *    UNAVAILABLE only when settled with no candidate, else READY);
 *  - modes: every mode whose backing task has a non-terminal candidate;
 *    NVIEW only when maxViews > 2;
 *  - max_views / max_streams / typical latency from VIEWS_MONO, else DEPTH;
 *    aux depth from VIEWS_MONO's DEPTH output (AFFINE kinds are RELATIVE).
 * No provider identity: backend reads "conversion".
 */
void
ipc_server_conv_lift_properties(struct ipc_server_conv *sc, struct xrt_dp_lift_caps *out);

xrt_result_t
ipc_server_conv_lift_stream_create(struct ipc_server_conv *sc,
                                   uint64_t client_id,
                                   uint32_t mode,
                                   uint32_t content_hint,
                                   float input_scale,
                                   uint32_t aux_outputs,
                                   uint32_t aux_depth_format,
                                   uint64_t *out_stream_id);

xrt_result_t
ipc_server_conv_lift_stream_destroy(struct ipc_server_conv *sc, uint64_t client_id, uint64_t stream_id);

/*! @p handle is consumed in every case (imported or released through the glue). */
xrt_result_t
ipc_server_conv_lift_submit(struct ipc_server_conv *sc,
                            uint64_t client_id,
                            const struct ipc_arg_lift_submit *args,
                            xrt_graphics_buffer_handle_t handle,
                            bool is_dxgi,
                            uint64_t *out_frame_id);

/*!
 * ipc_server_conv_lift_submit with the submitting session's window as the
 * lifted rect (ADR-048: the viewpoints are rebased to it; the result echoes
 * it). @p frame NULL / invalid = the panel centre, as svc_lift did.
 */
xrt_result_t
ipc_server_conv_lift_submit_framed(struct ipc_server_conv *sc,
                                   uint64_t client_id,
                                   const struct ipc_arg_lift_submit *args,
                                   xrt_graphics_buffer_handle_t handle,
                                   bool is_dxgi,
                                   const struct xrt_lift_rect_frame *frame,
                                   uint64_t *out_frame_id);

/*!
 * xrAcquireLiftBlobDXR for a core-routed GAUSSIANS stream (the facade): the
 * newest SUCCEEDED job's blob, with the v3 two-call latch. On delivery
 * (@p capacity >= byte_count) @p out_bytes receives a malloc'd copy.
 */
xrt_result_t
ipc_server_conv_lift_acquire_blob(struct ipc_server_conv *sc,
                                  uint64_t client_id,
                                  uint64_t stream_id,
                                  uint64_t capacity,
                                  bool *out_ready,
                                  struct xrt_lift_blob_info *out_info,
                                  uint8_t **out_bytes);

xrt_result_t
ipc_server_conv_lift_acquire(struct ipc_server_conv *sc,
                             uint64_t client_id,
                             uint64_t stream_id,
                             bool *out_ready,
                             struct ipc_lift_result *out);

bool
ipc_server_conv_lift_export_output(struct ipc_server_conv *sc,
                                   uint64_t client_id,
                                   uint64_t stream_id,
                                   bool depth,
                                   xrt_graphics_buffer_handle_t *out_handle,
                                   uint32_t *out_width,
                                   uint32_t *out_height,
                                   uint32_t *out_format);

bool
ipc_server_conv_lift_export_fence(struct ipc_server_conv *sc,
                                  uint64_t client_id,
                                  uint64_t stream_id,
                                  xrt_graphics_sync_handle_t *out_handle);

xrt_result_t
ipc_server_conv_lift_set_priority(struct ipc_server_conv *sc,
                                  uint64_t client_id,
                                  uint64_t stream_id,
                                  uint32_t priority);

xrt_result_t
ipc_server_conv_lift_stats(struct ipc_server_conv *sc,
                           uint64_t client_id,
                           uint64_t stream_id,
                           struct xrt_lift_stream_stats *out);

/*! conv_hint_interest (the prewarm trigger, PrewarmOn = interest). Forwarded
 *  to the core whether or not the glue is installed: warming is
 *  provider-side and costs nothing on the wire. */
void
ipc_server_conv_hint_interest(struct ipc_server_conv *sc, uint64_t client_id, uint64_t task_mask);

/*! conv_warm (DIAG, `displayxr-cli conv warm`, #2029): the explicit warm of
 *  @p task's selected capability (conv_core_warm_task_explicit). No core
 *  (DXR_CONV=0, not started): XRT_ERROR_FEATURE_NOT_SUPPORTED, states 0. */
xrt_result_t
ipc_server_conv_warm(struct ipc_server_conv *sc, uint32_t task, uint32_t *out_prev_state, uint32_t *out_state);

/*! The connection is gone: its v3 streams (and the core's streams / jobs
 *  for it) die with it. */
void
ipc_server_conv_client_gone(struct ipc_server_conv *sc, uint64_t client_id);

#ifdef __cplusplus
}
#endif
