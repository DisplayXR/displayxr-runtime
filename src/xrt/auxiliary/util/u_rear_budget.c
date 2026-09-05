// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Rear depth budget policy state machine (XR_DXR_depth_budget).
 * @author David Fattal
 * @ingroup aux_util
 */

#include "util/u_rear_budget.h"
#include "util/u_logging.h"

#include <stdlib.h>
#include <string.h>

#define REAR_BUDGET_MS_TO_NS(ms) ((uint64_t)(ms) * (uint64_t)1000000)

void
u_rear_budget_tuning_defaults(struct u_rear_budget_tuning *t)
{
	if (t == NULL) {
		return;
	}
	t->open_dwell_ms = 400;
	t->close_ms = 100;
	t->ramp_open_ms = 300;
	t->ramp_close_ms = 150;
	t->stale_after_ms = 1000;
	t->force = U_REAR_BUDGET_FORCE_AUTO;
}

//! Read one unsigned override; logs once when armed. 0/garbage leaves @p field.
static void
rear_budget_env_u32(const char *name, uint32_t *field)
{
	const char *e = getenv(name);
	if (e == NULL || e[0] == '\0') {
		return;
	}
	const long v = strtol(e, NULL, 10);
	if (v <= 0 || v > 60000) {
		U_LOG_W("REAR_BUDGET: %s='%s' out of range (1..60000 ms) — ignored, keeping %u", name, e, *field);
		return;
	}
	*field = (uint32_t)v;
	U_LOG_W("REAR_BUDGET: %s armed = %u ms", name, *field);
}

void
u_rear_budget_tuning_from_env(struct u_rear_budget_tuning *t)
{
	if (t == NULL) {
		return;
	}

	const char *force = getenv("DXR_REAR_BUDGET");
	if (force != NULL && force[0] != '\0') {
		if (strcmp(force, "clip") == 0) {
			t->force = U_REAR_BUDGET_FORCE_CLIP;
			U_LOG_W("REAR_BUDGET: DXR_REAR_BUDGET armed = clip (budget pinned to 0 vH)");
		} else if (strcmp(force, "open") == 0) {
			t->force = U_REAR_BUDGET_FORCE_OPEN;
			U_LOG_W("REAR_BUDGET: DXR_REAR_BUDGET armed = open (budget pinned to unrestricted)");
		} else if (strcmp(force, "auto") == 0) {
			t->force = U_REAR_BUDGET_FORCE_AUTO;
			U_LOG_W("REAR_BUDGET: DXR_REAR_BUDGET armed = auto (the default policy)");
		} else {
			U_LOG_W("REAR_BUDGET: DXR_REAR_BUDGET='%s' unrecognised (clip|open|auto) — using auto", force);
		}
	}

	rear_budget_env_u32("DXR_REAR_BUDGET_OPEN_DWELL_MS", &t->open_dwell_ms);
	rear_budget_env_u32("DXR_REAR_BUDGET_CLOSE_MS", &t->close_ms);
	rear_budget_env_u32("DXR_REAR_BUDGET_RAMP_OPEN_MS", &t->ramp_open_ms);
	rear_budget_env_u32("DXR_REAR_BUDGET_RAMP_CLOSE_MS", &t->ramp_close_ms);
}

const char *
u_rear_budget_state_str(enum u_rear_budget_state s)
{
	switch (s) {
	case U_REAR_BUDGET_UNRESTRICTED_OPAQUE: return "UNRESTRICTED_OPAQUE";
	case U_REAR_BUDGET_UNRESTRICTED_WORKSPACE: return "UNRESTRICTED_WORKSPACE";
	case U_REAR_BUDGET_OPEN: return "OPEN";
	case U_REAR_BUDGET_CLIPPED_BUSY_BACKGROUND: return "CLIPPED_BUSY_BACKGROUND";
	case U_REAR_BUDGET_CLIPPED_NO_SOURCE: return "CLIPPED_NO_SOURCE";
	case U_REAR_BUDGET_FORCED: return "FORCED";
	default: return "?";
	}
}

