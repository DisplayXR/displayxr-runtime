// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_display's declared rendering-mode list and its config (ADR-066
 *         C1): the runtime keeps the whole list as the sizing envelope and
 *         shows apps two slots, 0 = 2D and 1 = the entry OUTPUT names.
 * @ingroup drv_sim_display
 *
 * Header-only and pure (no settings reads, no logging), so the list and the
 * start-up rule are unit-testable without the driver (tests_sim_display_modes).
 *
 * ## The declared list (the envelope)
 *
 * | idx | name          | views | scale     | tiles | OUTPUT values               |
 * |-----|---------------|-------|-----------|-------|-----------------------------|
 * | 0   | 2D            | 1     | 1.0 x 1.0 | 1x1   | (slot 0, never configurable)|
 * | 1   | Same as 2D    | 1     | 1.0 x 1.0 | 1x1   | default (unset), same_as_2d, 2d, passthrough |
 * | 2   | Anaglyph      | 2     | 0.5 x 0.5 | 2x1   | anaglyph                    |
 * | 3   | Cropped SBS   | 2     | 0.5 x 0.5 | 1x2   | sbs                         |
 * | 4   | Squeezed SBS  | 2     | 0.5 x 1.0 | 2x1   | squeezed, squeezed_sbs      |
 * | 5   | Interlaced    | 2     | 0.5 x 0.5 | 2x1   | interlaced, interlace       |
 * | 6   | Blend         | 2     | 0.5 x 0.5 | 2x1   | blend                       |
 * | 7   | Quad          | 4     | 0.5 x 0.5 | 2x2   | quad (declared only when OUTPUT=quad at start-up; listed
 * START_UP_ONLY otherwise) |
 *
 * "Same as 2D" is a passthrough 3D entry (`XRT_RENDERING_MODE_FLAG_3D_PASSTHROUGH`,
 * `hardware_display_3d` false): an app in its 3D slot is shown one sharp,
 * full-resolution view and the lens stays 2D. It is the DEFAULT config
 * (project lead, 2026-11-23): most apps ask for 3D at start, and an anaglyph
 * on an ordinary screen looks broken, so a 3D look is chosen on purpose.
 * Before ADR-066 the default was anaglyph, and `OUTPUT=2d|passthrough` meant
 * "anaglyph, start in 2D"; they now name the Same-as-2D config.
 *
 * The OUTPUT setting is the config: it names which declared entry slot 1 maps
 * to, and it applies LIVE: the runtime polls `XRT_DEVICE_PROPERTY_CONFIGURED_3D_MODE`
 * and remaps slot 1 without a restart, for every entry of the list.
 *
 * ## Quad, the interim rule
 *
 * The located view count of `PRIMARY_MULTIVIEW_DXR` is the maximum over the
 * declared list, fixed per instance. A 4-view entry in the list would make
 * every multiview session locate 4 views on a box with no 3D display, and an
 * app that submits only the active count would be refused at the default
 * `DXR_UNDER_SUBMIT`. So Quad is declared only when OUTPUT=quad at start-up
 * (it then behaves as before ADR-066). Otherwise it is still LISTED, last, with
 * `XRT_RENDERING_MODE_FLAG_START_UP_ONLY`: the runtime leaves it out of the
 * sizing envelope and reports it as `applies: restart_app`, and a live switch
 * TO quad applies at the next start. Pending the project lead's decision
 * (ADR-066 open question 2).
 *
 * ## Start-up
 *
 * The driver does not choose an app's start mode (ADR-065 Amendment 1, project
 * lead 2026-11-26): the device starts in **slot 1**, the 3D slot, like every
 * hardware driver, and the APP picks with `xrRequestDisplayRenderingModeDXR`
 * (or the user with V / 0 / 1). With the default Same-as-2D config slot 1 is one
 * full-resolution passthrough view, so a PC with no 3D display still looks 2D.
 * `START_3D` is deprecated and has no effect (one WARN when it is set).
 * `FORCE_MODE` (0 or 1) stays the developer pin for every app; any other value
 * is ignored with a WARN. FORCE_MODE is read once per process.
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

//! One declared entry: what slot 1 is when this kind is the config.
struct sim_display_kind_desc
{
	enum sim_display_output_mode output; //!< The DP output (shader) it drives.
	const char *mode_name;               //!< The name apps enumerate for slot 1.
	uint32_t view_count;
	float view_scale_x;
	float view_scale_y;
	uint32_t tile_columns;
	uint32_t tile_rows;
	bool passthrough_3d; //!< One full-resolution view serving as the 3D slot.
	bool hardware_3d;    //!< hardware_display_3d of the entry.
};

