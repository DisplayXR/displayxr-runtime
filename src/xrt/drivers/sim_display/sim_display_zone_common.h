// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Shared zone test-double helpers for the sim_display DP variants
 *         (#224 / ADR-027 Decision 5).
 *
 * Header-only so all five per-API variants (VK / D3D11 / D3D12 / GL / Metal,
 * C, C++ and Objective-C TUs) share one env-knob parser and one
 * OR-downsample map builder instead of five copies:
 *
 *  - `SIM_DISPLAY_ZONE_GRID="WxH"` — simulated hardware cell grid
 *    (default 1x1, today's global on/off shape; max 256 cells).
 *  - `SIM_DISPLAY_ZONE_DUMP=1` — read the published mask back and log the
 *    downsampled per-cell map whenever it changes (only on variants with an
 *    easy CPU readback path — D3D11 and GL; the others log-only).
 *  - `SIM_DISPLAY_WISH_QUANTIZE=off|band|cell` — which simulated quantization
 *    the dump/tint REPORTS (advisory quantization is the DP's prerogative,
 *    ADR-027 Decision 6 — this changes only what the sim visualizes):
 *      off  — no simulated quantization: the raw per-cell any-nonzero
 *             collapse, today's behavior (and the tint stays off);
 *      band — column bands: each grid COLUMN ORs to one value;
 *      cell — per-cell (today's behavior at grid > 1x1), tint enabled.
 *
 * @ingroup drv_sim_display
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>  // sscanf — SIM_DISPLAY_ZONE_GRID parsing
#include <stdlib.h> // getenv
#include <string.h>

#include "xrt/xrt_display_zones.h" // caps shape consumed by SIM_ZONE_FILL_CAPS
#include "util/u_logging.h"

#ifdef __cplusplus
extern "C" {
#endif

//! Hard cap on simulated cells — bounds the per-variant map buffers
//! (`char map[SIM_ZONE_MAX_CELLS + 1]`) and the downsample cost.
#define SIM_ZONE_MAX_CELLS 256

//! Simulated wish quantization the sim REPORTS (dump map / D3D11 tint).
enum sim_zone_quantize
{
	SIM_ZONE_QUANTIZE_OFF = 0,  //!< Raw per-cell any-nonzero collapse (today's behavior); tint off.
	SIM_ZONE_QUANTIZE_BAND = 1, //!< Column bands: each grid column ORs to one value.
	SIM_ZONE_QUANTIZE_CELL = 2, //!< Per-cell (same map as OFF at any grid); tint on.
};

//! Parsed zone env knobs, shared by every sim_display DP variant.
struct sim_zone_config
{
	uint32_t grid_w; //!< Simulated cells across (>= 1).
	uint32_t grid_h; //!< Simulated cells down (>= 1).
	bool dump;       //!< SIM_DISPLAY_ZONE_DUMP readback enabled.
	enum sim_zone_quantize quantize; //!< SIM_DISPLAY_WISH_QUANTIZE.
};

/*!
 * Parse the three zone env knobs into @p cfg. @p variant tags the bad-value
 * warnings (e.g. "D3D11"). Defaults: 1x1 grid, no dump, quantize off.
 */
static inline void
sim_zone_config_from_env(struct sim_zone_config *cfg, const char *variant)
{
	cfg->grid_w = 1;
	cfg->grid_h = 1;
	cfg->dump = false;
	cfg->quantize = SIM_ZONE_QUANTIZE_OFF;

	const char *grid_env = getenv("SIM_DISPLAY_ZONE_GRID");
	if (grid_env != NULL && grid_env[0] != '\0') {
		unsigned gw = 0;
		unsigned gh = 0;
		if (sscanf(grid_env, "%ux%u", &gw, &gh) == 2 && gw >= 1 && gh >= 1 && gw <= SIM_ZONE_MAX_CELLS &&
		    gh <= SIM_ZONE_MAX_CELLS && gw * gh <= SIM_ZONE_MAX_CELLS) {
			cfg->grid_w = gw;
			cfg->grid_h = gh;
		} else {
			U_LOG_W("sim_display %s: bad SIM_DISPLAY_ZONE_GRID '%s' (want WxH, 1..%d cells) — using 1x1",
			        variant, grid_env, SIM_ZONE_MAX_CELLS);
		}
	}

	const char *dump_env = getenv("SIM_DISPLAY_ZONE_DUMP");
	cfg->dump = (dump_env != NULL && dump_env[0] != '\0' && dump_env[0] != '0');

	const char *q_env = getenv("SIM_DISPLAY_WISH_QUANTIZE");
	if (q_env != NULL && q_env[0] != '\0') {
		if (strcmp(q_env, "band") == 0) {
			cfg->quantize = SIM_ZONE_QUANTIZE_BAND;
		} else if (strcmp(q_env, "cell") == 0) {
			cfg->quantize = SIM_ZONE_QUANTIZE_CELL;
		} else if (strcmp(q_env, "off") != 0) {
			U_LOG_W("sim_display %s: bad SIM_DISPLAY_WISH_QUANTIZE '%s' (want off|band|cell) — using off",
			        variant, q_env);
		}
	}
}

/*!
 * OR-downsample a CPU copy of the published R8 wish mask into the simulated
 * cell grid and render it as a '0'/'1' row-major map string (the arbitration
 * rule from docs/roadmap/local-3d-zones.md: any non-zero pixel overlapping a
 * cell ⟹ cell 3D). BAND quantize then collapses each grid column to its OR.
 *
 * @p pixels    Row-major R8 pixels (mask_h rows of @p row_pitch bytes).
 * @p out_map   At least grid_w*grid_h+1 bytes (<= SIM_ZONE_MAX_CELLS+1).
 *
 * Returns false (map untouched) when the grid exceeds SIM_ZONE_MAX_CELLS or
 * any dimension is 0.
 */
static inline bool
sim_zone_downsample_map(const uint8_t *pixels,
                        size_t row_pitch,
                        uint32_t mask_w,
                        uint32_t mask_h,
                        const struct sim_zone_config *cfg,
                        char *out_map)
{
	uint32_t cells = cfg->grid_w * cfg->grid_h;
	if (pixels == NULL || mask_w == 0 || mask_h == 0 || cells == 0 || cells > SIM_ZONE_MAX_CELLS) {
		return false;
	}

	for (uint32_t cy = 0; cy < cfg->grid_h; cy++) {
		uint32_t y0 = (cy * mask_h) / cfg->grid_h;
		uint32_t y1 = ((cy + 1) * mask_h) / cfg->grid_h;
		for (uint32_t cx = 0; cx < cfg->grid_w; cx++) {
			uint32_t x0 = (cx * mask_w) / cfg->grid_w;
			uint32_t x1 = ((cx + 1) * mask_w) / cfg->grid_w;
			char bit = '0';
			for (uint32_t y = y0; y < y1 && bit == '0'; y++) {
				const uint8_t *row = pixels + (size_t)y * row_pitch;
				for (uint32_t x = x0; x < x1; x++) {
					if (row[x] != 0) {
						bit = '1';
						break;
					}
				}
			}
			out_map[cy * cfg->grid_w + cx] = bit;
		}
	}

	// BAND: collapse each column to its OR (simulated column-band switch).
	if (cfg->quantize == SIM_ZONE_QUANTIZE_BAND) {
		for (uint32_t cx = 0; cx < cfg->grid_w; cx++) {
			char band = '0';
			for (uint32_t cy = 0; cy < cfg->grid_h && band == '0'; cy++) {
				if (out_map[cy * cfg->grid_w + cx] == '1') {
					band = '1';
				}
			}
			for (uint32_t cy = 0; cy < cfg->grid_h; cy++) {
				out_map[cy * cfg->grid_w + cx] = band;
			}
		}
	}

	out_map[cells] = '\0';
	return true;
}

/*!
 * Fill the caller's @ref xrt_dp_local_zone_caps the sim_display way (one
 * shared body for all five variants). Validates the V1 floor, writes the V1
 * fields always, and writes the ADR-027 appends only when the caller's
 * struct_size covers them (append contract — see xrt_display_zones.h).
 * The struct type is only forward-used through the per-variant callers'
 * includes, so this helper is a macro-free inline over raw fields.
 */
#define SIM_ZONE_FILL_CAPS(out_caps, cfg)                                                                              \
	do {                                                                                                           \
		(out_caps)->supported = 1;                                                                             \
		(out_caps)->zone_grid_width = (cfg)->grid_w;                                                           \
		(out_caps)->zone_grid_height = (cfg)->grid_h;                                                          \
		(out_caps)->max_mask_width = 16384;                                                                    \
		(out_caps)->max_mask_height = 16384;                                                                   \
		(out_caps)->max_update_hz = 0; /* unlimited — it's a log line */                                       \
		if ((out_caps)->struct_size >= sizeof(struct xrt_dp_local_zone_caps)) {                                \
			(out_caps)->wish_fractional = 1;                                                               \
			(out_caps)->switch_granularity = (uint32_t)XRT_DP_SWITCH_GRANULARITY_CELL_GRID;                \
			memset((out_caps)->reserved, 0, sizeof((out_caps)->reserved));                                 \
		}                                                                                                      \
	} while (0)

#ifdef __cplusplus
}
#endif
