// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Format-honest colour policy (#1589 / #1610): the two pure decisions.
 *
 * ADR-021 §6 says the swapchain FORMAT is the source of truth for a
 * swapchain's encoding state: `*_SRGB` ⟺ display-referred (encoded) bytes,
 * `*_UNORM` ⟺ scene-linear values. OpenXR says the same thing. The runtime
 * used to assume every colour swapchain held encoded bytes ("Model A
 * passthrough"), which made an honest UNORM app look ~2.2x too dark, and it
 * composited LAYERS on top of each other in that encoded space, which the
 * spec's linear-space blend rule forbids.
 *
 * The fix is a property of the RENDER TARGET, not of a shader: layers are
 * composed into a runtime-private target that carries an `_SRGB` view, so the
 * hardware decodes each `_SRGB` source on sample, blends in linear, and
 * encodes once on write. There is deliberately NO gamma arithmetic in any
 * compose shader — a manual encode would double-apply the moment the target
 * encodes on write. The composed result is then COPIED (same typeless family,
 * a bit reinterpretation) into the atlas, so the display processor still
 * receives ENCODED bytes and `set_atlas_encoding` is untouched.
 *
 * This header holds only the parts that are pure functions, so they can be
 * pinned without a GPU: the transfer function that defines the expected atlas
 * BYTES, the escape hatch, and the fast-path predicate. It is deliberately
 * BACKEND-NEUTRAL — plain C, no graphics types, no platform headers — because
 * every native compositor (D3D11, D3D12, Vulkan, Metal, OpenGL) makes the same
 * two decisions and must make them identically. Anything that needs a DXGI /
 * VkFormat / MTLPixelFormat belongs in that backend's own format header.
 *
 * @ingroup aux_util
 */

#pragma once

#include "util/u_debug.h"
#include "util/u_logging.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Standard sRGB OETF (linear → display-referred), IEC 61966-2-1.
 *
 * NOT used by any render path — the hardware `_SRGB` render target applies
 * this curve, and duplicating it in a shader is the double-apply bug this
 * work exists to avoid. It is here so the numerical ORACLE has one testable
 * definition: this is the curve the atlas-capture check reads back.
 */
static inline float
u_color_srgb_encode(float linear)
{
	if (linear <= 0.0f) {
		return 0.0f;
	}
	if (linear >= 1.0f) {
		return 1.0f;
	}
	if (linear <= 0.0031308f) {
		return linear * 12.92f;
	}
	return 1.055f * powf(linear, 1.0f / 2.4f) - 0.055f;
}

/*!
 * The 8-bit atlas byte a linear value must land on: `0.0 → 0`, `0.2 → 124`,
 * `0.5 → 188`, `1.0 → 255`. These four are the #1589 acceptance numbers.
 */
static inline int
u_color_srgb_encode_u8(float linear)
{
	return (int)(u_color_srgb_encode(linear) * 255.0f + 0.5f);
}

/*!
 * Is the transitional escape hatch on? Read ONCE, then cached.
 *
 * `DXR_COLOR_LEGACY_UNORM_ENCODED=1` restores the pre-#1589 behaviour
 * WHOLESALE: UNORM sources are treated as already-encoded, sources are
 * sampled through their non-decoding views, no private compose target is
 * created and nothing blends in linear. For the app population that cannot be
 * migrated. Transitional — to be deleted two releases after the flip ships.
 *
 * Spelled out rather than built with DEBUG_GET_ONCE_BOOL_OPTION because this
 * header is included by every backend: the macro expands to a plain (not
 * inline) static function, which a TU that includes the header without
 * calling this warns about under -Wunused-function.
 */
static inline bool
u_color_legacy_unorm_encoded(void)
{
	static int cached = -1;
	if (cached < 0) {
		cached = debug_get_bool_option("DXR_COLOR_LEGACY_UNORM_ENCODED", false) ? 1 : 0;
	}
	return cached != 0;
}

/*!
 * THE fast-path predicate, as a pure function of the frame.
 *
 * A frame whose tile is painted by exactly ONE full-tile projection-class
 * layer out of an `_SRGB` swapchain needs neither of the two things the
 * compose target provides: no encode is owed (the app already encoded) and
 * nothing blends. Such a frame takes the pre-existing raw path verbatim, so
 * the atlas is byte-identical to before this work — which is both the
 * performance guard for the shipping app population and the no-regression
 * proof. A single UNORM source does NOT qualify: it is linear and owes the
 * encode.
 *
 * @param legacy_hatch          @ref u_color_legacy_unorm_encoded.
 * @param contributing_layers   Layers that will actually paint this frame
 *                              (types the backend draws; a layer type it
 *                              only warns about does not count).
 * @param base_is_projection    The single contributing layer is a full-tile
 *                              projection / projection-depth blit. A quad, a
 *                              zone or a Local2D layer covers a SUB-RECT and
 *                              blends over the clear, so it is a compose
 *                              case even when alone.
 * @param all_sources_srgb      Every source swapchain this frame reads is
 *                              `*_SRGB`.
 */
static inline bool
u_color_compose_fast_path(bool legacy_hatch,
                          uint32_t contributing_layers,
                          bool base_is_projection,
                          bool all_sources_srgb)
{
	if (legacy_hatch) {
		return true;
	}
	return contributing_layers == 1 && base_is_projection && all_sources_srgb;
}

/*!
 * One WARN per component at init, stating the hatch state either way.
 *
 * Logged unconditionally (not only when the hatch is on) because it is the
 * line a hardware check reads to prove which colour regime a session ran
 * under. One-off init event, never per frame.
 *
 * @param where Component tag, e.g. "d3d11" / "d3d11_service".
 */
static inline void
u_color_log_state_once(const char *where)
{
	static bool logged = false;
	if (logged) {
		return;
	}
	logged = true;
	if (u_color_legacy_unorm_encoded()) {
		U_LOG_W("Color (#1589) [%s]: DXR_COLOR_LEGACY_UNORM_ENCODED=1 — LEGACY: UNORM swapchains "
		        "treated as already encoded, no private compose target, layers blend in encoded "
		        "space. Transitional; migrate the app to an _SRGB swapchain.",
		        where != NULL ? where : "?");
	} else {
		U_LOG_W("Color (#1589) [%s]: format-honest — _SRGB sources decode on sample, UNORM sources "
		        "are linear, layers compose in an _SRGB-view private target and the atlas stays "
		        "ENCODED. Set DXR_COLOR_LEGACY_UNORM_ENCODED=1 to restore the old behaviour.",
		        where != NULL ? where : "?");
	}
}

#ifdef __cplusplus
}
#endif