/*!
 * The descriptor of @p kind. PASSTHROUGH is the Same-as-2D entry; an
 * out-of-range value answers with it too (the default config).
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
	d.passthrough_3d = false;
	d.hardware_3d = true;
	switch (kind) {
	case SIM_DISPLAY_OUTPUT_ANAGLYPH: d.mode_name = "Anaglyph"; break;
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
	case SIM_DISPLAY_OUTPUT_PASSTHROUGH:
	default:
		// ADR-066 C1.2: one full-resolution view in the 3D slot; the lens
		// stays 2D. Position-preserving, like anaglyph, so zones compose.
		d.output = SIM_DISPLAY_OUTPUT_PASSTHROUGH;
		d.mode_name = "Same as 2D";
		d.view_count = 1;
		d.view_scale_x = 1.0f;
		d.view_scale_y = 1.0f;
		d.tile_columns = 1;
		d.tile_rows = 1;
		d.passthrough_3d = true;
		d.hardware_3d = false;
		break;
	}
	return d;
}

//! The default config (OUTPUT unset): Same as 2D (project lead, 2026-11-23).
#define SIM_DISPLAY_DEFAULT_KIND SIM_DISPLAY_OUTPUT_PASSTHROUGH

/*!
 * Parse the OUTPUT setting (the config).
 *
 * @param str           The value (NULL / empty = unset).
 * @param[out] out_kind The config's slot-1 kind (Same as 2D when unset or
 *                      unknown; `default`, `same_as_2d`, `2d` and
 *                      `passthrough` name it explicitly).
 * @return False for a value that is not a known OUTPUT (the caller WARNs and
 *         keeps the default); true otherwise, including unset.
 */
