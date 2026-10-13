// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The D3D11 half of a conversion-host satellite lane (ADR-053 P2,
 *         roadmap conversion-providers.md §3.5): conv_host_proxy_gpu_ops over
 *         NT-handle textures and a shared fence.
 *
 * THE FENCE RULE. This lane never enqueues a GPU wait on a host-signalled
 * fence, on any context. A host result is consumed only after the CPU has
 * seen the host's fence reach the completion record's value
 * (ID3D11Fence::SetEventOnCompletion + a bounded wait). The copy into the
 * runtime's ring target then runs on the lane's OWN device and immediate
 * context (never a compositor context), and the lane waits on the CPU for
 * that copy too (an event query) before the proxy publishes the result.
 *
 * The lane device is created on the adapter whose LUID the proxy was given
 * (the service's ingest adapter; 0 = the default adapter). Both the host's
 * output and the runtime's target are opened here by their NT handles
 * (OpenSharedResource1), the target cached per import_key; a target guarded
 * by a keyed mutex is acquired / released with its own keys around the copy.
 *
 * @ingroup comp_d3d11_service
 */

#pragma once

#include "conversion/conv_host_proxy.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Create a lane on the adapter with @p adapter_luid (0 = default) and fill
 * @p out (out->ud owns it; out->destroy frees it). false = no D3D11.1 device
 * with shared fences on that adapter: the satellite stays on the CPU lane.
 */
bool
d3d11_conv_host_lane_create(uint64_t adapter_luid, struct conv_host_proxy_gpu_ops *out);

#ifdef __cplusplus
}
#endif
