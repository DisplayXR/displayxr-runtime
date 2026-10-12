// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Conversion readiness: the per-(provider, capability) state machine
 *         and the spec-3 legacy mapping (ADR-053 decision 6).
 *
 * Pure C11: no threads, no clocks, no logging, no I/O. Every event carries the
 * caller's monotonic time (os_monotonic_get_ns() in the service; anything in a
 * test). The caller logs one WARN per transition (CONV_RD_CHANGED) and bumps
 * its own conv_gen / status_gen counters; this module only computes states.
 *
 * Spec: docs/specs/runtime/conversion-provider-discovery.md §4.2 (backoff),
 * §4.3 (readiness table, retry triggers, spec-3 mapping); roadmap
 * docs/roadmap/conversion-providers.md §2.5, §7.3.
 *
 * One machine per (provider, capability) row of the capability table. A
 * provider whose task is served by a single capability (the usual case) has
 * exactly one machine per (provider, task), which is what the roadmap names.
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

/*
 *
 * Reasons. XRT_CONV_FAIL_* (0..9) plus core-local values for the ABSENT
 * sub-reasons the provider ABI has no code for. Core-local values never cross
 * the provider ABI; they are diagnostics (CLI, status snapshot).
 *
 */

#define CONV_REASON_LOCAL_BASE 0x100u
#define CONV_REASON_POLICY (CONV_REASON_LOCAL_BASE + 1u)         //!< ABSENT(policy): download forbidden
#define CONV_REASON_BINARY_MISSING (CONV_REASON_LOCAL_BASE + 2u) //!< ABSENT: provider binary not found
#define CONV_REASON_ABI_REJECTED (CONV_REASON_LOCAL_BASE + 3u)   //!< ABSENT: ABI major mismatch / negotiate failed
#define CONV_REASON_NOT_DECLARED (CONV_REASON_LOCAL_BASE + 4u)   //!< ABSENT: the provider no longer declares it

/*
 *
 * Timing constants (spec §4.2 / §4.3).
 *
 */

#define CONV_NS_PER_S 1000000000ull
#define CONV_CRASH_LOOP_COUNT 5u                           //!< abnormal exits ...
#define CONV_CRASH_LOOP_WINDOW_NS (600ull * CONV_NS_PER_S) //!< ... within 10 min
#define CONV_CRASH_LOOP_RETRY_NS (1800ull * CONV_NS_PER_S) //!< retried 30 min after the cutoff
#define CONV_CREATE_RETRY_AFTER_NS (30ull * CONV_NS_PER_S) //!< create for an unserved task
#define CONV_BACKOFF_FIRST_NS (15ull * CONV_NS_PER_S)      //!< retryable failure: 15 s ...
#define CONV_BACKOFF_MAX_NS (600ull * CONV_NS_PER_S)       //!< ... doubling to 10 min (spec §4.2)

/*
 *
 * Events.
 *
 */

enum conv_readiness_event_type
{
	CONV_EV_NONE = 0,
	//! describe_cap declared the capability (live, or from the describe cache).
	CONV_EV_DESCRIBED,
	//! The provider could not be described: binary missing, ABI rejected, or the
	//! capability vanished from a re-describe. reason = CONV_REASON_* / XRT_CONV_FAIL_*.
	CONV_EV_DESCRIBE_FAILED,
	//! A warm was requested (prewarm, `conv warm`). flags = CONV_EVF_*. From
	//! FAILED this is the explicit-warm retry trigger.
	CONV_EV_WARM_REQUESTED,
	//! A stream / job create selected this row (warms it like a request). From
	//! FAILED it is the "create for an unserved task" trigger: it retries only
	//! when the failure is older than 30 s. The caller sends it to the failed
	//! rows only when selection found no non-terminal candidate.
	CONV_EV_CREATE_REQUESTED,
	CONV_EV_WARM_PROGRESS, //!< progress
	CONV_EV_WARM_OK,       //!< provider reports READY
	CONV_EV_WARM_FAILED,   //!< reason + retryable
	CONV_EV_DOWNLOAD_STARTED,
	CONV_EV_DOWNLOAD_PROGRESS, //!< progress
	CONV_EV_DOWNLOAD_DONE,
	CONV_EV_DOWNLOAD_FAILED, //!< reason (NETWORK / MODEL) + retryable
	//! Idle past keep-alive. flags & CONV_EVF_BOUND: a stream or job is bound,
	//! so the event is refused.
	CONV_EV_EVICTED,
	CONV_EV_HOST_CRASH,       //!< abnormal host exit or watchdog kill
	CONV_EV_CRASH_LOOP,       //!< supervisor-declared cutoff (the FSM also counts on its own)
	CONV_EV_DISABLE,          //!< `conv disable`, Enabled = 0
	CONV_EV_ENABLE,           //!< `conv enable`, Enabled = 1
	CONV_EV_REGISTRY_CHANGED, //!< the provider's row / manifest changed (retries FAILED + ABSENT)
	CONV_EV_BUDGET_CHANGED,   //!< adapter memory budget changed (retries FAILED(RESOURCES))
	CONV_EV_TIMER,            //!< time passed; fires the backoff / crash-loop retry when due
};

/*! @name Event flags (conv_readiness_event::flags) @{ */
#define CONV_EVF_MODEL_MISSING (1u << 0)      //!< warm: model blobs not in the store
#define CONV_EVF_DOWNLOAD_FORBIDDEN (1u << 1) //!< warm: policy forbids the download
#define CONV_EVF_EXCEEDS_MEMORY (1u << 2)     //!< warm: est_vram_mb exceeds the adapter, nothing evictable
#define CONV_EVF_BOUND (1u << 3)              //!< evict: a stream or job is bound
/*! @} */

