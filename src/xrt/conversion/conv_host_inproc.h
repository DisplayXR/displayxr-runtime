// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  In-service conversion host (ADR-053 roadmap §3.2): a provider loaded
 *         into the service, run on its own exec thread(s) so it behaves
 *         exactly like a satellite.
 *
 * Threads: one exec thread by default (one call in flight per provider);
 * one more when any capability declares XRT_CONV_CAP_LANES_CONCURRENT (a job
 * beside streams), one more for XRT_CONV_CAP_WARM_CONCURRENT (a warm beside
 * converts), and max_concurrent - 1 more for overlapping converts. Admission
 * keeps submission order between commands that conflict, so a queued warm is
 * never starved by a stream of converts.
 *
 * is_cancelled is plumbed through: warm's callback and a job's
 * xrt_conv_job_control::is_cancelled are wrapped so a host shutdown also
 * cancels. Images are registered once per slot lifetime and passed through
 * unchanged (the in-service shim still receives shared handles opened by the
 * service, never per frame).
 *
 * @ingroup aux_conversion
 */

#pragma once

#include "conv_host_iface.h"

#ifdef __cplusplus
extern "C" {
#endif

//! Upper bound on exec threads per in-service provider.
#define CONV_HOST_INPROC_MAX_THREADS 8u
//! Image slots per host.
#define CONV_HOST_INPROC_MAX_IMAGES 256u

/*!
 * Wrap a live provider (negotiated and probed by the loader). The host takes
 * ownership of @p inst: destroy() calls iface->destroy(inst) once every exec
 * thread has returned. Reads the capabilities once (refresh_caps) to size its threads.
 *
 * @return XRT_SUCCESS, XRT_ERROR_ALLOCATION or XRT_ERROR_THREADING_INIT_FAILURE.
 */
xrt_result_t
conv_host_inproc_create(const char *provider_id,
                        const struct xrt_conversion_plugin_iface *iface,
                        struct xrt_conversion_plugin_instance *inst,
                        struct conv_host **out_host);

#ifdef __cplusplus
}
#endif
