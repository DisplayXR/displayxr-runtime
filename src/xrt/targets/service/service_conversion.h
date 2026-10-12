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
 * GPU glue: the core is created with CPU image allocation and no view
 * synthesis. HOOK for #2003: `service_conversion_set_gpu_alloc` swaps the
 * image allocator for the D3D11 service's (the CPU one keeps freeing what it
 * allocated); the reference view synthesis and the depth readback are core
 * CONFIG, fixed at create, so #2003 either creates the core after the D3D11
 * device (moving service_conversion_start) or adds a core setter; and the v3
 * route's I/O glue is `ipc_server_conv_set_io` (server/ipc_server_conv.h).
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

#ifdef __cplusplus
}
#endif
