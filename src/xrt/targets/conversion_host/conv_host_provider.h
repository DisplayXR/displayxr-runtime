// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-conversion-host: load ONE provider through the conversion
 *         loader and wrap it in the same exec-thread host the service uses
 *         in-process (conv_host_inproc), so a provider behaves identically in
 *         both hostings (ADR-053 D5; discovery spec §2.4, §4.4).
 *
 * The host iface the provider negotiates with is the satellite's:
 *  - host_flags bit0 (satellite) set;
 *  - get_setting answers exactly as the service's in-process host does:
 *    `DXR_CONV_<ID>_<KEY>` from the environment (the host's environment IS
 *    the service's: the supervisor spawns it with its own block), then the
 *    registration's `Settings\<KEY>` / manifest "settings". The provider never
 *    reads the environment itself. (The P2-1 wire has no settings message:
 *    hello goes host -> service AFTER probe, and probe already needs ENABLE.)
 *  - get_adapter_hint names the adapter LUID passed on the command line;
 *  - notify_changed raises a flag the control loop forwards to the service.
 *
 * @ingroup conv_host
 */

#pragma once

#include "conversion/conv_host_iface.h"
#include "target_conversion_plugin_loader.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Settings the service may forward at hello.
#define CONV_HOST_MAX_SETTINGS XRT_CONV_LOADER_MAX_SETTINGS

struct conv_host_provider
{
	struct xrt_conversion_plugin_entry *entry; //!< heap (the entry is ~11 KB)
	struct xrt_conversion_plugin_instance *inst;
	struct conv_host *host; //!< conv_host_inproc around inst; NULL until loaded
	bool probed;
};

/*!
 * Install the settings get_setting answers from after the environment
 * (copied; replaces any previous set), the provider id that names the
 * `DXR_CONV_<ID>_` prefix, and the adapter LUID get_adapter_hint names. Call
 * on the main thread before @ref conv_host_provider_load.
 */
void
conv_host_provider_set_config(const char *provider_id,
                              const struct xrt_conversion_plugin_setting *settings,
                              uint32_t count,
                              uint64_t adapter_luid);

/*!
 * @ref conv_host_provider_set_config with the settings of @p provider_id's own
 * registration (enumerated, never loaded): the same values the service reads.
 */
void
conv_host_provider_config_from_registration(const char *provider_id, uint64_t adapter_luid);

/*!
 * Negotiate (ABI gate), probe and wrap the provider at @p binary_path.
 * @return a CONV_HOST_EXIT_* code: 0 on success, 10 on an ABI-major mismatch,
 *         11 for every other load / negotiate / probe failure. @p out_reason
 *         (optional) says why.
 */
int
conv_host_provider_load(struct conv_host_provider *p,
                        const char *provider_id,
                        const char *binary_path,
                        char *out_reason,
                        uint32_t reason_size);

/*!
 * True once since the last call when the provider called notify_changed;
 * *out_cap_id gets the cap (0 = everything; several = 0).
 */
bool
conv_host_provider_take_changed(uint32_t *out_cap_id);

/*!
 * Orderly teardown: destroy the inproc host (cancels, drains, destroys the
 * instance). The binary itself stays mapped until process exit (a provider
 * thread may still be unwinding).
 */
void
conv_host_provider_unload(struct conv_host_provider *p);

//! The host iface the provider was negotiated with (for tests).
const struct xrt_conversion_plugin_host_iface *
conv_host_provider_host_iface(void);

#ifdef __cplusplus
}
#endif
