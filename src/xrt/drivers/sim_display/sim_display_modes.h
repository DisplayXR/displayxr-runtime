// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_display's two run-time rendering modes (ADR-065): 2D, and the 3D
 *         KIND its OUTPUT setting names, chosen once at device creation.
 * @ingroup drv_sim_display
 *
 * Header-only and pure (no settings reads, no logging), so the table and the
 * start-up rule are unit-testable without the driver (tests_sim_display_modes).
 *
 * ## The table
 *
 * `[0]` = 2D (one view, full resolution, 1x1). `[1]` = the kind:
 *
 * | OUTPUT                   | mode 1 name   | views | scale     | tiles |
 * |--------------------------|---------------|-------|-----------|-------|
 * | anaglyph (default)       | Anaglyph      | 2     | 0.5 x 0.5 | 2x1   |
 * | sbs                      | Cropped SBS   | 2     | 0.5 x 0.5 | 1x2   |
 * | squeezed / squeezed_sbs  | Squeezed SBS  | 2     | 0.5 x 1.0 | 2x1   |
 * | quad                     | Quad          | 4     | 0.5 x 0.5 | 2x2   |
 * | interlaced / interlace   | Interlaced    | 2     | 0.5 x 0.5 | 2x1   |
 * | blend                    | Blend         | 2     | 0.5 x 0.5 | 2x1   |
 * | 2d / passthrough         | Anaglyph      | 2     | 0.5 x 0.5 | 2x1   |
 *
 * `2d` / `passthrough` choose no kind (mode 1 is the default, anaglyph) and
 * force the start in 2D.
 *
 * Quad is the one kind with more than two views: an INSPECTION kind. A stereo
 * session cannot fill it, so the session-begin floor drops such a session to
 * 2D (and a request for mode 1 is denied); a PRIMARY_MULTIVIEW_DXR session
 * fills all four tiles. It is the only N-view path that runs without hardware.
 *
 * ## Start-up
 *
 * The device starts in **2D** (mode 0). `START_3D=1` starts it in mode 1
 * instead, except under OUTPUT=2d. `FORCE_MODE` (0 or 1) pins the mode for
 * every app; any other value is ignored with a WARN. All read once per
 * process, at device creation; the kind never changes while it lives.
 */

#pragma once

#include "xrt/xrt_device.h"

#include "sim_display_interface.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

//! One 3D kind: what mode 1 is when it is selected.
struct sim_display_kind_desc
{
	enum sim_display_output_mode output; //!< The DP output (shader) it drives.
	const char *mode_name;               //!< The mode-1 name apps enumerate.
	uint32_t view_count;
	float view_scale_x;
	float view_scale_y;
	uint32_t tile_columns;
	uint32_t tile_rows;
};

/*!
 * The descriptor of @p kind. Anything that is not a 3D kind (PASSTHROUGH, an
 * out-of-range value) answers with the default kind, anaglyph.
 */
static inline struct sim_display_kind_desc
sim_display_kind_desc_get(enum sim_display_output_mode kind)
{
	struct sim_display_kind_desc d;
	d.output = kind;
	d.view_count = 2;
	d.view_scale_x = 0.5f;
	d.view_scale_y = 0.5f;
	d.tile_columns = 2;
	d.tile_rows = 1;
	switch (kind) {
	case SIM_DISPLAY_OUTPUT_SBS:
		// Top-bottom half-res, center-cropped by the DP.
		d.mode_name = "Cropped SBS";
		d.tile_columns = 1;
		d.tile_rows = 2;
		break;
	case SIM_DISPLAY_OUTPUT_SQUEEZED_SBS:
		// Side-by-side, full height, tiles placed as-is.
		d.mode_name = "Squeezed SBS";
		d.view_scale_y = 1.0f;
		break;
	case SIM_DISPLAY_OUTPUT_QUAD:
		d.mode_name = "Quad";
		d.view_count = 4;
		d.tile_rows = 2;
		break;
	case SIM_DISPLAY_OUTPUT_INTERLACED:
		// #817: anaglyph's geometry (2 full-canvas views, position-preserving),
		// but the DP picks the view per PANEL COLUMN, so the output is sensitive
		// to the weave target's size and X origin like a real lenticular weave.
		d.mode_name = "Interlaced";
		break;
	case SIM_DISPLAY_OUTPUT_BLEND:
		// 50/50 mix of the two views, same geometry as anaglyph.
		d.mode_name = "Blend";
		break;
	case SIM_DISPLAY_OUTPUT_ANAGLYPH:
	default:
		d.output = SIM_DISPLAY_OUTPUT_ANAGLYPH;
		d.mode_name = "Anaglyph";
		break;
	}
	return d;
}

