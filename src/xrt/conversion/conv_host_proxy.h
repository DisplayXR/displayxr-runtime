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
#include <stddef.h>
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

	//! Free @p ud (the proxy owns the ops once created; NULL = nothing to free).
	void (*destroy)(void *ud);
};

struct conv_host_proxy_config
{
	const char *provider_id;     //!< registry id (CONV_HOST_ARG_PROVIDER)
	const char *provider_binary; //!< absolute path of the provider DLL / .so (CONV_HOST_ARG_BINARY)
	const char *host_exe;        //!< absolute path of displayxr-conversion-host(.exe)
	uint64_t adapter_luid;       //!< the service's ingest adapter; 0 = CPU / any

	//! Supervisor timings; NULL = the §3.4 defaults (tests shorten them).
	const struct conv_supervisor_config *supervisor;

	/*! How long a starting host may take to connect its channel and say
	 *  hello. A start costs ~4 s at best, far more under load: default and
	 *  floor CONV_HOST_PROXY_CONNECT_TIMEOUT_MS_DEFAULT (0 = the default). The
	 *  supervisor's hello timeout is raised to it. */
	uint32_t connect_timeout_ms;

	//! CPU scratch bytes per lane (input images); 0 = default (64 MiB).
	uint64_t scratch_bytes;

	//! The GPU lane; NULL = CPU memory only.
	const struct conv_host_proxy_gpu_ops *gpu;

	//! Extra host arguments (tests: a scripted host); appended after the standard ones.
	const char *const *extra_args;
	uint32_t extra_arg_count;

	//! Append the host's stdout / stderr here (NULL = discard).
	const char *log_path;

	/*! Spawn the host as soon as the proxy exists, to describe it. Default
	 *  false: the describe cache answers and the first warm / create spawns
	 *  the host (roadmap §3.4 "Spawn"); with no cached describe at all, the
	 *  service calls conv_host_proxy_request_describe instead. */
	bool spawn_for_describe;

	/*! The maintenance-mutex probe (roadmap §3.6): true while an installer
	 *  holds `Global\DisplayXR-ConvMaintenance-<id>`. NULL = the OS check
	 *  (os_named_mutex_held on Windows; never held on POSIX). Tests on every
	 *  OS substitute their own. Called on the supervisor thread, no lock held. */
	bool (*maintenance_held)(void *ud, const char *provider_id);
	void *maintenance_ud;
};

//! Default (and floor) of conv_host_proxy_config::connect_timeout_ms.
#define CONV_HOST_PROXY_CONNECT_TIMEOUT_MS_DEFAULT 20000u

//! Defaults: connect_timeout_ms = 20 s; everything else 0 / NULL / false.
void
conv_host_proxy_config_defaults(struct conv_host_proxy_config *cfg);

/*!
 * Create the proxy. Nothing is spawned here unless spawn_for_describe;
 * conv_core_add_provider (desc.host) takes ownership.
 */
xrt_result_t
conv_host_proxy_create(const struct conv_host_proxy_config *cfg, struct conv_host **out_host);

/*! The provider has no cached describe (first ever run): spawn the host once
 *  to describe it; the idle exit reclaims it afterwards. */
void
conv_host_proxy_request_describe(struct conv_host *h);

/*!
 * The service's ingest adapter is known (the GPU side attached): later spawns
 * pass it as --adapter-luid, and a live host on another adapter is restarted
 * on it (closed-for-update shape: no crash accounting; the core re-warms on
 * RESTORED). @p gpu (may be NULL), the D3D11 lane on that adapter, is adopted
 * when the proxy has none yet, else destroyed.
 */
void
conv_host_proxy_set_adapter(struct conv_host *h, uint64_t adapter_luid, const struct conv_host_proxy_gpu_ops *gpu);

/*!
 * The provider's registry row or manifest changed (a re-registration, a new
 * `Version`, another `Binary`): clears the supervisor's FAILED and UPDATING
 * gates (roadmap §3.6 respawn rule (i)). A respawn still waits for demand and
 * a free maintenance mutex.
 */
void
conv_host_proxy_config_changed(struct conv_host *h);

//! The maintenance-mutex name for @p provider_id: `Global\DisplayXR-ConvMaintenance-<id>`.
void
conv_host_proxy_maintenance_mutex_name(const char *provider_id, char *buf, size_t size);

//! The LUID the current / last host was spawned with, and the one it said it opened.
void
conv_host_proxy_get_adapter(struct conv_host *h, uint64_t *out_spawned, uint64_t *out_hello);

//! Diagnostics: the supervisor's view (state, restarts, last exit) and the host pid (0 = none).
void
conv_host_proxy_get_status(struct conv_host *h, struct conv_supervisor_status *out_status, long *out_pid);

#ifdef __cplusplus
}
#endif