struct conv_readiness_event
{
	uint32_t type;   //!< conv_readiness_event_type
	uint32_t flags;  //!< CONV_EVF_*
	uint32_t reason; //!< XRT_CONV_FAIL_* / CONV_REASON_*
	bool retryable;  //!< failures: a later warm may succeed
	float progress;  //!< [0,1]; < 0 indeterminate
	uint64_t now_ns; //!< caller's monotonic clock
};

/*
 *
 * Machine.
 *
 */

/*! One (provider, capability) machine. Zero-init == UNKNOWN (not yet described). */
struct conv_readiness
{
	uint32_t state;           //!< XRT_CONV_STATE_*
	uint32_t reason;          //!< XRT_CONV_FAIL_* / CONV_REASON_* while FAILED / ABSENT, else 0
	bool retryable;           //!< FAILED only
	bool restarting;          //!< WARMING after a host crash
	uint32_t restart_attempt; //!< n in WARMING(restarting, n); reset at READY
	float progress;           //!< WARMING / DOWNLOADING

	uint64_t generation;   //!< bumped on every state change
	uint64_t entered_ns;   //!< when the current state was entered
	uint64_t failed_ns;    //!< when FAILED was entered (0 = never)
	uint64_t retry_due_ns; //!< backoff / crash-loop retry deadline; 0 = none
	uint64_t backoff_ns;   //!< current backoff step (0 = next failure starts at 15 s)

	uint64_t crash_ns[CONV_CRASH_LOOP_COUNT]; //!< ring of the last abnormal exits
	uint32_t crash_count;                     //!< total recorded (ring index = count % N)
};

/*! @name conv_readiness_apply() result bits @{ */
#define CONV_RD_CHANGED (1u << 0)        //!< state (or reason) changed: log one WARN, push an event
#define CONV_RD_PROGRESS (1u << 1)       //!< progress moved, state unchanged (rate-limit events)
#define CONV_RD_ACT_WARM (1u << 2)       //!< caller should (re)issue warm with fresh CONV_EVF_* flags
#define CONV_RD_ACT_REDESCRIBE (1u << 3) //!< caller should re-describe the provider
#define CONV_RD_IGNORED (1u << 4)        //!< event not applicable in this state
/*! @} */

void
conv_readiness_init(struct conv_readiness *r);

/*! Apply one event. Returns CONV_RD_* bits. */
uint32_t
conv_readiness_apply(struct conv_readiness *r, const struct conv_readiness_event *ev);

/*! Convenience: CONV_EV_TIMER at @p now_ns. */
uint32_t
conv_readiness_tick(struct conv_readiness *r, uint64_t now_ns);

/*! Next time conv_readiness_tick() can change something; 0 = none pending. */
uint64_t
conv_readiness_next_deadline(const struct conv_readiness *r);

/*! INSTALLED / DOWNLOADING / WARMING / EVICTED: selection waits for it. */
bool
conv_state_is_waited(uint32_t state);

/*! READY or waited-for (selection keeps it). Everything else falls through. */
bool
conv_state_is_selectable(uint32_t state);

/*! ABSENT / INCOMPATIBLE / UNLICENSED / FAILED / DISABLED. UNKNOWN is NOT
 *  terminal ("not yet asked is never terminal"). */
bool
conv_state_is_terminal(uint32_t state);

/*! Known XRT_CONV_STATE_* value (0..10). */
bool
conv_state_in_range(uint32_t state);

const char *
conv_state_name(uint32_t state);

const char *
conv_reason_name(uint32_t reason);

/*
 *
 * Spec-3 legacy mapping (roadmap §7.3, spec §4.3).
 *
 */

/*! Values equal XrLiftStateDXR (XR_DXR_lift.h, frozen v3). */
#define CONV_V3_STATE_UNAVAILABLE 0u
#define CONV_V3_STATE_ACTIVATING 1u
#define CONV_V3_STATE_READY 2u

/*!
 * XrLiftPropertiesDXR::state for a v3 caller.
 *
 * @param task_states        states of the tasks backing the v3 modes (one per
 *                           backing task; may be empty, e.g. before any describe)
 * @param count              number of entries
 * @param discovery_settled  every registered provider is described or terminal
 *                           (conv_discovery_settled())
 *
 * READY if any backing task is READY; else ACTIVATING while discovery is
 * unsettled or any backing task is non-terminal (UNKNOWN counts as
 * non-terminal); else UNAVAILABLE. Shipping browsers never re-poll
 * UNAVAILABLE, so it is returned only when nothing will ever become READY
 * without a new trigger.
 */
uint32_t
conv_legacy_v3_state(const uint32_t *task_states, uint32_t count, bool discovery_settled);

/*!
 * Discovery has settled when every registered provider is either described
 * (at least once, live or from the cache) or terminal (binary missing, ABI
 * rejected, disabled).
 */
bool
conv_discovery_settled(const bool *provider_described, const bool *provider_terminal, uint32_t count);

/*! @name v3 mode bits (equal XR_LIFT_MODE_*_BIT_DXR) @{ */
#define CONV_V3_MODE_DEPTH 0x1u
#define CONV_V3_MODE_SBS 0x2u
#define CONV_V3_MODE_NVIEW 0x4u
#define CONV_V3_MODE_GAUSSIANS 0x8u
/*! @} */

/*!
 * The task backing one v3 mode (roadmap §7.3): DEPTH → DEPTH_VIDEO (photo
 * content → DEPTH_MONO), SBS / NVIEW → VIEWS_MONO, GAUSSIANS → SPLATS_MONO.
 * Returns XRT_CONV_TASK_NONE for an unknown or multi-bit mode.
 */
uint32_t
conv_legacy_mode_task(uint32_t v3_mode_bit, bool photo_content);

#ifdef __cplusplus
}
#endif
