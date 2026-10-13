// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The conversion core (ADR-053): the one object the service owns.
 *
 * Wires the registry + describe cache (conv_registry), deterministic
 * selection (conv_select), the readiness machines (conv_readiness), one host
 * per provider (conv_host_iface; in-service: conv_host_inproc), one stream
 * dispatcher and one job worker per host (conv_stream, conv_job), the
 * pipeline composer (conv_pipeline) and the policy stages (conv_policy).
 *
 * Callers:
 *  - the loader (target_conversion_plugin_loader) → conv_core_add_provider;
 *  - the service (#2003) → create / destroy, eyes publishing, the GPU glue
 *    callbacks in conv_core_config (image allocation, view synthesis,
 *    depth readback);
 *  - the IPC handlers (#2005) → properties (task descs, v3 state,
 *    generations), conv_hint_interest, stream create / submit / acquire /
 *    reset / destroy, job submit / status / read / cancel, client teardown.
 *
 * Threads it owns: one control thread (readiness timers, warm progress, the
 * provider-state poll, the stall watchdog, rebinding), and per host one
 * dispatcher and one job worker; the host owns its exec thread(s).
 * Every function here is callable from IPC / producer threads: none blocks on
 * a provider, none takes the compositor's render_mutex.
 *
 * Lock order (roadmap §5.2): streams_mtx → registry_mtx → host mutex;
 * jobs_mtx → registry_mtx → host mutex. registry_mtx and the host mutexes are
 * leaves. on_event may be called with core locks held: it must only queue.
 *
 * @ingroup aux_conversion
 */

#pragma once

#include "xrt/xrt_conv_wire.h"
#include "xrt/xrt_conversion_plugin.h"

#include "conv_job.h"
#include "conv_pipeline.h"
#include "conv_policy.h"
#include "conv_registry.h"
#include "conv_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

struct conv_core;

/*! @name PrewarmOn @{ */
#define CONV_PREWARM_INTEREST 0u     //!< warm the prewarm tasks on the first conv_hint_interest (desktop default)
#define CONV_PREWARM_FIRST_CREATE 1u //!< warm only at the first stream / job create (battery default)
#define CONV_PREWARM_START 2u        //!< warm at core start (kiosk opt-in)
/*! @} */

//! Default prewarm tasks: views-mono, depth-video.
#define CONV_PREWARM_DEFAULT_MASK ((1ull << XRT_CONV_TASK_VIEWS_MONO) | (1ull << XRT_CONV_TASK_DEPTH_VIDEO))

//! Local reason: the stall watchdog abandoned an in-service provider (FAILED(hung)).
#define CONV_REASON_HUNG (CONV_REASON_LOCAL_BASE + 0x10u)

//! Display processors with an eyes snapshot slot (live at once; LRU beyond).
#define CONV_CORE_MAX_DPS 8u

/*! @name Eyes keys (roadmap §5.2). A display processor is named by an opaque,
 *  non-zero key its publisher chooses (the service: the DP's address), so the
 *  core never dereferences a DP and needs no index bookkeeping. @{ */
//! No binding: read the most recent snapshot any DP published (single screen).
#define CONV_EYES_DP_ANY 0ull
//! Never read eyes: the nominal viewer.
#define CONV_EYES_DP_NONE (~0ull)
/*! @} */

struct conv_core_config
{
	/* GPU glue, injected by the service (CPU implementations in tests). */
	struct conv_image_alloc_ops alloc;       //!< required
	struct conv_viewsyn_ops viewsyn;         //!< S5; render == NULL = no composed pipelines
	struct conv_depth_readback_ops readback; //!< S6 / S7 for GPU depth; may be empty

	/*! S7 runtime auto-convergence (rt#1978 u_lift_autoconv behind the
	 *  observer interface); NULL = none. One observer per stream. */
	bool (*create_convergence_observer)(void *ud, struct conv_observer *out);
	void *observer_ud;

	/*! Pushed events (TASK_STATE_CHANGED, JOB_STATE_CHANGED,
	 *  PROVIDERS_CHANGED). client_id 0 = every client. May be called with
	 *  core locks held and from any core thread: queue, never call back. */
	void (*on_event)(void *ud, uint64_t client_id, const struct xrt_conv_wire_event *ev);
	void *event_ud;

	/* Policy (HKCU / HKLM resolved by the caller, conv_select_resolve_*). */
	const char *preferred[XRT_CONV_TASK_RESERVED_FIRST]; //!< PreferredProvider\<task>; NULL = none (copied)
	bool allow_research_only;
	uint64_t prewarm_task_mask; //!< 0 = CONV_PREWARM_DEFAULT_MASK
	uint32_t prewarm_on;        //!< CONV_PREWARM_*

	/* Geometry for the policy stages. */
	float nominal_z_m;        //!< ≤ 0 = U_LIFT_NOMINAL_Z_DEFAULT_M
	float default_budget;     //!< ≤ 0 = CONV_POLICY_BUDGET_DEFAULT
	float rear_cap_m;         //!< ADR-040 rear cap for lifted content; < 0 = unlimited (0 is "nothing behind")
	bool recenter_off;        //!< DXR_LIFT_RECENTER=0
	uint64_t eyes_max_age_ns; //!< older snapshots count as stale; 0 = no limit

	/* Control thread. */
	uint32_t tick_ms; //!< 0 = 100
	//! Stall watchdog for stream calls: max(this, 20 × est_latency); 0 = 10 s.
	uint64_t watchdog_stream_ns;
};

/*! Create the core and start its control thread. */
xrt_result_t
conv_core_create(const struct conv_core_config *cfg, struct conv_core **out_core);

/*! Stop every thread, destroy streams / jobs / hosts (providers are destroyed
 *  through their hosts; a hung one is leaked). */
void
conv_core_destroy(struct conv_core **core_ptr);

/*! The GPU-side stages the service can only build once its device exists
 *  (#2003): the core is created at service start, the D3D11 device later. */
struct conv_core_gpu
{
	struct conv_viewsyn_ops viewsyn;         //!< S5; render == NULL = no composed pipelines
	struct conv_depth_readback_ops readback; //!< S6 / S7 for GPU depth
	//! The display's nominal viewing distance (S3's straight-on viewer), known
	//! with the display; ≤ 0 = keep conv_core_config::nominal_z_m.
	float nominal_z_m;
};

/*! Install (or, with NULL, remove) the GPU-side stages. Replaces the
 *  conv_core_config fields of the same names. Call while no stream is alive
 *  (the dispatchers read them between conversions without a lock). */
void
conv_core_set_gpu(struct conv_core *core, const struct conv_core_gpu *gpu);


/*
 *
 * Providers (loader / registry).
 *
 */

/*!
 * Register one provider. With a live iface + instance the core wraps it in an
 * in-service host (conv_host_inproc) and describes it; without, its rows can
 * still come from the describe cache. Returns the provider index, or -1.
 */
int32_t
conv_core_add_provider(struct conv_core *core, const struct conv_provider_desc *desc);

/*! Restore rows from a persisted describe cache (before or after adding providers). */
int32_t
conv_core_load_describe_cache(struct conv_core *core, const uint8_t *buf, size_t len);

/*! Serialise the describe cache (buf NULL = size). */
size_t
conv_core_save_describe_cache(struct conv_core *core, uint8_t *buf, size_t buf_size);

/*! Mark a capability row as validated against the stereo ground-truth corpus:
 *  its suggested_convergence / suggested_strength hints are then used. */
void
conv_core_set_corpus_validated(struct conv_core *core, const char *provider_id, uint32_t cap_id, bool validated);

/*! A provider reported notify_changed / a registry change: re-describe now. */
void
conv_core_provider_changed(struct conv_core *core, const char *provider_id);

/*! `conv disable` / `conv enable` for a provider. */
void
conv_core_set_provider_enabled(struct conv_core *core, const char *provider_id, bool enabled);


/*
 *
 * Properties (v3 / v4 handlers, CLI, status snapshot).
 *
 */

void
conv_core_get_generation(struct conv_core *core, struct xrt_conv_wire_generation *out);

/*! The EFFECTIVE capability of @p task (selected row, or the composed
 *  pipeline) and its readiness; no provider identity. Returns false for a
 *  task v1 does not serve. With no candidate the state is UNKNOWN while
 *  discovery is unsettled, else ABSENT. */
bool
conv_core_get_task_desc(struct conv_core *core, uint32_t task, struct xrt_conv_wire_task_desc *out);

/*! XrLiftPropertiesDXR::state for a v3 caller asking about @p v3_modes
 *  (CONV_V3_MODE_* bits; 0 = all four). */
uint32_t
conv_core_v3_state(struct conv_core *core, uint32_t v3_modes, bool photo_content);

/*! Every registered provider is described (live or cached) or terminal.
 *  The v3 adapter (#2005) routes a properties query to the core while this
 *  is false even before any row exists (cold boot: ACTIVATING, not
 *  UNAVAILABLE). */
bool
conv_core_discovery_settled(struct conv_core *core);

/*! Selection has a candidate for @p task right now (any state; a terminal
 *  one only when it is the last resort). The v3 adapter's routing rule. */
bool
conv_core_task_has_candidate(struct conv_core *core, uint32_t task);

/*! Copy the registry (providers, rows, readiness) for diagnostics: the
 *  service's status-snapshot source. Pointers in the copy (iface / inst)
 *  must not be called. */
void
conv_core_copy_registry(struct conv_core *core, struct conv_registry *out);

/*! conv_hint_interest: a client enabled XR_DXR_lift (task_mask 0 = default
 *  set). Prewarms per PrewarmOn. */
void
conv_core_hint_interest(struct conv_core *core, uint64_t client_id, uint64_t task_mask);

/*! conv_warm / conv_evict for a task's selected capability. */
xrt_result_t
conv_core_warm_task(struct conv_core *core, uint32_t task);

xrt_result_t
conv_core_evict_task(struct conv_core *core, uint32_t task);

/*! Readiness of the selected capability of @p task (XRT_CONV_STATE_*). */
uint32_t
conv_core_task_state(struct conv_core *core, uint32_t task);


/*
 *
 * Streams.
 *
 */

/*!
 * conv_stream_create. Selects a capability (readiness never reorders),
 * composes the pipeline, requests a warm when cold, and returns at once.
 * @return XRT_SUCCESS; XRT_ERROR_FEATURE_NOT_SUPPORTED (no candidate, view
 *         count out of range, READY capability lacking a required output);
 *         XRT_ERROR_CLIENT_LIMIT_REACHED (max_streams).
 */
xrt_result_t
conv_core_stream_create(struct conv_core *core,
                        uint64_t client_id,
                        const struct xrt_conv_wire_stream_desc *desc,
                        const struct conv_stream_options *opts,
                        struct xrt_conv_wire_stream_created *out);

/*! Asynchronous: the stream stops accepting frames now and is reaped by its
 *  dispatcher once no result is pinned. */
xrt_result_t
conv_core_stream_destroy(struct conv_core *core, uint64_t stream_id);

/*! conv_stream_reset (cut / seek): FRAME_RESET on the next conversion,
 *  `discontinuity` on its result, observers reset. */
xrt_result_t
conv_core_stream_reset(struct conv_core *core, uint64_t stream_id);

xrt_result_t
conv_core_stream_set_priority(struct conv_core *core, uint64_t stream_id, uint32_t priority);

/*! Producer, step 1: pick an input slot (latest-wins: may drop a pending
 *  frame) and get runtime-allocated images to snapshot into. */
xrt_result_t
conv_core_stream_begin_submit(struct conv_core *core,
                              uint64_t stream_id,
                              const struct conv_submit_layout *layout,
                              struct conv_submit_slot *out);

/*! Producer, step 2: the snapshot is complete. Returns the frame id via
 *  @p out_frame_id. Never waits for the provider. */
xrt_result_t
conv_core_stream_commit_submit(struct conv_core *core,
                               uint64_t stream_id,
                               int32_t slot,
                               const struct conv_frame_submit *frame,
                               uint64_t *out_frame_id);

void
conv_core_stream_abort_submit(struct conv_core *core, uint64_t stream_id, int32_t slot);

/*! Pin the latest result (@p only_newer: newer than the last newer-than
 *  acquire). XRT_ERROR_NO_IMAGE_AVAILABLE when none qualifies. */
xrt_result_t
conv_core_stream_acquire(struct conv_core *core, uint64_t stream_id, bool only_newer, struct conv_result *out);

void
conv_core_stream_release(struct conv_core *core, uint64_t stream_id, uint32_t ring_slot);

xrt_result_t
conv_core_stream_get_stats(struct conv_core *core, uint64_t stream_id, struct conv_stream_stats *out);


/*
 *
 * Jobs.
 *
 */

xrt_result_t
conv_core_job_submit(struct conv_core *core,
                     uint64_t client_id,
                     const struct conv_job_create_info *info,
                     uint64_t *out_job_id);

xrt_result_t
conv_core_job_status(struct conv_core *core, uint64_t job_id, struct conv_job_status *out);

/*! conv_job_fetch metadata (written, blob format, delivery, size, infos) of
 *  output @p output_index (0 = primary). */
xrt_result_t
conv_core_job_result(struct conv_core *core,
                     uint64_t job_id,
                     uint32_t output_index,
                     struct xrt_conv_wire_job_result *out);

/*! Chunked read of a SUCCEEDED job's output (no size cap). */
xrt_result_t
conv_core_job_read(struct conv_core *core,
                   uint64_t job_id,
                   uint32_t output_index,
                   uint64_t offset,
                   void *buf,
                   uint64_t capacity,
                   uint64_t *out_count,
                   uint64_t *out_total);

xrt_result_t
conv_core_job_cancel(struct conv_core *core, uint64_t job_id);

/*! Drop a job (cancelling it first if it still runs; asynchronous). */
xrt_result_t
conv_core_job_destroy(struct conv_core *core, uint64_t job_id);


/*
 *
 * Connections and eyes.
 *
 */

/*! A connection went away: its streams and jobs die with it. */
void
conv_core_client_gone(struct conv_core *core, uint64_t client_id);

/*!
 * A display processor's eye listener / timer: publish its snapshot under
 * @p dp_key (non-zero, not CONV_EYES_DP_NONE). One snapshot per DP: a stream
 * bound to the DP that owns its rect reads that DP's eyes, in that DP's
 * display space (multi-screen M6). A new key takes a free slot, else a retired
 * one, else the least recently published one.
 */
void
conv_core_eyes_publish(struct conv_core *core, uint64_t dp_key, const struct conv_eyes *eyes);

/*!
 * The DP went away (multi-screen M6 segment removed, a client's DP
 * destroyed): streams bound to it read stale (the nominal viewer) until they
 * are rebound, and the "any DP" snapshot forgets it if it was the last writer.
 */
void
conv_core_eyes_retire(struct conv_core *core, uint64_t dp_key);

/*!
 * Bind stream @p stream_id to the DP that owns its rect (CONV_EYES_DP_ANY =
 * unbound, CONV_EYES_DP_NONE = never tracked). A submit can rebind as well
 * (conv_frame_submit::eyes_dp), which is how a rect that moves to another
 * screen follows it.
 */
xrt_result_t
conv_core_stream_bind_eyes(struct conv_core *core, uint64_t stream_id, uint64_t dp_key);

/*!
 * Read the eyes a stream bound to @p dp_key would use at @p now_ns. False
 * (out->valid false) = stale: the nominal viewer. An unknown key (the DP never
 * published, or was evicted) reads the "any DP" snapshot -- the pre-M6
 * behaviour -- only while a single DP has ever published in this core's
 * lifetime; once a second DP has published it reads stale, never another
 * screen's eyes. A retired key always reads stale; CONV_EYES_DP_ANY always
 * reads the newest snapshot of any DP. Diagnostics and tests; the dispatchers
 * call it per conversion.
 */
bool
conv_core_eyes_read(struct conv_core *core, uint64_t dp_key, uint64_t now_ns, struct conv_eyes *out);

#ifdef __cplusplus
}
#endif