void
u_rear_budget_init(struct u_rear_budget *b,
                   const struct u_rear_budget_tuning *tuning,
                   const char *label,
                   uint64_t now_ns)
{
	if (b == NULL) {
		return;
	}
	memset(b, 0, sizeof(*b));

	if (tuning != NULL) {
		b->tuning = *tuning;
	} else {
		u_rear_budget_tuning_defaults(&b->tuning);
	}
	if (b->tuning.open_dwell_ms == 0) {
		b->tuning.open_dwell_ms = 400;
	}
	if (b->tuning.ramp_open_ms == 0) {
		b->tuning.ramp_open_ms = 1;
	}
	if (b->tuning.ramp_close_ms == 0) {
		b->tuning.ramp_close_ms = 1;
	}
	if (b->tuning.stale_after_ms == 0) {
		b->tuning.stale_after_ms = 1000;
	}

	// The conservative start: no source yet, so clip at the ZDP. A session
	// that turns out to be opaque is corrected on its very first update.
	b->state = U_REAR_BUDGET_CLIPPED_NO_SOURCE;
	b->current_vh = 0.0f;
	b->ramp_from_vh = 0.0f;
	b->ramp_to_vh = 0.0f;
	b->ramp_start_ns = now_ns;
	b->ramp_duration_ns = 0;
	b->last_generation_change_ns = now_ns;

	if (label != NULL) {
		size_t n = strlen(label);
		if (n >= sizeof(b->label)) {
			n = sizeof(b->label) - 1;
		}
		memcpy(b->label, label, n);
		b->label[n] = '\0';
	} else {
		b->label[0] = '\0';
	}
}

//! Ease-out so the clip plane decelerates into its target instead of stopping dead.
static inline float
rear_budget_ease_out(float p)
{
	const float inv = 1.0f - p;
	return 1.0f - inv * inv;
}

//! Retarget the ramp. @p duration_ms == 0 snaps.
static void
rear_budget_retarget(struct u_rear_budget *b, float target_vh, uint32_t duration_ms, uint64_t now_ns)
{
	if (b->ramp_to_vh == target_vh) {
		return;
	}
	b->ramp_from_vh = b->current_vh;
	b->ramp_to_vh = target_vh;
	b->ramp_start_ns = now_ns;

	if (duration_ms == 0) {
		b->ramp_duration_ns = 0;
		b->current_vh = target_vh;
		return;
	}

	// Scale the configured full-span duration by the span actually travelled,
	// so a half-open budget closes in half the close time rather than taking
	// the full one — the RATE is the tunable, not the trip.
	float span = target_vh - b->ramp_from_vh;
	if (span < 0.0f) {
		span = -span;
	}
	const float frac = span / U_REAR_BUDGET_UNRESTRICTED_VH;
	uint64_t dur = (uint64_t)((double)REAR_BUDGET_MS_TO_NS(duration_ms) * (double)frac);
	if (dur == 0) {
		dur = 1;
	}
	b->ramp_duration_ns = dur;
}

static void
rear_budget_advance_ramp(struct u_rear_budget *b, uint64_t now_ns)
{
	if (b->ramp_duration_ns == 0) {
		b->current_vh = b->ramp_to_vh;
		return;
	}
	const uint64_t elapsed = (now_ns > b->ramp_start_ns) ? (now_ns - b->ramp_start_ns) : 0;
	if (elapsed >= b->ramp_duration_ns) {
		b->current_vh = b->ramp_to_vh;
		b->ramp_duration_ns = 0;
		return;
	}
	const float p = (float)((double)elapsed / (double)b->ramp_duration_ns);
	b->current_vh = b->ramp_from_vh + (b->ramp_to_vh - b->ramp_from_vh) * rear_budget_ease_out(p);
}

static bool
rear_budget_is_clipped(enum u_rear_budget_state s)
{
	return s == U_REAR_BUDGET_CLIPPED_BUSY_BACKGROUND || s == U_REAR_BUDGET_CLIPPED_NO_SOURCE;
}

void
u_rear_budget_update(struct u_rear_budget *b,
                     const struct u_rear_budget_in *in,
                     uint64_t now_ns,
                     struct u_rear_budget_out *out)
{
	if (b == NULL || in == NULL) {
		if (out != NULL) {
			out->far_offset_vh = 0.0f;
			out->state = U_REAR_BUDGET_CLIPPED_NO_SOURCE;
			out->cue_energy = 0.0f;
		}
		return;
	}

	const enum u_rear_budget_state prev = b->state;
	enum u_rear_budget_state next = prev;
	float target_vh = 0.0f;
	uint32_t ramp_ms = 0; // 0 = snap

