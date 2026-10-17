// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Conversion core internals shared by conv_core.c, conv_stream.c and
 *         conv_job.c. Not for use outside src/xrt/conversion.
 * @ingroup aux_conversion
 */

#pragma once

#include "conv_core.h"
#include "conv_host_iface.h"

#include "os/os_threading.h"
#include "util/u_lift_mailbox.h"

#ifdef __cplusplus
extern "C" {
#endif

//! A stream's or job's selected capability.
struct conv_binding
{
	uint32_t provider; //!< registry provider index == host slot index
	uint32_t cap_id;
	bool composed;
};

//! Output targets per ring slot: views, depth, confidence, disparity.
#define CONV_RING_TARGETS 4u

struct conv_in_slot
{
	uint32_t image_count;
	struct xrt_conv_image images[CONV_STREAM_MAX_SUBMIT_IMAGES];
	uint32_t role[CONV_STREAM_MAX_SUBMIT_IMAGES];
	struct conv_host *reg_host; //!< host the images are registered with (NULL = none)
	uint32_t reg_epoch;         //!< the host's epoch at registration (satellite restarts)
	uint32_t reg_idx[CONV_STREAM_MAX_SUBMIT_IMAGES];
	struct conv_frame_submit frame; //!< the committed frame's inputs
};

struct conv_ring_slot
{
	/* Targets (runtime-allocated, exported to clients). */
	struct xrt_conv_image targets[CONV_RING_TARGETS]; //!< views, depth, confidence, disparity
	struct conv_host *reg_host;
	uint32_t reg_epoch; //!< the host's epoch at registration (satellite restarts)
	uint32_t reg_idx[CONV_RING_TARGETS];
	uint32_t in_w, in_h; //!< input extent the targets were sized for (0 = none)
	uint32_t sized_key;  //!< outputs / views the targets were sized for
	uint64_t alloc_gen;  //!< bumps on every (re)allocation

	/* #2030: GPU mirrors of CPU-memory results (views, depth), published in
	 * their place; allocated on first need, kept while the extent holds. */
	struct xrt_conv_image mirror[CONV_RING_TARGETS];
	uint32_t mirror_layers[CONV_RING_TARGETS];
	bool mirrored[CONV_RING_TARGETS]; //!< the published result is the mirror

	/* Published result. */
	uint32_t written;          //!< XRT_CONV_OUT_* published to the app (S8 mask)
	uint32_t provider_written; //!< what the provider wrote this frame, before the mask
	struct xrt_conv_views_info views_info;
	struct xrt_conv_depth_info depth_info;
	float active[4];
	float rect_center[3]; //!< ADR-048 echo (the frame's rect)
	float rect_size[2];
	bool discontinuity;
	bool provisional;
};

struct conv_stream
{
	struct conv_stream *next;
	uint64_t id;
	uint64_t client_id;
	uint32_t priority;
	uint32_t quality_hint;
	uint32_t content_hint;
	uint32_t max_input_w, max_input_h;
	struct conv_stream_options opts;
	struct conv_pipeline_request req;
	struct conv_pipeline pipe;

	struct conv_binding bind;
	struct conv_binding rebind;
	bool rebind_pending;
	bool unsatisfied;

	struct u_lift_mailbox mb;
	struct conv_in_slot in[U_LIFT_INPUT_SLOTS];
	struct conv_ring_slot out[U_LIFT_RING_SIZE];
	uint64_t consumer_alloc_gen; //!< alloc generation the last acquire saw

	/* Provider side (dispatcher thread only). */
	uint64_t pstream;
	bool pstream_created;
	bool pstream_failed;
	struct conv_host *pstream_host;
	uint32_t pstream_epoch; //!< host epoch the provider stream lives in

	/* Temporal state. */
	bool reset_pending;         //!< FRAME_RESET on the next conversion
	bool discontinuity_pending; //!< `discontinuity` on the next result
	//! A published result carried `discontinuity` that the consumer has not
	//! acquired (latest-wins skipped it): the next acquired result carries it.
	bool discontinuity_unread;
	struct conv_policy_view_state vstate;
	struct conv_observer observer;
	bool has_observer;
	struct xrt_conv_depth_info last_depth;
	bool has_last_depth;

	/* Still-image dedupe (PHOTO; dispatcher thread only): what the latest
	 * published result was converted from. */
	struct
	{
		bool valid;
		uint64_t content_key;
		int32_t out_slot;
		struct conv_binding bind;
		uint32_t outputs;
		bool provisional;
		struct xrt_conv_params params; //!< S3's decisions (frame id / time / flags cleared)
		float budget, rear_cap_m;
	} still;

	struct conv_stream_stats stats;
	uint64_t last_stats_log_ns;
	bool dead;
	bool converting;
};

struct conv_stream_lane
{
	struct conv_core *core;
	uint32_t provider;
	struct os_thread thread;
	struct os_cond cond; //!< waits on core->streams_mtx
	bool started;
	bool stop;
	struct u_lift_sched sched;
};

struct conv_job
{
	struct conv_job *next;
	struct conv_core *core;
	uint64_t id;
	uint64_t client_id;
	uint32_t task;
	uint32_t priority;
	struct conv_binding bind;
	char preferred_provider[XRT_CONV_WIRE_PREFERENCE_MAX]; //!< per-job hint, kept for a rebind ("" = none)

	uint32_t status; //!< CONV_JOB_*
	uint32_t failure_reason;
	float progress;
	char stage_label[32];
	uint64_t created_ns, started_ns, ended_ns;
	uint64_t last_progress_ns;
	uint64_t last_emit_ns;
	float last_emit_progress;
	bool cancel;
	bool destroyed; //!< the client dropped it: free once it stops running
	bool running;
	uint32_t host_losses; //!< times its host died under it (one retry, then FAILED)

	struct xrt_conv_job_in in;
	void *owned[XRT_CONV_MAX_FRAME_IMAGES]; //!< CPU input copies
	struct xrt_conv_params params;
	uint32_t requested_outputs;
	struct conv_stage stage;
	struct xrt_conv_job_out out;
	//! ADR-057: the facts of a SPLATS_* job; out.splat_result points here while it runs.
	struct xrt_conv_splat_result splat_result;
	xrt_result_t result;
};

struct conv_job_lane
{
	struct conv_core *core;
	uint32_t provider;
	struct os_thread thread;
	struct os_cond cond; //!< waits on core->jobs_mtx
	bool started;
	bool stop;
};

struct conv_warm_op
{
	struct conv_host_cmd cmd;
	struct conv_core *core;
	uint32_t provider;
	uint32_t cap_id; //!< 0 = free slot
	uint32_t epoch;  //!< host epoch at submit: a crash casualty is not a warm failure
	bool busy;
	bool cancel;
};

//! conv_host::event_ud of a provider's host.
struct conv_host_listener
{
	struct conv_core *core;
	uint32_t provider;
};

struct conv_host_slot
{
	struct conv_host *host; //!< NULL = not loaded (cache-only rows)
	struct conv_host_listener listener;
	struct conv_stream_lane lane;
	struct conv_job_lane jlane;
	struct conv_warm_op warms[XRT_CONV_MAX_CAPABILITIES];
	bool hung;
	//! Satellite: the host process is gone (LOST seen, RESTORED not yet).
	bool down;
	//! Satellite: RESTORED seen; re-warm the restarting rows after the re-describe.
	bool rewarm;
	//! Satellite: a non-crash exit (idle, update) evicted READY rows; RESTORED re-warms them.
	bool rewarm_evicted;
	uint32_t restarts; //!< LOST events seen (diagnostics)
	uint64_t generation_seen;
};

//! Host lifecycle events, queued by the listener and applied by the control thread.
#define CONV_CORE_HOST_EVQ 64u

struct conv_host_evq_entry
{
	uint32_t provider;
	uint32_t ev; //!< CONV_HOST_EV_*
	int32_t detail;
};

struct conv_validated
{
	char provider_id[CONV_ID_MAX];
	uint32_t cap_id;
};

#define CONV_CORE_MAX_VALIDATED 32u

struct conv_core
{
	struct conv_core_config cfg;
	char preferred[XRT_CONV_TASK_RESERVED_FIRST][CONV_ID_MAX + 16];

	/* registry_mtx: registry, readiness, generations, warm ops (leaf). */
	struct os_mutex registry_mtx;
	struct os_cond ctrl_cond;
	struct os_thread ctrl_thread;
	bool ctrl_started;
	bool stop;
	bool rebind_check;
	bool redescribe[CONV_REGISTRY_MAX_PROVIDERS];
	//! A registry change retried this provider's rows (#2003): the re-describe
	//! it triggers must not demote them back to ABSENT on the provider's
	//! stale "absent" (a warm is what tries again).
	bool retrying[CONV_REGISTRY_MAX_PROVIDERS];
	struct conv_registry reg;
	uint64_t conv_gen;
	uint64_t status_gen;
	struct conv_validated validated[CONV_CORE_MAX_VALIDATED];
	uint32_t validated_count;
	bool interest_seen;
	bool prewarm_after_describe; //!< a provider change re-described: apply PrewarmOn again (#2005)
	bool first_create_seen;
	bool wake_pending; //!< a row became READY: the control thread wakes the lanes

	struct conv_host_slot hosts[CONV_REGISTRY_MAX_PROVIDERS];

	/* evq_mtx (leaf): host lifecycle events, pushed from host threads. */
	struct os_mutex evq_mtx;
	struct conv_host_evq_entry evq[CONV_CORE_HOST_EVQ];
	uint32_t evq_count;
	uint32_t evq_dropped;

	/* streams_mtx: the stream table (held between commands only). */
	struct os_mutex streams_mtx;
	struct conv_stream *streams;
	uint64_t next_stream_id;

	/* jobs_mtx: the job table. */
	struct os_mutex jobs_mtx;
	struct conv_job *jobs;
	uint64_t next_job_id;

	/* eyes_mtx (leaf): the per-DP eyes table (roadmap §5.2). */
	struct os_mutex eyes_mtx;
	struct conv_eyes_slot eyes[CONV_CORE_MAX_DPS];
	uint64_t eyes_key[CONV_CORE_MAX_DPS];        //!< 0 = slot never used
	uint64_t eyes_publish_seq[CONV_CORE_MAX_DPS]; //!< LRU order
	uint64_t eyes_seq;
	struct conv_eyes_slot eyes_any; //!< the most recent snapshot of any DP
	uint64_t eyes_any_key;          //!< who wrote eyes_any
	uint64_t eyes_first_key;        //!< the first DP that ever published (0 = none yet)
	bool eyes_multi_dp;             //!< a second DP has published: unknown keys read stale
};


/*
 *
 * conv_core.c, used by the lanes.
 *
 */

//! XRT_CONV_STATE_* of a row (registry_mtx held).
uint32_t
conv_core_row_state_locked(struct conv_core *core, uint32_t provider, uint32_t cap_id);

//! The row's capability (registry_mtx held); false = gone.
bool
conv_core_row_cap_locked(struct conv_core *core, uint32_t provider, uint32_t cap_id, struct xrt_conv_capability *out);

//! The row is corpus-validated (registry_mtx held).
bool
conv_core_row_validated_locked(struct conv_core *core, uint32_t provider, uint32_t cap_id);

/*! Apply a warm-type readiness event (CONV_EV_WARM_REQUESTED /
 *  CONV_EV_CREATE_REQUESTED) to a row and start its warm when due
 *  (registry_mtx held). */
void
conv_core_request_warm_locked(struct conv_core *core, uint32_t provider, uint32_t cap_id, uint32_t ev_type);

/*!
 * Select a capability for @p task (registry_mtx held). Returns the select
 * reason (conv_select_reason) and fills @p out and its state when a row was
 * chosen. @p busy_providers (CONV_REGISTRY_MAX_PROVIDERS entries, may be
 * NULL): providers with live streams. A job whose choice lacks
 * LANES_CONCURRENT on a busy provider is routed to the next selectable
 * candidate that either declares it or sits on an idle provider, when one
 * exists (discovery spec §2.3).
 *
 * @p hint: the per-create preference of the stream / job (lift spec 5,
 * #2178; NULL / "" = none). It reorders the non-terminal candidates of this
 * selection only (conv_select rule 3a); one that matches no non-terminal
 * candidate falls through to the normal selection with one INFO line.
 */
uint32_t
conv_core_select_locked(struct conv_core *core,
                        uint32_t task,
                        uint32_t lane,
                        uint32_t input_kind,
                        uint32_t view_count,
                        const bool *busy_providers,
                        const char *hint,
                        struct conv_binding *out,
                        uint32_t *out_state);

/*! A provider call failed: read the provider's state, mark the row FAILED
 *  when it says so (rebinding follows on the control thread). No lock held. */
void
conv_core_call_failed(struct conv_core *core, uint32_t provider, uint32_t cap_id, xrt_result_t r);

//! Push an event (any lock state).
void
conv_core_emit(struct conv_core *core, uint64_t client_id, const struct xrt_conv_wire_event *ev);

//! The host of a provider (NULL = not loaded / hung).
struct conv_host *
conv_core_host(struct conv_core *core, uint32_t provider);


/*! A stream or job was bound, unbound, rebound or finished: the status
 *  snapshot's streams / jobs columns moved, so bump status_gen (#2029).
 *  Takes registry_mtx: call with no lock, streams_mtx or jobs_mtx held
 *  (lock order), never with registry_mtx held. */
void
conv_core_activity_changed(struct conv_core *core);


/*
 *
 * conv_stream.c.
 *
 */

xrt_result_t
conv_stream_lane_start(struct conv_core *core, uint32_t provider);

//! Stop + join a lane (streams_mtx NOT held).
void
conv_stream_lane_stop(struct conv_core *core, uint32_t provider);

//! Wake every dispatcher (no lock held).
void
conv_streams_wake(struct conv_core *core);

//! Control thread: move streams whose row went terminal (no lock held).
void
conv_streams_rebind_check(struct conv_core *core);

//! Number of live streams bound to a row (streams_mtx NOT held).
uint32_t
conv_streams_bound_count(struct conv_core *core, uint32_t provider, uint32_t cap_id, bool any_cap);

//! Live streams bound to exactly this row, composed or not (streams_mtx NOT held).
uint32_t
conv_streams_row_count(struct conv_core *core, uint32_t provider, uint32_t cap_id, bool composed);

//! Destroy every stream (core shutdown, lanes stopped).
void
conv_streams_destroy_all(struct conv_core *core);

//! A connection went away: its streams die (asynchronously, on their dispatcher).
void
conv_streams_client_gone(struct conv_core *core, uint64_t client_id);


/*
 *
 * conv_job.c.
 *
 */

xrt_result_t
conv_job_lane_start(struct conv_core *core, uint32_t provider);

void
conv_job_lane_stop(struct conv_core *core, uint32_t provider);

//! Wake every job worker (no lock held).
void
conv_jobs_wake(struct conv_core *core);

uint32_t
conv_jobs_bound_count(struct conv_core *core, uint32_t provider, uint32_t cap_id);

//! Jobs queued or running on exactly this row (jobs_mtx NOT held).
uint32_t
conv_jobs_row_count(struct conv_core *core, uint32_t provider, uint32_t cap_id, bool composed);

void
conv_jobs_destroy_all(struct conv_core *core);

#ifdef __cplusplus
}
#endif
