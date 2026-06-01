// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header-only accessor that selects a vendor DP factory from the
 *         per-monitor @ref xrt_dp_factory_registry, with a scalar safety net.
 * @author David Fattal
 * @ingroup comp_util
 *
 * Phase 3a of multi-display vendor DP routing (issue #69 / ADR-015). The
 * registry (`xrt_system_compositor_info::dp_registry`) is populated at system
 * init but, before this accessor, was consumed by nothing — every compositor
 * read the scalar `dp_factory_*` fields directly. This routes the live
 * single-display render *through* the registry so any resolution bug surfaces
 * immediately, while keeping the scalar as the safety-net fallback:
 *
 *   - `entry_count == 0`            → return the scalar verbatim (zero-risk).
 *   - entry found for the monitor   → return its per-API factory, but if that
 *                                      is NULL fall back to the scalar.
 *   - registry pointer != scalar    → log once (drift detector). On a single
 *                                      display the primary entry equals the
 *                                      scalar, so this stays silent.
 *
 * The factory pointers are `void *` (cast back to `xrt_dp_factory_<api>_fn_t`
 * at the call site, exactly as the scalar fields were).
 */

#pragma once

#include "xrt/xrt_compositor.h"
#include "util/u_logging.h"

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Graphics API selector for @ref comp_dp_factory_for_window — picks which
 * `dp_factory_<api>` field (scalar and registry-entry) to read.
 */
enum comp_dp_api
{
	COMP_DP_API_VK,
	COMP_DP_API_D3D11,
	COMP_DP_API_D3D12,
	COMP_DP_API_GL,
	COMP_DP_API_METAL,
};

/*!
 * Sentinel monitor id meaning "don't care — use the primary entry". Phase 3a
 * call sites pass this (single display → one entry); Phase 3b passes the real
 * per-window monitor id once split-weave routing exists.
 */
#define COMP_DP_PRIMARY_MONITOR ((uint64_t)UINT64_MAX)

/*! @private Read the scalar `dp_factory_<api>` for @p api. */
static inline void *
comp_dp_scalar_for_api(const struct xrt_system_compositor_info *info, enum comp_dp_api api)
{
	switch (api) {
	case COMP_DP_API_VK: return info->dp_factory_vk;
	case COMP_DP_API_D3D11: return info->dp_factory_d3d11;
	case COMP_DP_API_D3D12: return info->dp_factory_d3d12;
	case COMP_DP_API_GL: return info->dp_factory_gl;
	case COMP_DP_API_METAL: return info->dp_factory_metal;
	default: return NULL;
	}
}

/*! @private Read the per-entry `dp_factory_<api>` for @p api. */
static inline void *
comp_dp_entry_for_api(const struct xrt_dp_registry_entry *e, enum comp_dp_api api)
{
	switch (api) {
	case COMP_DP_API_VK: return e->dp_factory_vk;
	case COMP_DP_API_D3D11: return e->dp_factory_d3d11;
	case COMP_DP_API_D3D12: return e->dp_factory_d3d12;
	case COMP_DP_API_GL: return e->dp_factory_gl;
	case COMP_DP_API_METAL: return e->dp_factory_metal;
	default: return NULL;
	}
}

/*!
 * Select the vendor DP factory for the window on @p monitor_id and graphics
 * @p api, routing through the registry with a scalar fallback (see file doc).
 *
 * @param info        System compositor info (holds both the scalar fields and
 *                    the per-monitor registry). Must be non-NULL.
 * @param monitor_id  Monitor to route for, or @ref COMP_DP_PRIMARY_MONITOR for
 *                    the primary entry (Phase 3a always passes the sentinel).
 * @param api         Which graphics-API factory to select.
 * @return Factory pointer (cast to the matching `xrt_dp_factory_<api>_fn_t`),
 *         or NULL if neither the registry nor the scalar provides one.
 */
static inline void *
comp_dp_factory_for_window(const struct xrt_system_compositor_info *info,
                           uint64_t monitor_id,
                           enum comp_dp_api api)
{
	if (info == NULL) {
		return NULL;
	}

	void *scalar = comp_dp_scalar_for_api(info, api);

	// No per-display routing resolved → the scalar is authoritative.
	if (info->dp_registry.entry_count == 0) {
		return scalar;
	}

	// Pick the entry: an exact monitor match, else the primary (entries[0]).
	const struct xrt_dp_registry_entry *chosen_entry = &info->dp_registry.entries[0];
	if (monitor_id != COMP_DP_PRIMARY_MONITOR) {
		for (uint32_t i = 0; i < info->dp_registry.entry_count; i++) {
			if (info->dp_registry.entries[i].monitor_id == monitor_id) {
				chosen_entry = &info->dp_registry.entries[i];
				break;
			}
		}
	}

	void *chosen = comp_dp_entry_for_api(chosen_entry, api);

	// Drift detector: on a single display the registry primary equals the
	// scalar, so this must stay silent. Warn once if they ever diverge.
	if (chosen != scalar) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			U_LOG_W("comp_dp_factory: registry factory %p != scalar %p (api=%d, "
			        "monitor=0x%016llx) — falling back to scalar; investigate registry resolution",
			        chosen, scalar, (int)api, (unsigned long long)monitor_id);
		}
	}

	// Registry-chosen factory wins when present; scalar is the safety net.
	return (chosen != NULL) ? chosen : scalar;
}

#ifdef __cplusplus
}
#endif
