// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Conversion readiness state machine + spec-3 legacy mapping.
 * @ingroup aux_conversion
 */

#include "conv_readiness.h"

#include <string.h>

/*
 *
 * Helpers.
 *
 */

static uint32_t
enter(struct conv_readiness *r, uint32_t state, uint32_t reason, uint64_t now_ns)
{
	if (r->state == state && r->reason == reason) {
		return 0;
	}
	r->state = state;
	r->reason = reason;
	r->entered_ns = now_ns;
	r->generation++;
	if (state != XRT_CONV_STATE_FAILED) {
		r->retryable = false;
	}
	if (state != XRT_CONV_STATE_WARMING) {
		r->restarting = false;
	}
	if (state != XRT_CONV_STATE_WARMING && state != XRT_CONV_STATE_DOWNLOADING) {
		r->progress = state == XRT_CONV_STATE_READY ? 1.0f : 0.0f;
	} else {
		r->progress = 0.0f;
	}
	r->report_progress = r->progress;
	r->report_ns = now_ns;
	return CONV_RD_CHANGED;
}

//! Progress moved: always CONV_RD_PROGRESS; CONV_RD_PROGRESS_DUE at 2 Hz / 5 % (and at 100 %).
static uint32_t
progress_moved(struct conv_readiness *r, float p, uint64_t now_ns)
{
	r->progress = p;
	const float d = p > r->report_progress ? p - r->report_progress : r->report_progress - p;
	const bool done = p >= 1.0f && r->report_progress < 1.0f;
	const bool due = d >= CONV_PROGRESS_MIN_STEP && now_ns - r->report_ns >= CONV_PROGRESS_MIN_NS;
	if (!done && !due) {
		return CONV_RD_PROGRESS;
	}
	r->report_progress = p;
	r->report_ns = now_ns;
	return CONV_RD_PROGRESS | CONV_RD_PROGRESS_DUE;
}

//! Network / device loss are retryable by nature, whatever the caller said.
static bool
reason_forced_retryable(uint32_t reason)
{
	return reason == XRT_CONV_FAIL_NETWORK || reason == XRT_CONV_FAIL_DEVICE_LOST;
}

static uint32_t
fail(struct conv_readiness *r, uint32_t reason, bool retryable, uint64_t now_ns)
{
	if (reason == XRT_CONV_FAIL_NONE) {
		reason = XRT_CONV_FAIL_LOAD;
	}
	uint32_t res = enter(r, XRT_CONV_STATE_FAILED, reason, now_ns);
	r->failed_ns = now_ns;
	r->retry_due_ns = 0;

	if (reason == XRT_CONV_FAIL_CRASH_LOOP) {
		// Retried 30 min after the cutoff (spec §4.3).
		r->retryable = true;
		r->retry_due_ns = now_ns + CONV_CRASH_LOOP_RETRY_NS;
	} else if (reason == XRT_CONV_FAIL_RESOURCES) {
		// Terminal until the budget changes: no timer.
		r->retryable = false;
	} else {
		r->retryable = retryable || reason_forced_retryable(reason);
		if (r->retryable) {
			// 15 s, doubling to 10 min (spec §4.2).
			if (r->backoff_ns == 0) {
				r->backoff_ns = CONV_BACKOFF_FIRST_NS;
			} else {
				r->backoff_ns *= 2;
				if (r->backoff_ns > CONV_BACKOFF_MAX_NS) {
					r->backoff_ns = CONV_BACKOFF_MAX_NS;
				}
			}
			r->retry_due_ns = now_ns + r->backoff_ns;
		}
	}
	return res;
}

static void
clear_crashes(struct conv_readiness *r)
{
	memset(r->crash_ns, 0, sizeof(r->crash_ns));
	r->crash_count = 0;
}

//! Record an abnormal exit; true when it completes a crash loop.
static bool
record_crash(struct conv_readiness *r, uint64_t now_ns)
{
	r->crash_ns[r->crash_count % CONV_CRASH_LOOP_COUNT] = now_ns;
	r->crash_count++;
	if (r->crash_count < CONV_CRASH_LOOP_COUNT) {
		return false;
	}
	// The oldest of the last N exits is the slot we will overwrite next.
	uint64_t oldest = r->crash_ns[r->crash_count % CONV_CRASH_LOOP_COUNT];
	return now_ns - oldest <= CONV_CRASH_LOOP_WINDOW_NS;
}

