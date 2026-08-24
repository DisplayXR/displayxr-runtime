// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Weave scope: how much of the panel a display processor's output
 *         transform covers.
 *
 * Every display processor turns a tiled atlas into "what the panel wants".
 * They differ in **where that transform applies**, and the runtime cannot
 * infer it:
 *
 * - A GPU weaver runs the lens math itself, over the canvas it was handed.
 *   Its output is final pixels for that rectangle and the rest of the screen
 *   is untouched — so a windowed app is native and needs nothing special.
 *   This is @ref XRT_DP_WEAVE_SCOPE_CANVAS, and it is the default for every
 *   plug-in that does not implement the caps slot.
 *
 * - A display that weaves in its own hardware (an FPGA or ASIC on the panel's
 *   scaler board) is not handed final pixels at all: the DP hands it a
 *   *packed* frame (the atlas repacked into the layout the chip expects — the
 *   tile geometry the plug-in already declares in @ref xrt_rendering_mode) and
 *   the chip does the weave during scanout. Two sub-cases, and they have
 *   completely different windowing consequences:
 *
 *   - The chip accepts a rectangle ("this region carries a packed pair, weave
 *     only there, pass the rest through"). Windowed 3D works exactly as it
 *     does for a GPU weaver — the DP writes the packed frame into the canvas
 *     and forwards the panel-absolute rect over its own sideband channel.
 *     This is @ref XRT_DP_WEAVE_SCOPE_REGION, and it is the same shape as an
 *     ADR-027 zones DP: `zone_grid_*` is the chip's region granularity and
 *     `snap_window_rect` is its placement alignment.
 *
 *   - The chip transforms the WHOLE incoming frame and has no notion of a
 *     rectangle. Then a windowed app is not expressible: everything else on
 *     the panel — desktop, taskbar, other windows — is de-packed along with
 *     it. Such a DP is only correct when the runtime owns the entire scanout
 *     (the panel-native fullscreen composition the service compositor already
 *     performs). This is @ref XRT_DP_WEAVE_SCOPE_SCANOUT.
 *
 * The runtime uses the scope to pick a path and, for SCANOUT, to say plainly
 * in the log why a windowed session cannot look right — instead of presenting
 * a frame that a global-scanout chip will shred into crosstalk with no
 * diagnostic anywhere.
 *
 * Consumed through the per-API `get_scanout_caps` slot on each
 * `xrt_display_processor_<api>` vtable. Absent slot, NULL, or a `false`
 * return all mean @ref XRT_DP_WEAVE_SCOPE_CANVAS — so no existing plug-in
 * changes behaviour, needs a rebuild, or needs to declare anything.
 *
 * @author David Fattal
 * @ingroup xrt_iface
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h> // offsetof — used by the ABI tripwire at the end of this header

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * How much of the panel a DP's output transform covers. Stored in
 * @ref xrt_dp_scanout_caps::weave_scope as a `uint32_t` so no enum-width
 * assumption crosses the plug-in ABI.
 *
 * @ingroup xrt_iface
 */
enum xrt_dp_weave_scope
{
	/*! The DP produces final pixels for the canvas it is handed and nothing
	 *  outside it is affected. Every GPU weaver. **The default** — an absent
	 *  or NULL `get_scanout_caps` slot means exactly this. */
	XRT_DP_WEAVE_SCOPE_CANVAS = 0,

	/*! Hardware weaver that accepts a region descriptor: the DP emits a
	 *  packed frame into the canvas and tells its hardware which panel
	 *  rectangle carries it. Windowed output is correct. */
	XRT_DP_WEAVE_SCOPE_REGION = 1,

	/*! Hardware weaver that transforms the entire scanout and takes no
	 *  rectangle. Correct output requires the runtime to own the whole
	 *  panel (fullscreen, panel-native composition); a windowed session
	 *  cannot be made correct by the DP. */
	XRT_DP_WEAVE_SCOPE_SCANOUT = 2,
};

/*!
 * Scanout capability descriptor, filled by a DP's `get_scanout_caps`.
 *
 * Forward-compat mirrors @ref xrt_dp_local_zone_caps exactly: the CALLER (the
 * runtime) pre-sets @ref struct_size to its own `sizeof`, and the plug-in
 * writes only fields that fall within it. A plug-in built against a newer
 * header keeps working with an older runtime as long as it accepts any caller
 * `struct_size >= XRT_DP_SCANOUT_CAPS_SIZE_V1`. Growth is append-only at the
 * end (ADR-020) — the `reserved[]` words exist so the first few additions cost
 * no size change at all.
 *
 * @ingroup xrt_iface
 */
struct xrt_dp_scanout_caps
{
	//! `sizeof(struct xrt_dp_scanout_caps)` at the RUNTIME's compile time.
	//! Set by the caller before `get_scanout_caps`; the plug-in writes only
	//! fields within it.
	uint32_t struct_size;

	//! @ref xrt_dp_weave_scope, as `uint32_t` for a fixed-width ABI. A value
	//! the runtime does not recognise is treated as
	//! @ref XRT_DP_WEAVE_SCOPE_CANVAS (see @ref xrt_dp_weave_scope_clamp).
	uint32_t weave_scope;

	//! Reserved for future scanout capabilities. Plug-ins MUST write 0.
	uint32_t reserved[6];
};

/*!
 * Size of the V1 caps shape. Plug-ins accept any caller `struct_size >= ` this
 * and reject only callers older than the slot itself.
 */
#define XRT_DP_SCANOUT_CAPS_SIZE_V1 32

#ifndef XRT_DP_ABI_ASSERT
#if defined(__cplusplus)
#define XRT_DP_ABI_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define XRT_DP_ABI_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif
#endif

// V1-size tripwire: if this trips you grew or reordered the struct mid-way —
// that is an ABI break (ADR-020). Append at the end, inside reserved[].
XRT_DP_ABI_ASSERT(sizeof(struct xrt_dp_scanout_caps) == XRT_DP_SCANOUT_CAPS_SIZE_V1,
                  "xrt_dp_scanout_caps layout changed — append-only (ADR-020)");

/*!
 * Pre-set @p caps for a `get_scanout_caps` call: zero it and stamp the
 * caller's `struct_size`. Every call site uses this so the convention is
 * stated once.
 *
 * @ingroup xrt_iface
 */
static inline void
xrt_dp_scanout_caps_init(struct xrt_dp_scanout_caps *caps)
{
	for (size_t i = 0; i < sizeof(*caps); i++) {
		((char *)caps)[i] = 0;
	}
	caps->struct_size = (uint32_t)sizeof(*caps);
}

/*!
 * Normalise a raw `weave_scope` word to a scope this runtime understands.
 * A plug-in built against a NEWER header may report a scope this runtime has
 * never heard of; the safe reading of "unknown scope" is the default one,
 * because that is the behaviour every path already implements.
 *
 * @ingroup xrt_iface
 */
static inline enum xrt_dp_weave_scope
xrt_dp_weave_scope_clamp(uint32_t raw)
{
	switch (raw) {
	case XRT_DP_WEAVE_SCOPE_REGION: return XRT_DP_WEAVE_SCOPE_REGION;
	case XRT_DP_WEAVE_SCOPE_SCANOUT: return XRT_DP_WEAVE_SCOPE_SCANOUT;
	default: return XRT_DP_WEAVE_SCOPE_CANVAS;
	}
}

/*!
 * Short lowercase name for logging. Never NULL.
 *
 * @ingroup xrt_iface
 */
static inline const char *
xrt_dp_weave_scope_name(enum xrt_dp_weave_scope scope)
{
	switch (scope) {
	case XRT_DP_WEAVE_SCOPE_REGION: return "region";
	case XRT_DP_WEAVE_SCOPE_SCANOUT: return "scanout";
	case XRT_DP_WEAVE_SCOPE_CANVAS:
	default: return "canvas";
	}
}

/*!
 * True when this scope needs the runtime to own the entire panel — i.e. a
 * windowed, canvas-scoped presentation cannot be made correct for it.
 *
 * @ingroup xrt_iface
 */
static inline bool
xrt_dp_weave_scope_needs_panel(enum xrt_dp_weave_scope scope)
{
	return scope == XRT_DP_WEAVE_SCOPE_SCANOUT;
}

#ifdef __cplusplus
}
#endif
