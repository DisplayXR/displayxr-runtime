// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The satellite host proxy: a conv_host (conv_host_iface.h) whose
 *         provider runs in a supervised `displayxr-conversion-host` process
 *         (ADR-053 D5; roadmap conversion-providers.md §3.4, §3.5).
 *
 * The service never loads the provider binary. The proxy spawns the host
 * lazily (conv_supervisor.h decides when), speaks the control channel and the
 * shm rings of conv_host_wire.h, answers the [C] calls from what the host
 * described, and turns the host's lifetime into the core's lifecycle events
 * (CONV_HOST_EV_LOST / RESTORED / CRASH_LOOP / FAILED).
 *
 * Memory lanes:
 *  - CPU (portable, every OS): the lane's shm section holds the two rings and,
 *    after them, a SCRATCH area. A CPU input image is registered once per slot
 *    lifetime at a scratch offset (conv_host_wire_register_image.image.
 *    shm_offset) and its bytes are copied there before each submit. A CPU
 *    output is host-allocated: OUTPUT_IMAGE carries a host-created shm section
 *    as handle [0] (bytes at image.shm_offset in it), which the service maps
 *    and copies into the runtime's ring target after the completion record.
 *  - GPU (Windows, D3D11 NT handle + shared fence): conv_host_proxy_gpu_ops,
 *    supplied by the service. FENCE RULE: the service never enqueues a GPU wait
 *    on a host-signalled fence on any compositor context; the proxy observes
 *    the completion record's fence value from the CPU (gpu->observe_fence, with
 *    a timeout) before the copy into the ring target is issued.
 *
 * Threads per proxy: the supervisor thread (spawn, process wait, bring-up,
 * loss), the channel reader (per host incarnation), and one command worker
 * (one [S] call in flight; the host enforces the provider's own threading).
 *
 * Not built on Android (its `:conversion` service is a different mechanism).
 *
 * @ingroup aux_conversion
 */

#pragma once

#include "conv_host_iface.h"
#include "conv_supervisor.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * The GPU half of a satellite lane, implemented by the graphics backend that
 * owns the service's device (Windows D3D11: d3d11_conv_host_lane.cpp). Every
 * call is made on the proxy's command worker, with no proxy lock held.
 */
struct conv_host_proxy_gpu_ops
{
	void *ud;

	/*! Open a host output (a pulled NT handle, now owned by the callee) on
	 *  the service's device. *out_ctx is passed back to copy / close. */
	bool (*open_output)(void *ud, uint64_t handle, const struct xrt_conv_image *desc, void **out_ctx);

	//! Open a host-created output fence (a pulled NT handle, now owned by the callee).
	bool (*open_fence)(void *ud, uint64_t handle, void **out_fence);

	/*! THE FENCE RULE: block the calling (worker) thread until @p fence
	 *  reaches @p value or @p timeout_ms passes. CPU only: never a GPU wait.
	 *  false = timed out / device lost. */
	bool (*observe_fence)(void *ud, void *fence, uint64_t value, uint32_t timeout_ms);

	//! Copy an opened host output into the runtime's target (already observed complete).
	bool (*copy_output)(void *ud, void *src_ctx, const struct xrt_conv_image *dst);

	void (*close_output)(void *ud, void *ctx);
	void (*close_fence)(void *ud, void *fence);
};

struct conv_host_proxy_config
{
	const char *provider_id;     //!< registry id (CONV_HOST_ARG_PROVIDER)
	const char *provider_binary; //!< absolute path of the provider DLL / .so (CONV_HOST_ARG_BINARY)
	const char *host_exe;        //!< absolute path of displayxr-conversion-host(.exe)
	uint64_t adapter_luid;       //!< the service's ingest adapter; 0 = CPU / any

	//! Supervisor timings; NULL = the §3.4 defaults (tests shorten them).
	const struct conv_supervisor_config *supervisor;

	//! CPU scratch bytes per lane (input images); 0 = default (64 MiB).
	uint64_t scratch_bytes;

	//! The GPU lane; NULL = CPU memory only.
	const struct conv_host_proxy_gpu_ops *gpu;

	//! Extra host arguments (tests: a scripted host); appended after the standard ones.
	const char *const *extra_args;
	uint32_t extra_arg_count;

	//! Append the host's stdout / stderr here (NULL = discard).
	const char *log_path;

	/*! Spawn the host as soon as the proxy exists when it has no describe yet
	 *  (default true: the first service start has no describe cache).
	 *  Otherwise the first warm / create spawns it. */
	bool spawn_for_describe;
};

//! Defaults: spawn_for_describe = true, everything else 0 / NULL.
void
conv_host_proxy_config_defaults(struct conv_host_proxy_config *cfg);

/*!
 * Create the proxy. Nothing is spawned here unless spawn_for_describe;
 * conv_core_add_provider (desc.host) takes ownership.
 */
xrt_result_t
conv_host_proxy_create(const struct conv_host_proxy_config *cfg, struct conv_host **out_host);

//! Diagnostics: the supervisor's view (state, restarts, last exit) and the host pid (0 = none).
void
conv_host_proxy_get_status(struct conv_host *h, struct conv_supervisor_status *out_status, long *out_pid);

#ifdef __cplusplus
}
#endif