/*!
 * Parse the OUTPUT setting.
 *
 * @param str           The value (NULL / empty = unset).
 * @param[out] out_kind The 3D kind (anaglyph when unset, unknown or 2d).
 * @param[out] out_2d   True for `2d` / `passthrough`: no kind change, start in 2D.
 * @return False for a value that is not a known OUTPUT (the caller WARNs and
 *         keeps the anaglyph default); true otherwise, including unset.
 */
static inline bool
sim_display_kind_parse(const char *str, enum sim_display_output_mode *out_kind, bool *out_2d)
{
	*out_kind = SIM_DISPLAY_OUTPUT_ANAGLYPH;
	*out_2d = false;
	if (str == NULL || str[0] == '\0') {
		return true;
	}
	if (strcmp(str, "anaglyph") == 0) {
		*out_kind = SIM_DISPLAY_OUTPUT_ANAGLYPH;
	} else if (strcmp(str, "sbs") == 0) {
		*out_kind = SIM_DISPLAY_OUTPUT_SBS;
	} else if (strcmp(str, "squeezed") == 0 || strcmp(str, "squeezed_sbs") == 0) {
		*out_kind = SIM_DISPLAY_OUTPUT_SQUEEZED_SBS;
	} else if (strcmp(str, "quad") == 0) {
		*out_kind = SIM_DISPLAY_OUTPUT_QUAD;
	} else if (strcmp(str, "interlaced") == 0 || strcmp(str, "interlace") == 0) {
		*out_kind = SIM_DISPLAY_OUTPUT_INTERLACED;
	} else if (strcmp(str, "blend") == 0) {
		*out_kind = SIM_DISPLAY_OUTPUT_BLEND;
	} else if (strcmp(str, "2d") == 0 || strcmp(str, "passthrough") == 0) {
		*out_2d = true;
	} else {
		return false;
	}
	return true;
}

//! Fill a rendering mode from its parts (tiling fields are the runtime's).
static inline void
sim_display_mode_fill(struct xrt_rendering_mode *m,
                      uint32_t index,
                      const char *name,
                      uint32_t views,
                      float sx,
                      float sy,
                      bool hw3d,
                      uint32_t cols,
                      uint32_t rows)
{
	memset(m, 0, sizeof(*m));
	m->mode_index = index;
	snprintf(m->mode_name, sizeof(m->mode_name), "%s", name);
	m->view_count = views;
	m->view_scale_x = sx;
	m->view_scale_y = sy;
	m->hardware_display_3d = hw3d;
	m->tile_columns = cols;
	m->tile_rows = rows;
}

/*!
 * The two run-time modes for @p kind: `[0]` = 2D, `[1]` = the kind.
 * mode_flags are left zero (the caller adds HAS_TRACKING to mode 1 when it
 * fakes or really has tracking).
 */
static inline void
sim_display_fill_modes(enum sim_display_output_mode kind, struct xrt_rendering_mode out[2])
{
	sim_display_mode_fill(&out[0], 0, "2D", 1, 1.0f, 1.0f, false, 1, 1);
	const struct sim_display_kind_desc d = sim_display_kind_desc_get(kind);
	sim_display_mode_fill(&out[1], 1, d.mode_name, d.view_count, d.view_scale_x, d.view_scale_y, true,
	                      d.tile_columns, d.tile_rows);
}

//! What the device starts in, and whether it is pinned there.
struct sim_display_start
{
	uint32_t active;    //!< 0 (2D) or 1 (the kind).
	bool pinned;        //!< FORCE_MODE holds it against every request.
	bool force_ignored; //!< FORCE_MODE was set to something other than -1/0/1.
	bool start_3d_moot; //!< START_3D was asked for under OUTPUT=2d (ignored).
};

/*!
 * The start-up rule.
 *
 * @param output_2d   OUTPUT was `2d` / `passthrough`.
 * @param start_3d    START_3D is on.
 * @param force_mode  FORCE_MODE (-1 = unset).
 */
static inline struct sim_display_start
sim_display_resolve_start(bool output_2d, bool start_3d, long force_mode)
{
	struct sim_display_start s;
	s.pinned = false;
	s.force_ignored = false;
	s.start_3d_moot = output_2d && start_3d;
	s.active = (start_3d && !output_2d) ? 1u : 0u;
	if (force_mode == 0 || force_mode == 1) {
		s.active = (uint32_t)force_mode;
		s.pinned = true;
	} else if (force_mode != -1) {
		s.force_ignored = true;
	}
	return s;
}

#ifdef __cplusplus
}
#endif