static inline bool
sim_display_kind_parse(const char *str, enum sim_display_output_mode *out_kind)
{
	*out_kind = SIM_DISPLAY_DEFAULT_KIND;
	if (str == NULL || str[0] == '\0') {
		return true;
	}
	if (strcmp(str, "default") == 0 || strcmp(str, "same_as_2d") == 0 || strcmp(str, "2d") == 0 ||
	    strcmp(str, "passthrough") == 0) {
		*out_kind = SIM_DISPLAY_OUTPUT_PASSTHROUGH;
	} else if (strcmp(str, "anaglyph") == 0) {
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
 * THE seam for per-mode tracking (ADR-066 Part B's per-screen tri-state Off /
 * In 3D only / Always, owned by a separate change): is declared entry
 * @p entry_index tracked, when a tracking source is on (@p tracking_on: the
 * fake-tracking or webcam knob today)? Today every 3D entry is tracked with a
 * source and none without; the 2D entry never is. The tri-state will decide
 * here, per entry, so nothing in the list hardcodes "untracked".
 */
static inline bool
sim_display_entry_tracked(uint32_t entry_index, bool tracking_on)
{
	return tracking_on && entry_index >= 1;
}

//! The declared kinds in list order (index 0 is the 2D entry, PASSTHROUGH as a placeholder).
static const enum sim_display_output_mode sim_display_declared_order[] = {
    SIM_DISPLAY_OUTPUT_PASSTHROUGH, // 0: 2D (not a kind)
    SIM_DISPLAY_OUTPUT_PASSTHROUGH, // 1: Same as 2D
    SIM_DISPLAY_OUTPUT_ANAGLYPH,    SIM_DISPLAY_OUTPUT_SBS,   SIM_DISPLAY_OUTPUT_SQUEEZED_SBS,
    SIM_DISPLAY_OUTPUT_INTERLACED,  SIM_DISPLAY_OUTPUT_BLEND, SIM_DISPLAY_OUTPUT_QUAD,
};

/*!
 * Fill sim_display's declared list (the table above).
 *
 * @param include_quad   Declare the 4-view Quad entry (only when OUTPUT=quad
 *                       at start-up: the interim rule); otherwise it is listed
 *                       last with XRT_RENDERING_MODE_FLAG_START_UP_ONLY.
 * @param tracking_on    A tracking source is on (see @ref sim_display_entry_tracked).
 * @param out            XRT_MAX_RENDERING_MODES entries.
 * @param out_kinds      Optional: each entry's kind (index 0 = PASSTHROUGH).
 * @return The listed count (8).
 */
static inline uint32_t
sim_display_fill_declared(bool include_quad,
                          bool tracking_on,
                          struct xrt_rendering_mode out[XRT_MAX_RENDERING_MODES],
                          enum sim_display_output_mode *out_kinds)
{
	const uint32_t n = 8u;
	sim_display_mode_fill(&out[0], 0, "2D", 1, 1.0f, 1.0f, false, 1, 1);
	if (out_kinds != NULL) {
		out_kinds[0] = SIM_DISPLAY_OUTPUT_PASSTHROUGH;
	}
	for (uint32_t i = 1; i < n; i++) {
		const struct sim_display_kind_desc d = sim_display_kind_desc_get(sim_display_declared_order[i]);
		sim_display_mode_fill(&out[i], i, d.mode_name, d.view_count, d.view_scale_x, d.view_scale_y,
		                      d.hardware_3d, d.tile_columns, d.tile_rows);
		if (d.passthrough_3d) {
			out[i].mode_flags |= XRT_RENDERING_MODE_FLAG_3D_PASSTHROUGH;
		}
		if (sim_display_entry_tracked(i, tracking_on)) {
			out[i].mode_flags |= XRT_RENDERING_MODE_FLAG_HAS_TRACKING;
		}
		if (d.output == SIM_DISPLAY_OUTPUT_QUAD && !include_quad) {
			out[i].mode_flags |= XRT_RENDERING_MODE_FLAG_START_UP_ONLY;
		}
		if (out_kinds != NULL) {
			out_kinds[i] = d.output;
		}
	}
	return n;
}

/*!
 * The declared index of @p kind's entry (>= 1), or 0 when @p kind is not in a
 * list of @p count entries (Quad in a list declared without it).
 */
static inline uint32_t
sim_display_entry_for_kind(enum sim_display_output_mode kind, uint32_t count)
{
	for (uint32_t i = 1;
	     i < count && i < sizeof(sim_display_declared_order) / sizeof(sim_display_declared_order[0]); i++) {
		if (sim_display_declared_order[i] == kind) {
			return i;
		}
	}
	return 0;
}

/*!
 * The output the device is in now: passthrough in slot 0, the slot-1 kind in
 * slot 1 - from the ACTIVE MODE, which a runtime writer may set without an
 * OUTPUT_MODE request. With no device (a bare display processor) the last
 * request stands. sim_display_resolved_output() is this, on the live device.
 */
static inline enum sim_display_output_mode
sim_display_output_resolve(bool have_device,
                           uint32_t active_mode,
                           enum sim_display_output_mode kind,
                           enum sim_display_output_mode last_request)
{
	if (!have_device) {
		return last_request;
	}
	return active_mode == 0 ? SIM_DISPLAY_OUTPUT_PASSTHROUGH : kind;
}

/*!
 * What a display processor draws for an atlas of @p tile_count tiles:
 * passthrough for one tile (3D shaders need two views), otherwise the resolved
 * output - and the kind when that is still passthrough (the one frame a
 * multi-tile atlas lands after a switch back to 2D). With the Same-as-2D
 * config the kind IS passthrough: a multi-tile atlas that lands for a frame
 * after a live switch to it is shown as passthrough of its first tile region.
 */
static inline enum sim_display_output_mode
sim_display_output_draw(uint32_t tile_count, enum sim_display_output_mode resolved, enum sim_display_output_mode kind)
{
	if (tile_count <= 1) {
		return SIM_DISPLAY_OUTPUT_PASSTHROUGH;
	}
	return resolved == SIM_DISPLAY_OUTPUT_PASSTHROUGH ? kind : resolved;
}

//! What the device starts in, and whether it is pinned there.
struct sim_display_start
{
	uint32_t active;    //!< 0 (2D) or 1 (slot 1).
	bool pinned;        //!< FORCE_MODE holds it against every request.
	bool force_ignored; //!< FORCE_MODE was set to something other than -1/0/1.
};

/*!
 * The start-up rule: slot 1 (the 3D slot) unless FORCE_MODE pins 0 or 1. The
 * driver never decides an app's start mode; the app requests one (ADR-065
 * Amendment 1).
 *
 * @param force_mode  FORCE_MODE (-1 = unset).
 */
static inline struct sim_display_start
sim_display_resolve_start(long force_mode)
{
	struct sim_display_start s;
	s.pinned = false;
	s.force_ignored = false;
	s.active = 1u;
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