	if (b->tuning.force == U_REAR_BUDGET_FORCE_CLIP) {
		next = U_REAR_BUDGET_FORCED;
		target_vh = 0.0f;
		b->cue_energy = 0.0f;
	} else if (b->tuning.force == U_REAR_BUDGET_FORCE_OPEN) {
		next = U_REAR_BUDGET_FORCED;
		target_vh = U_REAR_BUDGET_UNRESTRICTED_VH;
		b->cue_energy = 0.0f;
	} else if (!in->transparent) {
		// Nothing composites over the desktop, so there is no conflict to
		// have an opinion about.
		next = U_REAR_BUDGET_UNRESTRICTED_OPAQUE;
		target_vh = U_REAR_BUDGET_UNRESTRICTED_VH;
		b->cue_energy = 0.0f;
		b->neutral_run_active = false;
		b->busy_run_active = false;
	} else if (in->under_workspace) {
		// A workspace controller owns what is behind the app; the desktop is
		// not showing through. This is today's behaviour, unchanged.
		next = U_REAR_BUDGET_UNRESTRICTED_WORKSPACE;
		target_vh = U_REAR_BUDGET_UNRESTRICTED_VH;
		b->cue_energy = 0.0f;
		b->neutral_run_active = false;
		b->busy_run_active = false;
	} else {
		// Transparent + standalone: the analysed background decides.
		if (!b->have_generation || in->generation != b->last_generation) {
			b->have_generation = true;
			b->last_generation = in->generation;
			b->last_generation_change_ns = now_ns;
		}
		const bool stalled =
		    (now_ns - b->last_generation_change_ns) > REAR_BUDGET_MS_TO_NS(b->tuning.stale_after_ms);

		if (!in->source_available || !in->have_result || stalled) {
			next = U_REAR_BUDGET_CLIPPED_NO_SOURCE;
			target_vh = 0.0f;
			b->cue_energy = 0.0f;
			b->neutral_run_active = false;
			b->busy_run_active = false;
		} else if (in->result.neutral) {
			b->cue_energy = in->result.cue_energy;
			b->busy_run_active = false;
			if (!b->neutral_run_active) {
				b->neutral_run_active = true;
				b->neutral_since_ns = now_ns;
			}
			if ((now_ns - b->neutral_since_ns) >= REAR_BUDGET_MS_TO_NS(b->tuning.open_dwell_ms)) {
				next = U_REAR_BUDGET_OPEN;
				target_vh = U_REAR_BUDGET_UNRESTRICTED_VH;
				ramp_ms = b->tuning.ramp_open_ms;
			} else {
				// Dwell not served yet. Stay shut, and stay HONEST about
				// why: if the reason was "no source" it still is not
				// "busy background".
				next = rear_budget_is_clipped(prev) ? prev : U_REAR_BUDGET_CLIPPED_BUSY_BACKGROUND;
				target_vh = 0.0f;
			}
		} else {
			b->cue_energy = in->result.cue_energy;
			b->neutral_run_active = false;
			if (!b->busy_run_active) {
				b->busy_run_active = true;
				b->busy_since_ns = now_ns;
			}
			if ((now_ns - b->busy_since_ns) >= REAR_BUDGET_MS_TO_NS(b->tuning.close_ms)) {
				next = U_REAR_BUDGET_CLIPPED_BUSY_BACKGROUND;
				target_vh = 0.0f;
				ramp_ms = b->tuning.ramp_close_ms;
			} else {
				// Inside the close grace: hold the state AND the ramp
				// already in flight rather than restarting either.
				next = prev;
				target_vh = b->ramp_to_vh;
				ramp_ms = 0;
			}
		}
	}

	// A ramp is only for the two states that SLIDE. Everything else is a
	// statement of fact, not a perceptual judgement, and snaps.
	if (next != U_REAR_BUDGET_OPEN && next != U_REAR_BUDGET_CLIPPED_BUSY_BACKGROUND) {
		ramp_ms = 0;
	}

	rear_budget_retarget(b, target_vh, ramp_ms, now_ns);
	rear_budget_advance_ramp(b, now_ns);

	if (next != prev) {
		b->state = next;
		// Rare lifecycle edge, one line each — never per frame (#441 tiering).
		U_LOG_W("REAR_BUDGET %s: state %s -> %s cue=%.2f gen=%u", b->label[0] != '\0' ? b->label : "session",
		        u_rear_budget_state_str(prev), u_rear_budget_state_str(next), (double)b->cue_energy,
		        in->generation);
	}

	if (out != NULL) {
		out->far_offset_vh = b->current_vh;
		out->state = b->state;
		out->cue_energy = b->cue_energy;
	}
}
