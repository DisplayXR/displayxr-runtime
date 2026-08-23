// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  "Which adapter should render?" resolver, ADR-037 §2 (C++ flavour).
 * @ingroup aux_d3d
 */

#pragma once

#include "util/u_logging.h"

#include <d3dcommon.h>
#include <dxgi.h>
#include <wil/com.h>

#include <stdint.h>


namespace xrt::auxiliary::d3d {

/*!
 * @brief The outcome of one render-adapter resolution.
 *
 * @p provenance is always a non-null pointer into static storage, even when
 * @p adapter is null, so a caller can log the reason for a failure the same way
 * it logs the reason for a success.
 */
struct RenderAdapterChoice
{
	//! The chosen adapter, or nullptr when nothing usable was found.
	wil::com_ptr<IDXGIAdapter> adapter;

	//! The chosen adapter's LUID. Only meaningful when @p adapter != nullptr.
	LUID luid;

	//! Short, stable string naming the rule that decided. Never nullptr.
	const char *provenance;

	//! True when `DXR_D3D_FORCE_GPU` decided rather than the ranking.
	bool from_env;
};

/*!
 * @brief The most capable render adapter, per ADR-037 §2.
 *
 * **Ranking**, applied to every adapter that survives the exclusions below, in
 * the order ADR-037 §2 states:
 *
 * 1. **Dedicated VRAM**, descending. This is the primary key on purpose: it is
 *    the one capability signal no registry state can reorder, which is also why
 *    `DXR_D3D_FORCE_GPU=igpu|dgpu` classifies by it (a per-app
 *    `UserGpuPreferences` entry overrides the *preference argument* to
 *    `EnumAdapterByGpuPreference`, so DXGI's own ordering is not trustworthy).
 * 2. **Adapter kind**, discrete > integrated > software. DXGI reports no kind,
 *    so it is inferred: the software flag (or the Microsoft Basic Render Driver
 *    PCI ids) means software, and at/above 512 MB of dedicated VRAM means
 *    discrete. The threshold is a *tiebreak* heuristic only — it never excludes.
 * 3. **Lowest enumeration index.** The documented deterministic tiebreak, so two
 *    identical adapters always resolve the same way across runs.
 *
 * **Exclusions.** Software adapters (WARP / Basic Render Driver), remote
 * adapters, and adapters that cannot create a device at @p min_feature_level.
 *
 * Deliberately **not** excluded: an adapter that enumerates zero DXGI outputs.
 * On a hybrid (Optimus) box the render-only discrete GPU has no outputs and
 * still presents perfectly well through the OS — treating "no outputs" as
 * "cannot present" would exclude exactly the adapter the rule wants. See the
 * note in the .cpp on how ADR-037's "cannot present" is read here.
 *
 * **Overrides** (`DXR_D3D_FORCE_GPU` = `igpu` | `dgpu` | `scanout` | `<index>`)
 * are honoured *through* this function, so the one override channel keeps
 * working without any call site reimplementing it. They set
 * @ref RenderAdapterChoice::from_env, because ADR-037 §4 calls them overrides,
 * not policy inputs: a caller that must distinguish "the policy chose this" from
 * "a human forced this" can.
 *
 * @param panel_screen_left Panel left edge in OS virtual-screen coordinates.
 * @param panel_screen_top Panel top edge in OS virtual-screen coordinates.
 * @param panel_pixel_width Panel width in physical pixels (0 = unknown).
 * @param panel_pixel_height Panel height in physical pixels (0 = unknown).
 * @param min_feature_level The feature level an adapter must reach to qualify.
 * @param log_level The level to compare against for internal log messages.
 *
 * @note The panel rect is consulted **only** by `DXR_D3D_FORCE_GPU=scanout`,
 * which delegates to @ref getScanoutAdapter rather than duplicating any
 * `QueryDisplayConfig` logic. Zeroes are fine when the rect is not known.
 */
RenderAdapterChoice
getRenderAdapter(int32_t panel_screen_left,
                 int32_t panel_screen_top,
                 uint32_t panel_pixel_width,
                 uint32_t panel_pixel_height,
                 D3D_FEATURE_LEVEL min_feature_level = D3D_FEATURE_LEVEL_11_0,
                 u_logging_level log_level = U_LOGGING_INFO);

} // namespace xrt::auxiliary::d3d
