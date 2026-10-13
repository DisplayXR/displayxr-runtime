// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-conversion-host: the control loop and the data-lane
 *         workers (ADR-053 roadmap §3.5) over one loaded provider.
 *
 * Threads, all runtime-owned (discovery spec §2.3):
 *  - the CONTROL thread (the caller of @ref conv_host_serve_run): blocking
 *    receive, dispatch. It never calls a blocking provider slot: [S] work
 *    (warm, evict, stream create / destroy, jobs) is queued to the provider's
 *    exec threads (conv_host_inproc) and replied from their on_complete;
 *  - one WORKER per data lane: pops submit records, runs stream_convert
 *    through the exec host, pushes completion records;
 *  - the TICKER: 1 Hz heartbeat, WARN+ log forwarding, notify_changed ->
 *    STATE_CHANGED, the idle exit (no open stream, job or warm pin for
 *    idle_exit_ms).
 *
 * Handles never travel in ring records; input images and fences are indices
 * registered over the control channel, outputs are host-allocated and
 * announced once per ALLOC_OUTPUTS.
 *
 * @ingroup conv_host
 */

#pragma once

#include "conv_host_chan.h"
#include "conv_host_provider.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct conv_host_d3d11;

struct conv_host_serve_config
{
	const char *provider_id;
	struct conv_host_provider *provider; //!< loaded
	struct conv_host_d3d11 *d3d11;       //!< NULL = CPU lane only
	uint64_t adapter_luid;               //!< what hello reports (0 = CPU only)
	uint32_t idle_exit_ms;
};

/*!
 * Say hello, wait for the ack, then serve until shutdown, the channel closes,
 * the idle exit, or @ref conv_host_serve_request_exit. Returns the process
 * exit code (CONV_HOST_EXIT_*). Tears the provider down on the orderly paths.
 */
int
conv_host_serve_run(struct conv_host_chan *chan, const struct conv_host_serve_config *cfg);

//! Main thread, first thing: makes @ref conv_host_serve_request_exit usable before run.
void
conv_host_serve_prepare(void);

/*!
 * Ask the running loop to return @p exit_code (first request wins). Safe from
 * any thread, including the session-end window's; a request made before
 * @ref conv_host_serve_run makes it return at once.
 */
void
conv_host_serve_request_exit(int exit_code);

#ifdef __cplusplus
}
#endif