//! INSTALLED / EVICTED + a warm (request or create).
static uint32_t
warm_from_cold(struct conv_readiness *r, const struct conv_readiness_event *ev)
{
	if ((ev->flags & CONV_EVF_EXCEEDS_MEMORY) != 0) {
		return fail(r, XRT_CONV_FAIL_RESOURCES, false, ev->now_ns);
	}
	if ((ev->flags & CONV_EVF_MODEL_MISSING) != 0) {
		if ((ev->flags & CONV_EVF_DOWNLOAD_FORBIDDEN) != 0) {
			return enter(r, XRT_CONV_STATE_ABSENT, CONV_REASON_POLICY, ev->now_ns);
		}
		return enter(r, XRT_CONV_STATE_DOWNLOADING, 0, ev->now_ns);
	}
	return enter(r, XRT_CONV_STATE_WARMING, 0, ev->now_ns);
}

//! FAILED → INSTALLED (the retry itself). Keeps backoff_ns so the next failure doubles.
static uint32_t
retry(struct conv_readiness *r, uint64_t now_ns)
{
	if (r->reason == XRT_CONV_FAIL_CRASH_LOOP) {
		clear_crashes(r);
		r->backoff_ns = 0;
	}
	r->retry_due_ns = 0;
	return enter(r, XRT_CONV_STATE_INSTALLED, 0, now_ns);
}

static bool
is_cold(uint32_t state)
{
	return state == XRT_CONV_STATE_INSTALLED || state == XRT_CONV_STATE_EVICTED;
}

/*
 *
 * API.
 *
 */

void
conv_readiness_init(struct conv_readiness *r)
{
	memset(r, 0, sizeof(*r));
	r->state = XRT_CONV_STATE_UNKNOWN;
}

