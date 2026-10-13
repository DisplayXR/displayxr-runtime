// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  A scriptable CPU conversion provider for the conversion-core tests
 *         (tests_conv_lanes). Implements xrt_conversion_plugin_iface on the
 *         CPU memory kind only; every [S] call is instrumented (concurrency,
 *         flags seen, timings) so the tests can check the threading contract.
 *
 * Not the shipping DisplayXR-SimConversion provider (src/xrt/drivers/
 * sim_conversion, #2002): this one exists to exercise the core's lanes.
 */

#pragma once

#include "xrt/xrt_conversion_plugin.h"

#include "os/os_threading.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CONV_FAKE_MAX_CAPS 8u

struct conv_fake_cap
{
	struct xrt_conv_capability cap;
	uint32_t state; //!< XRT_CONV_STATE_* reported by get_state
	uint32_t failure_reason;
	uint32_t retryable;
	float progress;
	uint32_t warm_ms;    //!< warm() duration
	uint32_t convert_ms; //!< stream_convert() duration
	uint32_t job_ms;     //!< job_execute() duration
	uint64_t job_bytes;  //!< bytes written to the primary sink
	uint32_t job_blob_format; //!< XRT_CONV_BLOB_* of the job blob; 0 = SPZ
	float convergence_value;  //!< > 0: reported (decoded units) on every depth result
	float relief_scale;       //!< != 0: relief reported on every depth result
	float relief_offset;
	bool fail_converts;  //!< converts fail and the cap turns FAILED(DEVICE_LOST)
	//! State a successful warm ends in (0 = READY); e.g. ABSENT for a provider
	//! that declares itself unavailable after trying (#2029). retryable 0.
	uint32_t warm_end_state;
};

struct conv_fake_provider
{
	struct xrt_conversion_plugin_iface iface;
	char id[64];
	struct os_mutex mtx;
	uint32_t cap_count;
	struct conv_fake_cap caps[CONV_FAKE_MAX_CAPS];
	uint64_t generation;
	uint32_t cancel_poll_ms; //!< job / warm cancel polling period (default 10)

	/* Instrumentation (read under mtx). */
	uint32_t in_flight;
	uint32_t max_in_flight;
	uint64_t warms, converts, jobs, streams_created, streams_destroyed;
	uint64_t resets_seen;       //!< converts carrying XRT_CONV_FRAME_RESET
	uint64_t first_frame_flags; //!< frame_flags of the first convert of the last created stream
	bool first_frame_pending;
	struct xrt_conv_params last_params;
	uint64_t job_cancel_seen_ns; //!< when job_execute observed is_cancelled
	uint64_t job_started_ns;
	bool job_running;
	bool destroyed;
};

/*! Initialise a provider with no capabilities (add them with conv_fake_add_cap). */
void
conv_fake_init(struct conv_fake_provider *f, const char *id);

void
conv_fake_fini(struct conv_fake_provider *f);

/*! Add a capability: task, lanes, outputs, flags; sensible defaults for the
 *  rest (CPU import, RGBA8 input, R32F depth, METRIC depth, views 2..8,
 *  PERMISSIVE). Starts INSTALLED. Returns the cap entry for tweaking. */
struct conv_fake_cap *
conv_fake_add_cap(
    struct conv_fake_provider *f, uint32_t cap_id, uint32_t task, uint32_t lanes, uint32_t outputs, uint32_t flags);

//! Script a state (bumps the generation when @p bump).
void
conv_fake_set_state(struct conv_fake_provider *f, uint32_t cap_id, uint32_t state, uint32_t reason, bool bump);

struct conv_fake_cap *
conv_fake_find(struct conv_fake_provider *f, uint32_t cap_id);

static inline struct xrt_conversion_plugin_instance *
conv_fake_inst(struct conv_fake_provider *f)
{
	return (struct xrt_conversion_plugin_instance *)(void *)f;
}

#ifdef __cplusplus
}
#endif
