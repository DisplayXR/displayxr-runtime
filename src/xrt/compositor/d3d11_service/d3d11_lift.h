// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The `dp-lift` backend on the D3D11 service: the one lift thread, its
 *         device, and the display plug-in's lift display processor (ADR-042;
 *         ADR-053 P1b).
 *
 * ## Shape
 *
 * One @ref d3d11_lift per service system, created lazily on the first lift
 * call. It owns:
 *
 *  - the LIFT THREAD, the only thread that ever touches the lift display
 *    processor (the vendor module's contract, xrt_display_processor_d3d11.h).
 *    It is a command executor: the conversion core's `dp-lift` provider
 *    (conversion/conv_provider_legacy_dp.h) runs in the in-service host, and
 *    its exec thread hands each [S] call to this thread as one command
 *    (d3d11_lift_get_dp_backend) and waits, so the vendor module keeps its
 *    one thread and there is one module instance per service. Scheduling
 *    across streams, readiness and when to (re)activate are the core's and
 *    the adapter's business, not this file's;
 *  - a dedicated LIFT DEVICE on the service's adapter, with ID3D11Multithread
 *    protection on its immediate context (a module may flush / signal it from
 *    its own worker). A conversion takes milliseconds to seconds, so it must
 *    never run on the service's shared immediate context.
 *
 * The same object carries the conversion core's D3D11 GLUE on the service
 * device (d3d11_conv_gpu.h): runtime image allocation, the v3 route's import /
 * export, the S5 / S6 stages and the weave path's submit / pin.
 *
 * ## Threads and locks
 *
 *  - `mtx` (inside d3d11_lift) guards the command queue and the module state.
 *    It is never held across a GPU wait, a keyed-mutex acquire, or a module call.
 *  - No slot is ever called on a render, compositor or IPC-dispatch thread:
 *    only the lift thread calls the display processor.
 *
 * @ingroup comp_d3d11_service
 */

#pragma once

#include "xrt/xrt_results.h"
#include "xrt/xrt_dp_lift.h"

#include "conversion/conv_provider_legacy_dp.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <mutex>
#include <stdint.h>

#include "d3d11_conv_gpu.h"

struct d3d11_lift;

/*!
 * Re-resolve the plug-in's factory pair (G5): its LIFT-ONLY factory
 * (xrt_plugin_iface::create_dp_d3d11_lift) and, from the SAME plug-in, its
 * ordinary D3D11 factory as the fallback (used with a NULL window when there
 * is no lift-only one). Called at every activation attempt and by the
 * provider's snapshot; must not block. False / both NULL = no factory.
 */
typedef bool (*d3d11_lift_resolve_factories_fn)(void *ud, void **out_lift_factory, void **out_fallback_factory);

/*!
 * Create the lift module for a service. Cheap: the lift device and the vendor
 * display processor are brought up on the lift thread when the dp-lift
 * adapter first warms.
 *
 * @param svc_device       the service's render device (ID3D11Device1-capable).
 * @param svc_context      its immediate context.
 * @param svc_ctx_mutex    the service's immediate_ctx_mutex.
 * @param resolve          re-resolves the plug-in's lift-only factory and its
 *                         fallback (see d3d11_lift_resolve_factories_fn) at
 *                         every activation attempt. NULL / nothing ⟹ no module.
 * @param resolve_ud       user data for @p resolve.
 */
struct d3d11_lift *
d3d11_lift_create(ID3D11Device *svc_device,
                  ID3D11DeviceContext *svc_context,
                  std::mutex *svc_ctx_mutex,
                  d3d11_lift_resolve_factories_fn resolve,
                  void *resolve_ud);

//! Stop the lift thread, destroy every module stream, the lift DP/device and the glue.
void
d3d11_lift_destroy(struct d3d11_lift **lift_ptr);

/*!
 * The `dp-lift` provider's backend (conv_provider_legacy_dp.h): every [S] call
 * is one command on this module's lift thread.
 */
void
d3d11_lift_get_dp_backend(struct d3d11_lift *lift, struct conv_dp_lift_backend *out);