uint32_t
conv_readiness_apply(struct conv_readiness *r, const struct conv_readiness_event *ev)
{
	const uint64_t now = ev->now_ns;
	const uint32_t s = r->state;

	switch (ev->type) {
	case CONV_EV_DESCRIBED:
		if (s == XRT_CONV_STATE_UNKNOWN || (s == XRT_CONV_STATE_ABSENT && r->reason != CONV_REASON_POLICY)) {
			return enter(r, XRT_CONV_STATE_INSTALLED, 0, now);
		}
		return CONV_RD_IGNORED;

	case CONV_EV_DESCRIBE_FAILED:
		if (s == XRT_CONV_STATE_DISABLED) {
			return CONV_RD_IGNORED;
		}
		r->retry_due_ns = 0;
		return enter(r, XRT_CONV_STATE_ABSENT, ev->reason != 0 ? ev->reason : CONV_REASON_BINARY_MISSING, now);

	case CONV_EV_WARM_REQUESTED:
		if (is_cold(s)) {
			return warm_from_cold(r, ev);
		}
		if (s == XRT_CONV_STATE_FAILED && r->reason != XRT_CONV_FAIL_RESOURCES) {
			// Explicit warm: a retry trigger for every FAILED reason except
			// RESOURCES (terminal until the budget changes).
			retry(r, now);
			warm_from_cold(r, ev);
			return CONV_RD_CHANGED;
		}
		return CONV_RD_IGNORED;

	case CONV_EV_CREATE_REQUESTED:
		if (is_cold(s)) {
			return warm_from_cold(r, ev);
		}
		if (s == XRT_CONV_STATE_FAILED && r->reason != XRT_CONV_FAIL_RESOURCES &&
		    r->reason != XRT_CONV_FAIL_CRASH_LOOP && now - r->failed_ns > CONV_CREATE_RETRY_AFTER_NS) {
			retry(r, now);
			warm_from_cold(r, ev);
			return CONV_RD_CHANGED;
		}
		return CONV_RD_IGNORED;

	case CONV_EV_WARM_PROGRESS:
		if (s == XRT_CONV_STATE_WARMING) {
			return progress_moved(r, ev->progress, now);
		}
		return CONV_RD_IGNORED;

	case CONV_EV_WARM_OK:
		// From DOWNLOADING too: a small model downloads and loads between two polls.
		if (s == XRT_CONV_STATE_WARMING || s == XRT_CONV_STATE_DOWNLOADING || is_cold(s)) {
			uint32_t res = enter(r, XRT_CONV_STATE_READY, 0, now);
			r->restart_attempt = 0;
			r->backoff_ns = 0;
			r->retry_due_ns = 0;
			return res;
		}
		return CONV_RD_IGNORED;

	case CONV_EV_WARM_FAILED:
		if (s == XRT_CONV_STATE_WARMING || s == XRT_CONV_STATE_DOWNLOADING) {
			return fail(r, ev->reason, ev->retryable, now);
		}
		return CONV_RD_IGNORED;

	case CONV_EV_DOWNLOAD_STARTED:
		// From WARMING too: the provider learns at warm-up that its model is missing.
		if (is_cold(s) || s == XRT_CONV_STATE_WARMING) {
			return enter(r, XRT_CONV_STATE_DOWNLOADING, 0, now);
		}
		return CONV_RD_IGNORED;

	case CONV_EV_DOWNLOAD_PROGRESS:
		if (s == XRT_CONV_STATE_DOWNLOADING) {
			return progress_moved(r, ev->progress, now);
		}
		return CONV_RD_IGNORED;

	case CONV_EV_MODEL_ABSENT:
		if (is_cold(s) || s == XRT_CONV_STATE_WARMING || s == XRT_CONV_STATE_DOWNLOADING) {
			return enter(r, XRT_CONV_STATE_ABSENT, ev->reason != 0 ? ev->reason : XRT_CONV_FAIL_MODEL, now);
		}
		return CONV_RD_IGNORED;

	case CONV_EV_DOWNLOAD_DONE:
		if (s == XRT_CONV_STATE_DOWNLOADING) {
			return enter(r, XRT_CONV_STATE_WARMING, 0, now);
		}
		return CONV_RD_IGNORED;

	case CONV_EV_DOWNLOAD_FAILED:
		if (s == XRT_CONV_STATE_DOWNLOADING) {
			return fail(r, ev->reason != 0 ? ev->reason : XRT_CONV_FAIL_NETWORK, ev->retryable, now);
		}
		return CONV_RD_IGNORED;

	case CONV_EV_EVICTED:
		if (s == XRT_CONV_STATE_READY && (ev->flags & CONV_EVF_BOUND) == 0) {
			return enter(r, XRT_CONV_STATE_EVICTED, 0, now);
		}
		return CONV_RD_IGNORED;

	case CONV_EV_HOST_CRASH: {
		if (s == XRT_CONV_STATE_DISABLED) {
			return CONV_RD_IGNORED;
		}
		if (record_crash(r, now)) {
			return fail(r, XRT_CONV_FAIL_CRASH_LOOP, true, now);
		}
		if (s == XRT_CONV_STATE_READY || s == XRT_CONV_STATE_WARMING) {
			if (enter(r, XRT_CONV_STATE_WARMING, 0, now) == 0) {
				// WARMING → WARMING(restarting, n+1) is a reportable transition too.
				r->generation++;
				r->entered_ns = now;
			}
			r->restarting = true;
			r->restart_attempt++;
			return CONV_RD_CHANGED | CONV_RD_ACT_WARM;
		}
		return CONV_RD_IGNORED;
	}

	case CONV_EV_HOST_UPDATING:
		// Closed for update (roadmap §3.6): not a crash, nothing recorded. The
		// tasks report WARMING(updating) until the respawned host re-warms them.
		if (s == XRT_CONV_STATE_READY || s == XRT_CONV_STATE_WARMING) {
			const uint32_t res = enter(r, XRT_CONV_STATE_WARMING, CONV_REASON_UPDATING, now);
			r->restarting = false;
			return res | CONV_RD_ACT_WARM;
		}
		return CONV_RD_IGNORED;

	case CONV_EV_CRASH_LOOP:
		if (s == XRT_CONV_STATE_DISABLED) {
			return CONV_RD_IGNORED;
		}
		return fail(r, XRT_CONV_FAIL_CRASH_LOOP, true, now);

	case CONV_EV_DISABLE:
		r->retry_due_ns = 0;
		return enter(r, XRT_CONV_STATE_DISABLED, 0, now) | (s == XRT_CONV_STATE_DISABLED ? CONV_RD_IGNORED : 0);

	case CONV_EV_ENABLE:
		if (s == XRT_CONV_STATE_DISABLED) {
			return enter(r, XRT_CONV_STATE_INSTALLED, 0, now) | CONV_RD_ACT_REDESCRIBE;
		}
		return CONV_RD_IGNORED;

	case CONV_EV_REGISTRY_CHANGED:
		if (s == XRT_CONV_STATE_FAILED || s == XRT_CONV_STATE_ABSENT) {
			if (s == XRT_CONV_STATE_FAILED) {
				retry(r, now);
			} else {
				enter(r, XRT_CONV_STATE_INSTALLED, 0, now);
			}
			clear_crashes(r);
			r->backoff_ns = 0;
			return CONV_RD_CHANGED | CONV_RD_ACT_REDESCRIBE;
		}
		if (s == XRT_CONV_STATE_UNKNOWN) {
			return CONV_RD_ACT_REDESCRIBE;
		}
		return CONV_RD_IGNORED;

	case CONV_EV_BUDGET_CHANGED:
		if (s == XRT_CONV_STATE_FAILED && r->reason == XRT_CONV_FAIL_RESOURCES) {
			return retry(r, now) | CONV_RD_ACT_WARM;
		}
		return CONV_RD_IGNORED;

	case CONV_EV_RUNTIME_FAILED:
		if (s == XRT_CONV_STATE_READY || s == XRT_CONV_STATE_WARMING || s == XRT_CONV_STATE_EVICTED) {
			return fail(r, ev->reason, ev->retryable, now);
		}
		return CONV_RD_IGNORED;

	case CONV_EV_TIMER:
		if (s == XRT_CONV_STATE_FAILED && r->retry_due_ns != 0 && now >= r->retry_due_ns) {
			return retry(r, now) | CONV_RD_ACT_WARM;
		}
		return CONV_RD_IGNORED;

	case CONV_EV_DECLARED_ABSENT:
		// From any live or retrying state; DISABLED (configuration) and the
		// other terminal states keep their own reason.
		if (conv_state_is_waited(s) || s == XRT_CONV_STATE_READY || s == XRT_CONV_STATE_FAILED ||
		    s == XRT_CONV_STATE_UNKNOWN || (s == XRT_CONV_STATE_ABSENT && r->reason != CONV_REASON_POLICY)) {
			r->retry_due_ns = 0;
			r->backoff_ns = 0;
			return enter(r, XRT_CONV_STATE_ABSENT, ev->reason != 0 ? ev->reason : CONV_REASON_POLICY, now);
		}
		return CONV_RD_IGNORED;

	default: return CONV_RD_IGNORED;
	}
}

