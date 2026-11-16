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

	//! `XRT_DP_SCANOUT_FLAG_*` (multi-screen M2). Carved out of the former
	//! `reserved[0]`: same size and offsets, and a plug-in built before the
	//! field existed wrote 0 here — which is the conservative default (the
	//! weave needs 1:1 pixels), so nothing changes for it.
	uint32_t flags;

	//! @ref xrt_dp_stereo_layout, as `uint32_t` for a fixed-width ABI: what the
	//! DP's output is, for a stereo desktop compositor. Carved out of the first
	//! word of `reserved[]`: same size and offsets, and a plug-in built before
	//! the field existed wrote 0 here, an ordinary weave, so nothing changes
	//! for it. A value the runtime does not recognise is read as 0 (see
	//! @ref xrt_dp_stereo_layout_clamp), the way `weave_scope` is clamped.
	uint32_t stereo_layout;

	//! Reserved for future scanout capabilities. Plug-ins MUST write 0.
	uint32_t reserved[4];
};

/*!
 * @ref xrt_dp_scanout_caps::flags: the DP's output survives the display
 * server resampling the window (a fractionally scaled desktop, an X11 window
 * under XWayland at a non-integer scale). True for outputs that are not
 * pixel-period patterns — anaglyph, side-by-side, blend, passthrough. False
 * (the default, bit clear) for a lenticular weave or any 1-pixel-period
 * interlace, which a resample turns into a uniform double image.
 *
 * The runtime's refuse-rather-than-resample gates (#1595 Wayland, #1831 X11,
 * and the per-segment gate of multi-screen M2) degrade a session or a segment
 * to flat 2D only when the DP does NOT set this bit.
 *
 * @ingroup xrt_iface
 */
#define XRT_DP_SCANOUT_FLAG_TOLERATES_RESAMPLE (1u << 0)

/*!
 * @ref xrt_dp_scanout_caps::flags: THIS instance weaves a window correctly
 * without holding it. Its output is right at any position it is handed
 * through `set_present_origin` (+ the canvas offset): either its phase comes
 * from that origin (a vendor weaver routed to the caller, ADR-033), or its
 * output has no position-dependent phase at all (side-by-side, anaglyph). Set
 * per INSTANCE, because a vendor may fall back per create (e.g. an SDK that
 * refuses external routing yields a weaver that reads only its own window and
 * ignores the origin — which must leave this bit clear).
 *
 * The runtime makes a windowless DP for a window it does not own (the D3D11
 * service's present-owner, ADR-060 R2; the texture target of ADR-060) only
 * when the instance sets this bit, and otherwise falls back to a DP bound to
 * the window. Bit clear (the default, every plug-in built before the bit) =
 * "do not use me windowless".
 *
 * @ingroup xrt_iface
 */
#define XRT_DP_SCANOUT_FLAG_PHASE_FROM_PRESENT_ORIGIN (1u << 1)

/*!
 * What a DP's output is, for a stereo desktop compositor. Stored in
 * @ref xrt_dp_scanout_caps::stereo_layout as a `uint32_t`.
 *
 * The values are the 3D_Structure numbers of HDMI 1.4b (the 3D_Structure field of
 * the HDMI Vendor-Specific InfoFrame, section 8.2.3; the same numbers the Linux
 * kernel names in `enum hdmi_3d_structure`, include/linux/hdmi.h), so one number
 * names one layout in every API that carries it. Only the values this runtime acts on
 * are named; any other value is read as @ref XRT_DP_STEREO_LAYOUT_WEAVE.
 *
 * **0 is not HDMI's 0.** In HDMI 1.4b the value 0 is 3D_Structure "Frame
 * packing". Here 0 means "an ordinary weave, nothing for a stereo compositor",
 * because that is what every plug-in built before this field already writes
 * there. The runtime acts on no frame packing output today; whoever adds one
 * gives it a value of its own (or moves this field to a version of its own)
 * instead of reusing 0.
 *
 * @ingroup xrt_iface
 */
enum xrt_dp_stereo_layout
{
	/*! An ordinary weave (or anything else the window shows as it is). **The
	 *  default**: a plug-in built before the field, an absent or NULL
	 *  `get_scanout_caps` slot, and every value this runtime does not know. */
	XRT_DP_STEREO_LAYOUT_WEAVE = 0,

	/*! HDMI 1.4b 3D_Structure value 3, "Side-by-Side (Full)": THIS instance's
	 *  output, in its current mode, is the views placed side by side, view 0
	 *  (the left eye) first, each at the resolution its tile was rendered at,
	 *  untouched. The DP keeps no knowledge of the screen: a stereo desktop
	 *  compositor resizes the views once and turns them into what each output
	 *  needs (a 3D television's HDMI 3D mode, anaglyph on a 2D monitor, the
	 *  left view alone on a plain 2D output).
	 *
	 *  When the DP reports it and the window system offers a stereo
	 *  declaration, the runtime presents a target as wide as the views laid
	 *  side by side (twice the window's width for two views) and declares the
	 *  window as holding them, once; the window keeps its size on screen. Where
	 *  no declaration is offered the target stays the window's size and the
	 *  runtime says so in its log, so the window shows the two views side by
	 *  side, flat.
	 *
	 *  Set per INSTANCE and per MODE (re-read when the rendering mode changes,
	 *  like @ref XRT_DP_SCANOUT_FLAG_TOLERATES_RESAMPLE). */
	XRT_DP_STEREO_LAYOUT_SIDE_BY_SIDE_FULL = 3,
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
 * Normalise a raw `stereo_layout` word to a layout this runtime understands.
 * A plug-in built against a NEWER header may report a layout this runtime has
 * never heard of; the safe reading of "unknown layout" is 0, an ordinary
 * weave, because that is the behaviour every path already implements.
 *
 * @ingroup xrt_iface
 */
static inline enum xrt_dp_stereo_layout
xrt_dp_stereo_layout_clamp(uint32_t raw)
{
	switch (raw) {
	case XRT_DP_STEREO_LAYOUT_SIDE_BY_SIDE_FULL: return XRT_DP_STEREO_LAYOUT_SIDE_BY_SIDE_FULL;
	default: return XRT_DP_STEREO_LAYOUT_WEAVE;
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
