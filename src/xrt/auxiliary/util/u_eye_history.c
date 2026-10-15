// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Short timestamped ring of viewer eye-position sets, sampled at a
 *         target time (ADR-054 §4.1 / N6 Part 1, #2144). See the header.
 * @ingroup aux_util
 */

#include "u_eye_history.h"

#include <string.h>


/*
 *
 * Ring helpers (caller holds the mutex).
 *
 */

//! Index of the i-th sample from the oldest (0) to the newest (count - 1).
static inline uint32_t
slot(const struct u_eye_history *h, uint32_t i)
{
	return (h->head + U_EYE_HISTORY_SIZE - h->count + i) % U_EYE_HISTORY_SIZE;
}

static inline const struct u_eye_history_entry *
at(const struct u_eye_history *h, uint32_t i)
{
	return &h->ring[slot(h, i)];
}

static inline struct u_eye_history_entry *
latest(struct u_eye_history *h)
{
	return &h->ring[slot(h, h->count - 1)];
}

static inline float
lerpf(float a, float b, float t)
{
	return a + (b - a) * t;
}

//! Positions lerped per eye; the other fields set by the caller.
static void
lerp_eyes(const struct xrt_eye_positions *a, const struct xrt_eye_positions *b, float t, struct xrt_eye_positions *out)
{
	const uint32_t max = (uint32_t)(sizeof(a->eyes) / sizeof(a->eyes[0]));
	const uint32_t n = a->count < max ? a->count : max;
	for (uint32_t i = 0; i < n; i++) {
		out->eyes[i].x = lerpf(a->eyes[i].x, b->eyes[i].x, t);
		out->eyes[i].y = lerpf(a->eyes[i].y, b->eyes[i].y, t);
		out->eyes[i].z = lerpf(a->eyes[i].z, b->eyes[i].z, t);
	}
	out->count = a->count;
}


/*
 *
 * 'Exported' functions.
 *
 */

void
u_eye_history_init(struct u_eye_history *h)
{
	memset(h, 0, sizeof(*h));
	os_mutex_init(&h->mutex);
}

void
u_eye_history_destroy(struct u_eye_history *h)
{
	os_mutex_destroy(&h->mutex);
	h->count = 0;
	h->head = 0;
}

void
u_eye_history_clear(struct u_eye_history *h)
{
	os_mutex_lock(&h->mutex);
	h->count = 0;
	h->head = 0;
	os_mutex_unlock(&h->mutex);
}

bool
u_eye_history_push(struct u_eye_history *h, const struct xrt_eye_positions *eyes, int64_t ts_ns)
{
	if (eyes == NULL || !eyes->valid) {
		return false; // Nothing to interpolate in an invalid answer.
	}

	os_mutex_lock(&h->mutex);
	if (h->count > 0) {
		struct u_eye_history_entry *l = latest(h);
		if (ts_ns < l->ts_ns) {
			os_mutex_unlock(&h->mutex);
			return false; // Out of order: the ring is time-sorted by construction.
		}
		if (ts_ns == l->ts_ns) {
			l->eyes = *eyes; // Same instant: the newer answer replaces it.
			os_mutex_unlock(&h->mutex);
			return true;
		}
	}
	struct u_eye_history_entry *e = &h->ring[h->head];
	e->eyes = *eyes;
	e->ts_ns = ts_ns;
	h->head = (h->head + 1) % U_EYE_HISTORY_SIZE;
	if (h->count < U_EYE_HISTORY_SIZE) {
		h->count++;
	}
	os_mutex_unlock(&h->mutex);
	return true;
}

enum u_eye_history_result
u_eye_history_get(struct u_eye_history *h, int64_t target_ns, struct xrt_eye_positions *out)
{
	os_mutex_lock(&h->mutex);
	if (h->count == 0) {
		os_mutex_unlock(&h->mutex);
		return U_EYE_HISTORY_RESULT_EMPTY;
	}

	const struct u_eye_history_entry *newest = at(h, h->count - 1);
	const struct u_eye_history_entry *oldest = at(h, 0);
	enum u_eye_history_result r;

	if (target_ns >= newest->ts_ns) {
		const int64_t age_ns = target_ns - newest->ts_ns;
		if (age_ns == 0) {
			*out = newest->eyes;
			out->timestamp_ns = target_ns;
			r = U_EYE_HISTORY_RESULT_EXACT;
		} else if (age_ns > U_EYE_HISTORY_MAX_AGE_NS) {
			// Last known, not tracking: the contract never hands out zeros.
			*out = newest->eyes;
			out->is_tracking = false;
			out->timestamp_ns = newest->ts_ns;
			r = U_EYE_HISTORY_RESULT_STALE;
		} else {
			// Velocity from the newest two samples when they describe the
			// same tracked layout; otherwise hold the latest.
			const struct u_eye_history_entry *prev = h->count >= 2 ? at(h, h->count - 2) : NULL;
			const bool have_velocity = prev != NULL && prev->ts_ns < newest->ts_ns &&
			                           prev->eyes.count == newest->eyes.count && prev->eyes.is_tracking &&
			                           newest->eyes.is_tracking;
			if (have_velocity) {
				const int64_t dt_ns = newest->ts_ns - prev->ts_ns;
				const int64_t ahead_ns = age_ns < U_EYE_HISTORY_MAX_EXTRAPOLATE_NS
				                             ? age_ns
				                             : U_EYE_HISTORY_MAX_EXTRAPOLATE_NS;
				// t > 1 on the prev->newest segment extrapolates it.
				const float t = 1.0f + (float)ahead_ns / (float)dt_ns;
				*out = newest->eyes;
				lerp_eyes(&prev->eyes, &newest->eyes, t, out);
				out->timestamp_ns = newest->ts_ns + ahead_ns;
				r = U_EYE_HISTORY_RESULT_EXTRAPOLATED;
			} else {
				*out = newest->eyes;
				out->timestamp_ns = newest->ts_ns;
				r = U_EYE_HISTORY_RESULT_LATEST;
			}
		}
	} else if (target_ns <= oldest->ts_ns) {
		*out = oldest->eyes;
		out->timestamp_ns = oldest->ts_ns;
		r = target_ns == oldest->ts_ns ? U_EYE_HISTORY_RESULT_EXACT : U_EYE_HISTORY_RESULT_BEFORE_OLDEST;
	} else {
		// Strictly inside (oldest, newest): find the first sample at or
		// after the target; its predecessor is strictly before it.
		uint32_t hi = 1;
		while (hi < h->count && at(h, hi)->ts_ns < target_ns) {
			hi++;
		}
		const struct u_eye_history_entry *b = at(h, hi);
		const struct u_eye_history_entry *a = at(h, hi - 1);
		if (b->ts_ns == target_ns) {
			*out = b->eyes;
			out->timestamp_ns = target_ns;
			r = U_EYE_HISTORY_RESULT_EXACT;
		} else {
			if (a->eyes.count == b->eyes.count) {
				const float t = (float)(target_ns - a->ts_ns) / (float)(b->ts_ns - a->ts_ns);
				*out = b->eyes;
				lerp_eyes(&a->eyes, &b->eyes, t, out);
			} else {
				*out = b->eyes; // Layout changed between them: the newer one.
			}
			// A tracking-lost edge is never interpolated away.
			out->is_tracking = a->eyes.is_tracking && b->eyes.is_tracking;
			out->timestamp_ns = target_ns;
			r = U_EYE_HISTORY_RESULT_INTERPOLATED;
		}
	}
	out->valid = true;
	os_mutex_unlock(&h->mutex);
	return r;
}

bool
u_eye_history_get_latest(struct u_eye_history *h, int64_t *out_ts_ns, struct xrt_eye_positions *out)
{
	os_mutex_lock(&h->mutex);
	if (h->count == 0) {
		os_mutex_unlock(&h->mutex);
		return false;
	}
	const struct u_eye_history_entry *l = latest(h);
	if (out_ts_ns != NULL) {
		*out_ts_ns = l->ts_ns;
	}
	if (out != NULL) {
		*out = l->eyes;
	}
	os_mutex_unlock(&h->mutex);
	return true;
}

uint32_t
u_eye_history_get_size(struct u_eye_history *h)
{
	os_mutex_lock(&h->mutex);
	const uint32_t n = h->count;
	os_mutex_unlock(&h->mutex);
	return n;
}

const char *
u_eye_history_result_str(enum u_eye_history_result r)
{
	switch (r) {
	case U_EYE_HISTORY_RESULT_EMPTY: return "empty";
	case U_EYE_HISTORY_RESULT_EXACT: return "exact";
	case U_EYE_HISTORY_RESULT_INTERPOLATED: return "interpolated";
	case U_EYE_HISTORY_RESULT_EXTRAPOLATED: return "extrapolated";
	case U_EYE_HISTORY_RESULT_LATEST: return "latest";
	case U_EYE_HISTORY_RESULT_STALE: return "stale";
	case U_EYE_HISTORY_RESULT_BEFORE_OLDEST: return "before-oldest";
	default: return "?";
	}
}