uint32_t
conv_readiness_tick(struct conv_readiness *r, uint64_t now_ns)
{
	struct conv_readiness_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = CONV_EV_TIMER;
	ev.now_ns = now_ns;
	return conv_readiness_apply(r, &ev);
}

uint64_t
conv_readiness_next_deadline(const struct conv_readiness *r)
{
	return r->state == XRT_CONV_STATE_FAILED ? r->retry_due_ns : 0;
}

bool
conv_state_is_waited(uint32_t state)
{
	return state == XRT_CONV_STATE_INSTALLED || state == XRT_CONV_STATE_DOWNLOADING ||
	       state == XRT_CONV_STATE_WARMING || state == XRT_CONV_STATE_EVICTED;
}

bool
conv_state_is_selectable(uint32_t state)
{
	return state == XRT_CONV_STATE_READY || conv_state_is_waited(state);
}

bool
conv_state_is_terminal(uint32_t state)
{
	switch (state) {
	case XRT_CONV_STATE_ABSENT:
	case XRT_CONV_STATE_INCOMPATIBLE:
	case XRT_CONV_STATE_UNLICENSED:
	case XRT_CONV_STATE_FAILED:
	case XRT_CONV_STATE_DISABLED: return true;
	default: return false;
	}
}

bool
conv_state_in_range(uint32_t state)
{
	return state <= XRT_CONV_STATE_EVICTED;
}

