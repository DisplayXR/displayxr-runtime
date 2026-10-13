// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The service's conversion core (ADR-053, #2005): built from the
 *         conversion-provider loader at service start, torn down last.
 *
 * What this owns:
 *  - enumerate + negotiate + probe the registered conversion providers
 *    (target_conversion_plugin_loader), add each to ONE conv_core;
 *  - the persisted describe cache (`conversion-describe-cache.bin` next to
 *    service.json), loaded before the providers are added, so the capability
 *    table is populated from the first query of a warm boot;
 *  - registry-change notifications: a RegNotifyChangeKeyValue watch on
 *    HKLM + HKCU `Software\DisplayXR\ConversionProviders` (the root `conv use`
 *    / `conv enable` write, the same mechanism the tray uses for `dp use`
 *    re-probes) plus the providers' own notify_changed, both turned into
 *    conv_core_provider_changed / set_provider_enabled / add_provider on the
 *    watch thread (never on a provider's thread: a provider may notify from
 *    inside a describe that holds the core's registry lock);
 *  - the IPC side: ipc_server_set_conv (the v3 lift handlers' adapter) and
 *    the status snapshot's conversion rows (ipc_server_set_conv_status_source).
 *
 * GPU glue (#2003): the core is created at start with CPU image allocation
 * and no view synthesis (the device does not exist yet). Once init_all made
 * the system compositor, the IPC layer (ipc_server_conv_d3d11.c) calls the
 * hook this file registers (ipc_server_conv_set_gpu_hook): it swaps the image
 * allocator for the D3D11 glue's (service_conversion_set_gpu_alloc; the CPU
 * allocator still serves providers that cannot import a shared texture) and
 * registers the legacy DP-lift adapter (`dp-lift`, ProbeOrder 150, hosted
 * in-service always, licence-exempt) over the lift thread's backend. The
 * S5 / S6 stages go in with conv_core_set_gpu; the v3 route's I/O last.
 * The S7 convergence observer (rt#1978's u_lift_autoconv) is core config from
 * the start: it needs no device.
 *
 * @ingroup targets_service
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct conv_core;
struct conv_image_alloc_ops;

/*! Build the core and register it with the IPC server. Never fails the
 *  service: with no providers (or on any error) the core still exists and
 *  the v3 handlers keep falling through to svc_lift. Call before
 *  ipc_server_main. */
void
service_conversion_start(void);

/*! Unregister, stop the watch thread, save the describe cache, destroy the
 *  core. Call after ipc_server_main returns (last). */
void
service_conversion_stop(void);

//! The core, or NULL before start / after stop.
struct conv_core *
service_conversion_core(void);

/*! HOOK for #2003: route image allocation to the GPU glue (NULL = back to
 *  CPU). Images keep being freed by whichever allocator made them (CPU
 *  images carry XRT_CONV_MEM_CPU). */
void
service_conversion_set_gpu_alloc(const struct conv_image_alloc_ops *ops);

/*! #2029: the display-plug-in registration changed (the tray's
 *  DisplayProcessors watch): a vendor plug-in or its conversion module was
 *  installed, removed or re-registered. Retries the dp-lift rows at once (a
 *  FAILED "no module" re-warms per PrewarmOn), as a ConversionProviders change
 *  does -- a vendor installer touches only DisplayProcessors. Any thread. */
void
service_conversion_display_plugins_changed(void);

#ifdef __cplusplus
}
#endif
