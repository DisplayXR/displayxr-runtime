// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Runtime-side loader for viewer-tracking provider plug-ins (ADR-054 §4.1, N6 Part 2).
 *
 * The fourth class loader, written as a table row on the shared component
 * loader (`target_component_loader.h`): `HKLM\Software\DisplayXR\TrackingProviders\<id>`
 * on Windows (HKCU `Enabled` overlay, `Policy\Lock`), `NNN-<id>-tracking-provider.json`
 * manifests in the shared POSIX roots. Every provider that loads, passes the
 * ABI gate and whose `probe()` claims a usable source stays resident, ranked
 * `PreferredPlugin` first, then ProbeOrder ascending. Which one serves a screen
 * is the viewer arbiter's call (`target_viewer_tracking.h`), never the loader's.
 *
 * The scan is one-shot per process and runs lazily — the first time the viewer
 * arbiter asks, which it does only for a display whose viewer policy is
 * EXTERNAL_OK and which does not track. A process whose display tracks never
 * loads a tracking provider.
 *
 * Contract: `docs/specs/runtime/tracking-provider-discovery.md`.
 *
 * @ingroup target_common
 */

#pragma once

#include "xrt/xrt_tracking_plugin.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct target_component_class;
struct target_component_setting_desc;

//! One registered provider as discovered, WITHOUT loading it (`displayxr-cli tracking list`).
struct target_tracking_plugin_desc
{
	char id[64];
	char display_name[128];
	char vendor[64];
	char version[64];
	char binary_path[512];
	uint32_t probe_order;
	bool enabled;
	char enabled_source[12]; //!< "user" / "machine" / "manifest" / "default" (the input class's rule)
	uint32_t provenance;     //!< enum target_component_provenance
	char provenance_reason[160];
	char signer[128];
};

//! The class's row in the shared loader (store key for `tracking options|set|unset`). Never NULL.
const struct target_component_class *
target_tracking_plugin_component_class(void);

//! Enumerate registered providers without loading; runs the provenance check per row. Returns the count.
int
target_tracking_plugin_enumerate(struct target_tracking_plugin_desc *out, int max);

//! Declared settings of @p id: manifest `settings_schema` + the binary's `describe_settings` (negotiate only).
int
target_tracking_plugin_describe_settings(const char *id, struct target_component_setting_desc *out, int max);

/*!
 * `PreferredPlugin`: Windows HKCU (unless `Policy\Lock`) then HKLM
 * `Software\DisplayXR\TrackingProviders\PreferredPlugin`; POSIX
 * `XRT_PREFERRED_TRACKING_PLUGIN_ID` then the per-user `tracking-preferred`
 * file. "" + false when unset.
 */
bool
target_tracking_plugin_get_preferred(char *out, size_t cap);

//! Resident providers (loaded, ABI-passed, probe claimed). Runs the one-shot scan on first call.
int
target_tracking_plugin_get_count(void);

//! The @p index'th resident provider's iface in rank order, or NULL.
const struct xrt_tracking_plugin_iface *
target_tracking_plugin_get_iface(int index);

//! Its `probe()` instance (may be NULL legitimately).
struct xrt_tracking_plugin_instance *
target_tracking_plugin_get_instance(int index);

//! Its REGISTRATION id (what `PreferredPlugin` / `Enabled` name), "" out of range.
const char *
target_tracking_plugin_get_row_id(int index);

//! Its rank key: 0 for the preferred row, else its ProbeOrder; UINT32_MAX out of range.
uint32_t
target_tracking_plugin_get_priority(int index);

/*!
 * The scan's tally (forces it): registered rows, enabled rows, declined
 * cleanly (no usable source), failed to load / ABI-rejected, refused by the
 * provenance gate. Any out-param may be NULL.
 */
void
target_tracking_plugin_get_scan_result(
    int *out_registered, int *out_enabled, int *out_declined, int *out_failed, int *out_untrusted);

//! `DXR_TRACKING_PROVIDERS=0` in this process: the scan loads nothing (CTS / test isolation).
bool
target_tracking_plugin_disabled_by_env(void);

/*!
 * Publish the active display's nominal geometry to providers (what
 * `host->get_display_geometry` returns from then on). Copied; NULL ignored.
 */
void
target_tracking_plugin_set_display_geometry(const struct xrt_tracking_host_display_geometry *geometry);

//! THE process-lifetime host iface handed to every provider. Never NULL.
const struct xrt_tracking_plugin_host_iface *
target_tracking_plugin_get_host_iface(void);

#ifdef __cplusplus
}
#endif
