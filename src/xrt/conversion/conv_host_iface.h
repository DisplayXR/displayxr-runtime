// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Conversion host interface: "a place where one provider runs"
 *         (ADR-053 decision 5; roadmap conversion-providers.md §3.1, §3.2).
 *
 * The conversion core never calls a provider slot directly. It talks to a
 * conv_host, which has two implementations: the in-service shim
 * (conv_host_inproc.c, P1) and the satellite proxy (conv_host_proxy.c, P2).
 * Both look the same from here, so a provider tested in-service runs
 * unchanged in a satellite.
 *
 *  - [C] calls (describe_cap, get_generation, get_state, get_output_desc)
 *    answer from memory, on the caller's thread, concurrently with anything.
 *  - [S] calls (warm, evict, stream create / destroy / convert, job) are
 *    COMMANDS: the caller fills a conv_host_cmd and submits it; the host runs
 *    it on one of its own threads, honouring the provider's threading
 *    declaration (one call in flight by default; LANES_CONCURRENT,
 *    WARM_CONCURRENT and max_concurrent widen exactly what they name), then
 *    calls cmd->on_complete on that thread. The command and everything it
 *    points to stay owned by the caller and must stay valid until then.
 *  - Images are registered once per slot lifetime (register_image) and looked
 *    up per frame (lookup_image): a satellite proxy duplicates the handles
 *    into the host process once, an in-service host stores the image as is.
 *  - A satellite host can die and come back (roadmap §3.5, "crash → WARMING;
 *    streams survive"). Every incarnation of the host process has an EPOCH
 *    (get_epoch), bumped the moment the loss is seen and BEFORE any in-flight
 *    command is completed as failed: a caller that recorded the epoch at
 *    submit tells a crash casualty from a provider failure by comparing it.
 *    Image indices and provider stream handles are per epoch; the dispatcher
 *    re-registers and re-opens on a mismatch. The lifecycle itself is pushed
 *    to the core through conv_host::on_event (LOST, RESTORED, CRASH_LOOP).
 *    An in-service host never dies on its own: its epoch stays 0.
 *
 * Locks: a host's own mutex is a leaf. No conversion lock may be held across
 * a completion wait, and on_complete is called with no host lock held.
 *
 * @ingroup aux_conversion
 */

#pragma once

#include "xrt/xrt_conversion_plugin.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! @name Command ops (conv_host_cmd::op) @{ */
#define CONV_OP_NONE 0u
#define CONV_OP_WARM 1u           //!< warm(cap_id, is_cancelled, cancel_ctx)
#define CONV_OP_EVICT 2u          //!< evict(cap_id); exclusive
#define CONV_OP_STREAM_CREATE 3u  //!< stream_create(cap_id, stream_info) -> stream
#define CONV_OP_STREAM_DESTROY 4u //!< stream_destroy(stream)
#define CONV_OP_CONVERT 5u        //!< stream_convert(stream, frame_in, params, frame_out)
#define CONV_OP_JOB 6u            //!< job_execute(cap_id, job_in, params, job_ctl, job_out)
/*! @} */

/*! @name conv_host_cmd::blocked: why a command waited in the host queue @{ */
#define CONV_CMD_BLOCKED_BY_JOB (1u << 0)   //!< a job without LANES_CONCURRENT was in flight
#define CONV_CMD_BLOCKED_BY_WARM (1u << 1)  //!< a warm without WARM_CONCURRENT was in flight
#define CONV_CMD_BLOCKED_BY_OTHER (1u << 2) //!< another serialised call was in flight
//! Refused at once: the host process is down (backoff, update, maintenance).
//! Not a failure of the call or the capability: retry after RESTORED.
#define CONV_CMD_HOST_DOWN (1u << 3)
//! Failed because the host process died (or its channel broke) under it: the
//! call may have run partly. Streams re-open on RESTORED; a job retries once.
#define CONV_CMD_HOST_LOST (1u << 4)
/*! @} */

/*! One [S] call. Caller-owned until on_complete returns. */
struct conv_host_cmd
{
	uint32_t op;     //!< CONV_OP_*
	uint32_t cap_id; //!< WARM, EVICT, STREAM_CREATE, CONVERT (the stream's cap), JOB

	/* WARM: cooperative cancel (the host also cancels on shutdown). */
	bool (*is_cancelled)(void *ctx);
	void *cancel_ctx;

	/* STREAM_CREATE (in: stream_info, out: stream); STREAM_DESTROY / CONVERT (in: stream). */
	const struct xrt_conv_stream_info *stream_info;
	uint64_t stream;

	/* CONVERT. */
	const struct xrt_conv_frame_in *frame_in;
	const struct xrt_conv_params *params; //!< CONVERT and JOB
	struct xrt_conv_frame_out *frame_out;

	/* JOB. The host wraps job_ctl->is_cancelled (and adds its own shutdown). */
	const struct xrt_conv_job_in *job_in;
	const struct xrt_conv_job_control *job_ctl;
	struct xrt_conv_job_out *job_out;

	/* Completion, on a host thread, no host lock held. May free the command. */
	void (*on_complete)(struct conv_host_cmd *cmd);
	void *user;

	/* Filled by the host. */
	xrt_result_t result;
	uint32_t blocked; //!< CONV_CMD_BLOCKED_*
	uint64_t queued_ns, start_ns, end_ns;

	/* Host-private. */
	struct conv_host_cmd *next_;
};

/*! @name Host lifecycle events (conv_host::on_event; satellite hosts only) @{ */
//! The host process went away. detail = its exit code (conv_host_wire.h
//! CONV_HOST_EXIT_*; anything unlisted = crash). The epoch was already bumped; in-flight commands
//! complete as XRT_ERROR_IPC_FAILURE; registrations and provider streams are gone.
#define CONV_HOST_EV_LOST 1u
//! CONV_HOST_EV_LOST detail when the supervisor classified an exit as abnormal
//! although its code reads orderly (an exit 0 the service did not ask for, a
//! kill whose victim managed to return 0 / 20): the core follows the
//! supervisor's verdict, never the raw code. Counts as a crash.
#define CONV_HOST_LOST_ABNORMAL (-2)
//! A fresh host process is up and described (after the supervisor's backoff):
//! re-warm, re-open streams, re-register images.
#define CONV_HOST_EV_RESTORED 2u
//! The supervisor's crash-loop cutoff: no respawn until a registry change.
#define CONV_HOST_EV_CRASH_LOOP 3u
//! A terminal host exit (ABI reject, licence, no device, elevated, ...): detail =
//! the XRT_CONV_FAIL_* reason. No respawn until a registry change.
#define CONV_HOST_EV_FAILED 4u
/*! @} */

/*! Does host exit code @p code count toward the crash-loop budget? The
 *  codes are conv_host_wire.h's CONV_HOST_EXIT_*: 0 (orderly / idle) and 20
 *  (closed for update) do not; everything else (30 = watchdog kill included) does. */
static inline bool
conv_host_exit_is_crash(int32_t code)
{
	return code != 0 && code != 20;
}

/*! Is @p code CONV_HOST_EXIT_UPDATE (20, "closed for update", roadmap §3.6)?
 *  The provider's tasks go WARMING(updating); the respawn waits for the
 *  maintenance mutex (or a registry change), never for a timer. */
static inline bool
conv_host_exit_is_update(int32_t code)
{
	return code == 20;
}

/*! Watchdog view of a host (roadmap §5.3: every call is bracketed). */
struct conv_host_heartbeat
{
	uint32_t in_flight;       //!< [S] calls running now
	uint32_t queued;          //!< [S] calls waiting
	uint32_t oldest_op;       //!< CONV_OP_* of the longest-running call; 0 = none
	uint32_t oldest_cap_id;   //!< its capability
	uint64_t oldest_since_ns; //!< when it started (0 = none)
	uint64_t calls_total;     //!< [S] calls completed
	uint32_t exec_threads;    //!< threads currently serving this provider
	bool hung;                //!< abandoned by the watchdog
};

struct conv_host;

struct conv_host_ops
{
	/* [C]: any thread, from memory. */
	xrt_result_t (*describe_cap)(struct conv_host *h,
	                             uint32_t index,
	                             struct xrt_conv_capability *out,
	                             uint32_t *out_count);
	uint64_t (*get_generation)(struct conv_host *h);
	/*! The provider's get_provider_status, as last reported (satellite: from
	 *  the host's describe). NULL = the registry reads the iface instead. */
	bool (*get_provider_status)(struct conv_host *h, struct xrt_conv_provider_status *out);
	xrt_result_t (*get_state)(struct conv_host *h, uint32_t cap_id, struct xrt_conv_state *out);
	xrt_result_t (*get_output_desc)(
	    struct conv_host *h, uint64_t stream, uint32_t w, uint32_t hgt, struct xrt_conv_output_desc *out);

	/*! Queue one [S] command. XRT_SUCCESS = on_complete WILL be called;
	 *  anything else = it will not (hung / shutting down / bad op). */
	xrt_result_t (*submit)(struct conv_host *h, struct conv_host_cmd *cmd);

	/*! Re-read the capability flags the admission rules use (after a
	 *  re-describe). May add exec threads; never removes them. */
	void (*refresh_caps)(struct conv_host *h);

	/*! Image slots: opened once per slot lifetime. */
	xrt_result_t (*register_image)(struct conv_host *h, const struct xrt_conv_image *img, uint32_t *out_index);
	void (*unregister_image)(struct conv_host *h, uint32_t index);
	//! The image as the PROVIDER's process sees it (false = unknown index).
	bool (*lookup_image)(struct conv_host *h, uint32_t index, struct xrt_conv_image *out);

	void (*heartbeat)(struct conv_host *h, struct conv_host_heartbeat *out);

	/*! Watchdog: the provider hung. Every queued command completes with
	 *  XRT_ERROR_IPC_FAILURE (host gone), new submits are refused, the busy
	 *  exec thread is abandoned (in-service: until the service restarts).
	 *  A satellite proxy instead kills the host process (exit 30); the loss
	 *  then follows the crash path (CONV_HOST_EV_LOST, respawn). Never raises
	 *  an event synchronously (the caller may hold core locks). */
	void (*abandon)(struct conv_host *h);

	/*! The current host-process incarnation. NULL = in-service (always 0). */
	uint32_t (*get_epoch)(struct conv_host *h);

	/*! [any thread, never blocks] The host process is down and will not come
	 *  back until RESTORED (backoff, closed for update, maintenance mutex held,
	 *  failed): submit refuses at once with CONV_CMD_HOST_DOWN. Callers keep
	 *  their frames pending instead of failing them (no unbounded waits,
	 *  workspace-stability rule). NULL = never (in-service). */
	bool (*is_down)(struct conv_host *h);

	/*! Orderly shutdown: cancel (is_cancelled turns true), drain, destroy the
	 *  provider instance, join. A hung host is leaked instead. */
	void (*destroy)(struct conv_host **h_ptr);
};

/*! Base of every host. */
struct conv_host
{
	const struct conv_host_ops *ops;
	char provider_id[64];
	uint32_t hosting; //!< CONV_HOSTING_* (conv_registry.h)

	/*! Lifecycle listener (CONV_HOST_EV_*), installed by the core before the
	 *  host's first command. Called on a host thread with no host lock held;
	 *  the listener only queues (it must not call back into the host). */
	void (*on_event)(void *ud, struct conv_host *h, uint32_t ev, int32_t detail);
	void *event_ud;
};

//! Raise a lifecycle event (host implementations; no host lock held).
static inline void
conv_host_raise_event(struct conv_host *h, uint32_t ev, int32_t detail)
{
	if (h->on_event != NULL) {
		h->on_event(h->event_ud, h, ev, detail);
	}
}

static inline xrt_result_t
conv_host_describe_cap(struct conv_host *h, uint32_t index, struct xrt_conv_capability *out, uint32_t *out_count)
{
	return h->ops->describe_cap(h, index, out, out_count);
}

static inline uint64_t
conv_host_get_generation(struct conv_host *h)
{
	return h->ops->get_generation(h);
}

static inline xrt_result_t
conv_host_get_state(struct conv_host *h, uint32_t cap_id, struct xrt_conv_state *out)
{
	return h->ops->get_state(h, cap_id, out);
}

static inline xrt_result_t
conv_host_get_output_desc(
    struct conv_host *h, uint64_t stream, uint32_t w, uint32_t hgt, struct xrt_conv_output_desc *out)
{
	return h->ops->get_output_desc(h, stream, w, hgt, out);
}

static inline xrt_result_t
conv_host_submit(struct conv_host *h, struct conv_host_cmd *cmd)
{
	return h->ops->submit(h, cmd);
}

static inline void
conv_host_refresh_caps(struct conv_host *h)
{
	h->ops->refresh_caps(h);
}

static inline xrt_result_t
conv_host_register_image(struct conv_host *h, const struct xrt_conv_image *img, uint32_t *out_index)
{
	return h->ops->register_image(h, img, out_index);
}

static inline void
conv_host_unregister_image(struct conv_host *h, uint32_t index)
{
	h->ops->unregister_image(h, index);
}

static inline bool
conv_host_lookup_image(struct conv_host *h, uint32_t index, struct xrt_conv_image *out)
{
	return h->ops->lookup_image(h, index, out);
}

static inline void
conv_host_get_heartbeat(struct conv_host *h, struct conv_host_heartbeat *out)
{
	h->ops->heartbeat(h, out);
}

static inline void
conv_host_abandon(struct conv_host *h)
{
	h->ops->abandon(h);
}

static inline uint32_t
conv_host_get_epoch(struct conv_host *h)
{
	return h->ops->get_epoch != NULL ? h->ops->get_epoch(h) : 0u;
}

static inline bool
conv_host_is_down(struct conv_host *h)
{
	return h != NULL && h->ops->is_down != NULL && h->ops->is_down(h);
}

static inline void
conv_host_destroy(struct conv_host **h_ptr)
{
	if (h_ptr != NULL && *h_ptr != NULL) {
		(*h_ptr)->ops->destroy(h_ptr);
	}
}

/*!
 * Submit @p cmd and block the calling thread until it completed. The command's
 * on_complete / user are overwritten. Returns the command's result, or the
 * submit error. Only for runtime-owned worker threads (dispatcher, job worker,
 * control): never a render / IPC thread, never with a conversion lock held.
 * There is no timeout: a hung provider is detected by the watchdog
 * (heartbeat), which abandons the host; the waiting worker stays with it.
 */
xrt_result_t
conv_host_call(struct conv_host *h, struct conv_host_cmd *cmd);

#ifdef __cplusplus
}
#endif
