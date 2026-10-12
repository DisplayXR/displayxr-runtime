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
	uint32_t reg_idx[CONV_STREAM_MAX_SUBMIT_IMAGES];
	struct conv_frame_submit frame; //!< the committed frame's inputs
};

struct conv_ring_slot
{
	/* Targets (runtime-allocated, exported to clients). */
	struct xrt_conv_image targets[CONV_RING_TARGETS]; //!< views, depth, confidence, disparity
	struct conv_host *reg_host;
	uint32_t reg_idx[CONV_RING_TARGETS];
	uint32_t in_w, in_h; //!< input extent the targets were sized for (0 = none)
	uint32_t sized_key;  //!< outputs / views the targets were sized for
	uint64_t alloc_gen;  //!< bumps on every (re)allocation

	/* Published result. */
	uint32_t written;
	struct xrt_conv_views_info views_info;
	struct xrt_conv_depth_info depth_info;
	float active[4];
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

	/* Temporal state. */
	bool reset_pending;         //!< FRAME_RESET on the next conversion
	bool discontinuity_pending; //!< `discontinuity` on the next result
	struct conv_policy_view_state vstate;
	struct conv_observer observer;
	bool has_observer;
	struct xrt_conv_depth_info last_depth;
	bool has_last_depth;

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

	struct xrt_conv_job_in in;
	void *owned[XRT_CONV_MAX_FRAME_IMAGES]; //!< CPU input copies
	struct xrt_conv_params params;
	uint32_t requested_outputs;
	struct conv_stage stage;
	struct xrt_conv_job_out out;
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
	bool busy;
	bool cancel;
};

struct conv_host_slot
{
	struct conv_host *host; //!< NULL = not loaded (cache-only rows)
	struct conv_stream_lane lane;
	struct conv_job_lane jlane;
	struct conv_warm_op warms[XRT_CONV_MAX_CAPABILITIES];
	bool hung;
	uint64_t generation_seen;
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

	/* streams_mtx: the stream table (held between commands only). */
	struct os_mutex streams_mtx;
	struct conv_stream *streams;
	uint64_t next_stream_id;

	/* jobs_mtx: the job table. */
	struct os_mutex jobs_mtx;
	struct conv_job *jobs;
	uint64_t next_job_id;

	struct conv_eyes_slot eyes[CONV_CORE_MAX_DPS];
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
 */
uint32_t
conv_core_select_locked(struct conv_core *core,
                        uint32_t task,
                        uint32_t lane,
                        uint32_t input_kind,
                        uint32_t view_count,
                        const bool *busy_providers,
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

//! Read a DP's eyes snapshot.
bool
conv_core_read_eyes(struct conv_core *core, int32_t dp, uint64_t now_ns, struct conv_eyes *out);


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

void
conv_jobs_destroy_all(struct conv_core *core);

#ifdef __cplusplus
}
#endif