const char *
conv_state_name(uint32_t state)
{
	switch (state) {
	case XRT_CONV_STATE_UNKNOWN: return "unknown";
	case XRT_CONV_STATE_READY: return "ready";
	case XRT_CONV_STATE_WARMING: return "warming";
	case XRT_CONV_STATE_ABSENT: return "absent";
	case XRT_CONV_STATE_INCOMPATIBLE: return "incompatible";
	case XRT_CONV_STATE_UNLICENSED: return "unlicensed";
	case XRT_CONV_STATE_FAILED: return "failed";
	case XRT_CONV_STATE_DISABLED: return "disabled";
	case XRT_CONV_STATE_INSTALLED: return "installed";
	case XRT_CONV_STATE_DOWNLOADING: return "downloading";
	case XRT_CONV_STATE_EVICTED: return "evicted";
	default: return "invalid";
	}
}

const char *
conv_reason_name(uint32_t reason)
{
	switch (reason) {
	case XRT_CONV_FAIL_NONE: return "none";
	case XRT_CONV_FAIL_LICENCE: return "licence";
	case XRT_CONV_FAIL_LOAD: return "load";
	case XRT_CONV_FAIL_PLATFORM: return "platform";
	case XRT_CONV_FAIL_NETWORK: return "network";
	case XRT_CONV_FAIL_VERSION: return "version";
	case XRT_CONV_FAIL_RESOURCES: return "resources";
	case XRT_CONV_FAIL_MODEL: return "model";
	case XRT_CONV_FAIL_DEVICE_LOST: return "device_lost";
	case XRT_CONV_FAIL_CRASH_LOOP: return "crash_loop";
	case XRT_CONV_FAIL_POLICY: return "policy";
	case XRT_CONV_FAIL_INPUT: return "input";
	case CONV_REASON_POLICY: return "policy";
	case CONV_REASON_BINARY_MISSING: return "binary_missing";
	case CONV_REASON_ABI_REJECTED: return "abi_rejected";
	case CONV_REASON_NOT_DECLARED: return "not_declared";
	case CONV_REASON_UPDATING: return "updating";
	default: return "unknown";
	}
}

uint32_t
conv_model_absent_reason(uint32_t provider_failure_reason)
{
	return provider_failure_reason == XRT_CONV_FAIL_POLICY ? CONV_REASON_POLICY : XRT_CONV_FAIL_MODEL;
}

/*
 *
 * Spec-3 mapping.
 *
 */

uint32_t
conv_legacy_v3_state(const uint32_t *task_states, uint32_t count, bool discovery_settled)
{
	bool any_non_terminal = false;
	for (uint32_t i = 0; i < count; i++) {
		if (task_states[i] == XRT_CONV_STATE_READY) {
			return CONV_V3_STATE_READY;
		}
		if (!conv_state_is_terminal(task_states[i])) {
			any_non_terminal = true; // waited-for, or UNKNOWN (never terminal)
		}
	}
	if (!discovery_settled || any_non_terminal) {
		return CONV_V3_STATE_ACTIVATING;
	}
	return CONV_V3_STATE_UNAVAILABLE;
}

bool
conv_discovery_settled(const bool *provider_described, const bool *provider_terminal, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++) {
		if (!provider_described[i] && !provider_terminal[i]) {
			return false;
		}
	}
	return true;
}

uint32_t
conv_legacy_mode_task(uint32_t v3_mode_bit, bool photo_content)
{
	switch (v3_mode_bit) {
	case CONV_V3_MODE_DEPTH: return photo_content ? XRT_CONV_TASK_DEPTH_MONO : XRT_CONV_TASK_DEPTH_VIDEO;
	case CONV_V3_MODE_SBS:
	case CONV_V3_MODE_NVIEW: return XRT_CONV_TASK_VIEWS_MONO;
	case CONV_V3_MODE_GAUSSIANS: return XRT_CONV_TASK_SPLATS_MONO;
	default: return XRT_CONV_TASK_NONE;
	}
}
