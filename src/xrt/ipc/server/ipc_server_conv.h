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
struct xrt_conv_wire_event;
struct xrt_conv_wire_generation;
struct xrt_conv_wire_result;
struct xrt_conv_wire_stream_created;
struct xrt_conv_wire_stream_desc;
struct xrt_conv_wire_task_desc;
struct xrt_eye_positions;

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

	/*!
	 * OPTIONAL (spec 4, #2054): import any spec-4 input kind (D3D11 / D3D12
	 * NT + a shared fence, DXGI, CPU bytes in a file mapping) over a region
	 * into @p slot. Takes ownership of both handles. NULL = only the v3
	 * kinds (D3D11 NT / DXGI at the origin) are offered.
	 */
	xrt_result_t (*import_input_ex)(void *ud,
	                                uint64_t stream_id,
	                                const struct conv_input_import *in,
	                                const struct conv_submit_slot *slot);
	/*!
	 * OPTIONAL (spec 4): the stream's export read back into a file mapping
	 * the glue owns (@p copy: refresh it now, after publish_result; else the
	 * current one). NULL = no CPU export kind.
	 */
	bool (*export_cpu)(void *ud,
	                   uint64_t stream_id,
	                   bool copy,
	                   xrt_graphics_buffer_handle_t *out_handle,
	                   uint32_t *out_width,
	                   uint32_t *out_height,
	                   uint32_t *out_format,
	                   uint64_t *out_bytes,
	                   bool *out_realloc);

	/*!
	 * A glue with no D3D textures (the POSIX CPU glue,
	 * ipc_server_conv_cpu.h): spec-4 streams take and give the CPU kind
	 * only, a platform-default export is CPU, and the v3 route (D3D11
	 * shared textures by definition) stays off — the v3 calls keep
	 * answering modes 0 / UNAVAILABLE. import_input / export_output /
	 * export_fence are refusing stubs.
	 */
	bool cpu_only;
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
	//! The adapter the allocator's images live on (packed LUID; 0 = unknown):
	//! conversion-host satellites are spawned on it (--adapter-luid, #2034).
	uint64_t adapter_luid;
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
 * ADR-053 P5 step C: under the CPU route (cpu_only, POSIX) spec 3 is off --
 * its submit and export kinds are D3D11 textures -- EXCEPT for SBS / NVIEW
 * stream create and properties once a weave engine can lift rects
 * (@p enabled; ipc_server_conv_vk.c sets it with the weave bridge). Such a
 * stream is only ever fed by lifted weave rects (the weave path's snapshot)
 * and only ever read by the weave; its properties report SBS / NVIEW only.
 */
void
ipc_server_conv_set_weave_rects(struct ipc_server_conv *sc, bool enabled);

/*!
 * The per-DP eyes sink (roadmap §5.2) as the POSIX service installs it: @p eyes
 * of the DP keyed @p dp_key (its address) into the core's per-DP snapshot,
 * stamped now; retire when that DP is destroyed. (The D3D11 service converts
 * in its own glue file.)
 */
void
ipc_server_conv_eyes_publish(struct ipc_server_conv *sc, uint64_t dp_key, const struct xrt_eye_positions *eyes);

void
ipc_server_conv_eyes_retire(struct ipc_server_conv *sc, uint64_t dp_key);

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


/*
 *
 * Spec 4 (ADR-053 P3, #2054): the conv_* family.
 *
 * A spec-4 stream is an ordinary entry of this module's stream table: its id
 * carries IPC_SERVER_CONV_STREAM_TAG and its v3 mode is the task's legacy
 * mode (DEPTH for DEPTH_*, SBS / NVIEW for VIEWS_MONO), so every v3
 * per-stream call (submit, export, priority, stats, destroy, the weave-rect
 * resolver) serves it unchanged. On this family a VALIDATION_FAILURE is
 * XRT_ERROR_INPUT_UNSUPPORTED; FEATURE_UNSUPPORTED and LIMIT_REACHED keep
 * their v3 codes.
 *
 * Events are OPT-IN per connection: a connection is latched by its first
 * spec-4 call (enum / warm / create / generation) and only then receives
 * task / provider events. They are ordered and deduplicated on the task's
 * EFFECTIVE state (the selected provider's): each delivered
 * TASK_STATE_CHANGED's previous_state is the state last delivered for that
 * task (or the latch-time baseline), an unchanged state is never repeated, and
 * progress moves of >= 5 % are reported at most every 500 ms.
 *
 */

//! Connections that can be latched for spec-4 events at once.
#define IPC_SERVER_CONV_MAX_CLIENTS 64u

/*! @name XR_LIFT_OUTPUT_*_BIT_DXR values (the app's requiredOutputs vocabulary; tests assert equality) @{ */
#define IPC_SERVER_CONV_OUTPUT_AUX_DEPTH 0x01ull
#define IPC_SERVER_CONV_OUTPUT_METRIC 0x02ull
#define IPC_SERVER_CONV_OUTPUT_INTRINSICS 0x04ull
#define IPC_SERVER_CONV_OUTPUT_TEMPORAL 0x08ull
#define IPC_SERVER_CONV_OUTPUT_EXPLICIT_VIEWPOINTS 0x10ull
#define IPC_SERVER_CONV_OUTPUT_CONVERGENCE_HINT 0x20ull
#define IPC_SERVER_CONV_OUTPUT_CONFIDENCE 0x40ull
#define IPC_SERVER_CONV_OUTPUT_COMPOSED 0x80ull
/*! @} */

/*! @name XrLiftImageKindDXR values the service's glue takes / gives (masks are 1 << kind) @{ */
#define IPC_SERVER_CONV_IMAGE_KIND_D3D11_NT 1u
#define IPC_SERVER_CONV_IMAGE_KIND_D3D11_DXGI 2u
#define IPC_SERVER_CONV_IMAGE_KIND_D3D12_NT 7u //!< with import_input_ex
#define IPC_SERVER_CONV_IMAGE_KIND_CPU 16u     //!< input with import_input_ex, export with export_cpu
/*! @} */

/*! Latch @p client_id for events (idempotent; baseline = the effective states now). */
void
ipc_server_conv_v4_latch(struct ipc_server_conv *sc, uint64_t client_id);

bool
ipc_server_conv_v4_latched(struct ipc_server_conv *sc, uint64_t client_id);

/*! conv_core's on_event target (the service wires it): any core thread, core
 *  locks possibly held; queues only. */
void
ipc_server_conv_on_event(struct ipc_server_conv *sc, uint64_t client_id, const struct xrt_conv_wire_event *ev);

/*! conv_poll_event: the connection's next event, or *out_has false. */
xrt_result_t
ipc_server_conv_v4_poll_event(struct ipc_server_conv *sc,
                              uint64_t client_id,
                              bool *out_has,
                              struct xrt_conv_wire_event *out);

/*! conv_get_generation (latches). */
void
ipc_server_conv_v4_get_generation(struct ipc_server_conv *sc,
                                  uint64_t client_id,
                                  struct xrt_conv_wire_generation *out);

/*! The XrLiftImageKindDXR mask (1 << kind) the glue imports (@p export_kinds
 *  false) or exports; 0 without glue. */
uint64_t
ipc_server_conv_v4_image_kinds(struct ipc_server_conv *sc, bool export_kinds);

/*!
 * conv_enum_task (latches): the effective capability + readiness of @p task
 * (XRT_CONV_TASK_*), no provider identity. A reserved, spec-5 or unserved
 * task reads ABSENT. @p out_image_kinds / @p out_export_kinds: what the
 * service's transport takes / gives for it (0 = not reachable yet).
 */
xrt_result_t
ipc_server_conv_v4_enum_task(struct ipc_server_conv *sc,
                             uint64_t client_id,
                             uint32_t task,
                             struct xrt_conv_wire_task_desc *out,
                             uint64_t *out_image_kinds,
                             uint64_t *out_export_kinds);

/*! conv_warm (latches). XRT_ERROR_INPUT_UNSUPPORTED for a non-spec-4 task. */
xrt_result_t
ipc_server_conv_v4_warm(struct ipc_server_conv *sc, uint64_t client_id, uint32_t task);

/*! XR_LIFT_OUTPUT_* bits (IPC_SERVER_CONV_OUTPUT_*) @p d offers. */
uint64_t
ipc_server_conv_v4_output_flags(const struct xrt_conv_wire_task_desc *d);

/*!
 * conv_stream_create (latches), roadmap §7.4:
 *  - XRT_ERROR_INPUT_UNSUPPORTED (VALIDATION_FAILURE): a reserved or spec-5
 *    task, @p legacy_mode not matching the task, an input kind other than
 *    MONO, an export kind the glue does not give (default / D3D11 NT, CPU
 *    with export_cpu), a view count
 *    outside 2..maxViews;
 *  - XRT_ERROR_FEATURE_NOT_SUPPORTED: no route (glue), no non-terminal
 *    candidate, a task without a stream lane, or a @p required_outputs bit
 *    (IPC_SERVER_CONV_OUTPUT_*) missing from a READY provider;
 *  - XRT_ERROR_CLIENT_LIMIT_REACHED: the provider's max_streams;
 *  - XRT_ERROR_OUTPUT_REQUEST_FAILURE: discovery has not described any
 *    provider for the task yet (UNKNOWN: retry).
 * @p in->outputs & XRT_CONV_OUT_DEPTH asks for ADVISORY aux depth
 * (XrLiftDepthRequestDXR): granted when the provider declares it.
 */
xrt_result_t
ipc_server_conv_v4_stream_create(struct ipc_server_conv *sc,
                                 uint64_t client_id,
                                 const struct xrt_conv_wire_stream_desc *in,
                                 uint64_t required_outputs,
                                 uint32_t legacy_mode,
                                 float input_scale,
                                 struct xrt_conv_wire_stream_created *out);

/*!
 * conv_stream_create_v5 (lift spec 5, #2178): ipc_server_conv_v4_stream_create
 * plus the per-create preference HINT @p preferred_provider
 * ("<provider-id>[/<cap_id or model_id>]"; NULL / "" = none). The hint
 * reorders the task's non-terminal candidates for this stream only (and for
 * its rebinds); an unknown or terminal one falls through to the normal
 * selection, never an error. It is never written to the user / machine
 * default. The v4 call is this with no hint.
 */
xrt_result_t
ipc_server_conv_v5_stream_create(struct ipc_server_conv *sc,
                                 uint64_t client_id,
                                 const struct xrt_conv_wire_stream_desc *in,
                                 uint64_t required_outputs,
                                 uint32_t legacy_mode,
                                 float input_scale,
                                 const char *preferred_provider,
                                 struct xrt_conv_wire_stream_created *out);

/*!
 * conv_enum_candidates (lift spec 5, #2178; latches): the read-only candidate
 * providers of @p task (conv_core_enum_candidates), one per call by @p index,
 * with their count and the core's generations (a walk whose generation moved
 * restarts). An index past the count returns the count and an empty
 * candidate; zero providers = count 0, XRT_SUCCESS. A reserved / out-of-range
 * task is XRT_ERROR_INPUT_UNSUPPORTED. Records what the connection saw: a
 * later readiness change that changes the list queues PROVIDERS_CHANGED for it.
 */
xrt_result_t
ipc_server_conv_v5_enum_candidates(struct ipc_server_conv *sc,
                                   uint64_t client_id,
                                   uint32_t task,
                                   uint32_t index,
                                   uint32_t *out_count,
                                   struct xrt_conv_wire_generation *out_generation,
                                   struct xrt_conv_wire_candidate *out_candidate);

/*! conv_stream_reset: drop temporal state before the next frame (cut / seek). */
xrt_result_t
ipc_server_conv_v4_stream_reset(struct ipc_server_conv *sc, uint64_t client_id, uint64_t stream_id);

/*!
 * conv_acquire_result: the v3 acquire (same export rule, same @p out_v3) plus
 * the result's full metadata (@p out: views / depth info twins) and whether
 * temporal state was reset before it (@p out_discontinuity).
 */
xrt_result_t
ipc_server_conv_v4_acquire(struct ipc_server_conv *sc,
                           uint64_t client_id,
                           uint64_t stream_id,
                           bool *out_ready,
                           bool *out_discontinuity,
                           struct ipc_lift_result *out_v3,
                           struct xrt_conv_wire_result *out);

/*!
 * conv_submit_frame (spec 4): the v3 submit (@p args: stream, extent =
 * the region's size, source time, sticky params) of a spec-4 input image
 * @p in (kind, handles, sync, region). Takes ownership of @p in's handles.
 * Results as ipc_server_conv_lift_submit_framed, plus FEATURE_NOT_SUPPORTED
 * when the glue has no import_input_ex.
 */
xrt_result_t
ipc_server_conv_v4_submit(struct ipc_server_conv *sc,
                          uint64_t client_id,
                          const struct ipc_arg_lift_submit *args,
                          const struct conv_input_import *in,
                          const struct xrt_lift_rect_frame *frame,
                          uint64_t *out_frame_id);

/*!
 * ADR-048 / ADR-053 P5: the lifted rect a spec-4 submit converts against.
 *
 * - @p frame valid (a session window or a flagged weave rect): @p frame,
 *   unchanged.
 * - Otherwise (a link or windowless connection): the rect has the SUBMITTED
 *   extent's aspect (@p extent_w x @p extent_h), fitted inside the size
 *   @p frame carries while invalid (the panel, in metres: what the handler
 *   fills for a windowless connection), centred on the panel. Geometry only:
 *   the view synthesis always runs at the submitted extent (output extent ==
 *   input extent), never at the panel's resolution.
 *
 * False (the rect stays unknown, as before) when @p frame is NULL, or invalid
 * with no size, or the extent is 0.
 */
bool
ipc_server_conv_submit_rect(const struct xrt_lift_rect_frame *frame,
                            uint32_t extent_w,
                            uint32_t extent_h,
                            struct xrt_lift_rect_frame *out);

/*! conv_get_cpu_output: a CPU-export stream's current mapping (glue-owned:
 *  the transport duplicates it) and its tightly packed layout. */
bool
ipc_server_conv_v4_cpu_output(struct ipc_server_conv *sc,
                              uint64_t client_id,
                              uint64_t stream_id,
                              xrt_graphics_buffer_handle_t *out_handle,
                              uint32_t *out_width,
                              uint32_t *out_height,
                              uint32_t *out_format,
                              uint64_t *out_bytes);

/*!
 * Spec 4 "viewCount = 0 = match the weave": the weave path tells a lifted
 * rect's stream how many views the active mode weaves (@p views; the batch
 * SBS layout is 2). A VIEWS_MONO task stream created with viewCount 0 is
 * resized to it behind the same handle (view v lands in tile v), within
 * [2, the task's maxViews]; any other stream is left alone.
 */
void
ipc_server_conv_v4_match_weave(struct ipc_server_conv *sc, uint64_t client_id, uint64_t stream_id, uint32_t views);

#ifdef __cplusplus
}
#endif
