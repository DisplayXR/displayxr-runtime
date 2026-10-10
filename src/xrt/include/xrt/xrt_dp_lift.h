// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Display-processor 2D→3D conversion ("lift") contract types (ADR-042).
 *
 * A vendor plug-in may ship a conversion module (monocular depth, stereo or
 * N-view synthesis, photo → Gaussian splats). The runtime reaches it through
 * optional appended slots on the per-API DP vtable — today only
 * @ref xrt_display_processor_d3d11 (lift_get_caps … lift_convert_blob, guarded
 * by XRT_DP_D3D11_HAS_LIFT; plus lift_get_depth, guarded by
 * XRT_DP_D3D11_HAS_LIFT_DEPTH) — and exposes it to apps as XR_DXR_lift.
 *
 * The split of labour (ADR-042, ADR-007):
 *  - the PLUG-IN converts, synchronously, one frame per call. It never weaves
 *    the result, never queues, never timestamps.
 *  - the RUNTIME owns everything around the call: the lift thread, the
 *    latest-wins mailbox, the output ring (a slot's output is valid only until
 *    that slot's next call), fences, timestamps, IPC, and weaving the SBS /
 *    N-view result on the ordinary weave path.
 *
 * Every struct starts with @c struct_size, set by the CALLER (the runtime for
 * caps / params / stream info), so either side may grow its struct by
 * appending fields without an ABI bump (ADR-020).
 *
 * @ingroup xrt_iface
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @name Lift mode bits (xrt_dp_lift_caps::modes, xrt_dp_lift_stream_info::mode)
 * Values match XR_LIFT_MODE_*_BIT_DXR.
 * @{
 */
#define XRT_DP_LIFT_MODE_DEPTH 1u     //!< monocular depth map
#define XRT_DP_LIFT_MODE_SBS 2u       //!< stereo, side by side, NOT woven
#define XRT_DP_LIFT_MODE_NVIEW 4u     //!< N views side by side in one row, NOT woven
#define XRT_DP_LIFT_MODE_GAUSSIANS 8u //!< photo → Gaussian splats, via lift_convert_blob
/*! @} */

/*!
 * @name Lift module state (xrt_dp_lift_caps::state)
 * Values are the DP-side encoding; XR_LIFT_STATE_*_DXR maps them 1:1 in name
 * (not in value — the OpenXR enum orders UNAVAILABLE=0, ACTIVATING=1, READY=2
 * too, so they do coincide; keep it that way).
 * @{
 */
#define XRT_DP_LIFT_STATE_UNAVAILABLE 0u
#define XRT_DP_LIFT_STATE_ACTIVATING 1u
#define XRT_DP_LIFT_STATE_READY 2u
/*! @} */

//! xrt_dp_lift_caps::depth_semantics
#define XRT_DP_LIFT_DEPTH_RELATIVE 0u
#define XRT_DP_LIFT_DEPTH_METRIC 1u

//! xrt_dp_lift_stream_info::content_hint
#define XRT_DP_LIFT_CONTENT_VIDEO 0u
#define XRT_DP_LIFT_CONTENT_PHOTO 1u

/*!
 * @name Blob formats (lift_convert_blob's out_format)
 * Values match XR_LIFT_BLOB_FORMAT_*_DXR.
 * @{
 */
#define XRT_DP_LIFT_BLOB_PLY_3DGS 1u //!< binary little-endian PLY, reference 3DGS layout
#define XRT_DP_LIFT_BLOB_SOG 2u      //!< PlayCanvas SOG container
/*! @} */

/*!
 * What the plug-in's conversion module can do. The runtime pre-sets
 * @c struct_size (zeroing the rest); the DP writes only fields within it.
 * Cheap: the runtime polls it from its lift thread (≤ 1 Hz while not READY).
 */
struct xrt_dp_lift_caps
{
	uint32_t struct_size;
	uint32_t modes; //!< bits: 1 DEPTH, 2 SBS, 4 NVIEW, 8 GAUSSIANS
	uint32_t max_streams;
	uint32_t max_views;
	uint32_t depth_semantics;    //!< 0 relative, 1 metric
	uint32_t state;              //!< 0 unavailable, 1 activating, 2 ready
	uint64_t typical_latency_ns; //!< submit→result as the module expects it; 0 = unknown
	char backend[32];            //!< NUL-terminated module name (informational)

	/*
	 * ── Auxiliary depth (ADR-048 Addendum A, XRT_DP_LIFT_HAS_AUX_DEPTH) ──────
	 * Appended; written only when struct_size covers them.
	 */

	//! XRT_DP_LIFT_AUX_* bits the module can return alongside SBS / NVIEW
	//! views (through xrt_display_processor_d3d11::lift_get_depth); 0 = none.
	uint32_t aux_outputs;
	//! Meaning of that auxiliary depth: XRT_DP_LIFT_DEPTH_RELATIVE / _METRIC.
	uint32_t aux_depth_semantics;
};

/*!
 * @name Auxiliary outputs (ADR-048 Addendum A)
 *
 * Announced by XRT_DP_LIFT_HAS_AUX_DEPTH (append-only, ADR-020: no ABI bump),
 * together with the D3D11 slot lift_get_depth (XRT_DP_D3D11_HAS_LIFT_DEPTH).
 * @{
 */
#define XRT_DP_LIFT_HAS_AUX_DEPTH 1

//! xrt_dp_lift_caps::aux_outputs / xrt_dp_lift_stream_info::aux_outputs bit:
//! the depth map of the same inference that produced the views.
#define XRT_DP_LIFT_AUX_DEPTH 1u

//! xrt_dp_lift_depth::encoding — values match XR_LIFT_DEPTH_ENCODING_*_DXR.
#define XRT_DP_LIFT_DEPTH_ENCODING_LINEAR 0u  //!< decoded value = depth
#define XRT_DP_LIFT_DEPTH_ENCODING_INVERSE 1u //!< decoded value = 1 / depth
/*! @} */

//! Stream creation parameters (runtime-filled).
struct xrt_dp_lift_stream_info
{
	uint32_t struct_size;
	uint32_t mode;         //!< ONE XRT_DP_LIFT_MODE_* bit
	uint32_t content_hint; //!< 0 video, 1 photo
	float input_scale;     //!< (0,1]: convert at reduced resolution; 1 = native

	/*
	 * ── Auxiliary outputs (ADR-048 Addendum A, XRT_DP_LIFT_HAS_AUX_DEPTH) ───
	 * Appended; read only when struct_size covers them.
	 */

	//! XRT_DP_LIFT_AUX_* bits the app asked for on this SBS / NVIEW stream
	//! (never set for DEPTH / GAUSSIANS). A module that sees
	//! XRT_DP_LIFT_AUX_DEPTH keeps each conversion's depth map for
	//! lift_get_depth — ideally from the same inference as the views.
	uint32_t aux_outputs;
	//! DXGI_FORMAT hint for the depth map (R32_FLOAT / R16_FLOAT); 0 = any.
	uint32_t aux_depth_format;
};

/*!
 * The auxiliary depth map of the conversion lift_convert just returned
 * (xrt_display_processor_d3d11::lift_get_depth). The runtime pre-sets
 * @c struct_size (zeroing the rest); the DP writes only fields within it.
 *
 * Vendor-neutral by construction: everything a consumer needs to place the
 * depth in metric space — the encoding of the samples, their unit, the
 * camera the module assumed, and the depth it put on the screen plane — and
 * nothing about how the module got there. The RUNTIME derives the display
 * alignment (XrLiftDepthResultDXR::depthToDisplay) from these plus the
 * viewpoints and rect it already owns; the plug-in only reports.
 *
 * Decoding (runtime and app): d = value_scale * sample + value_offset; depth =
 * d (LINEAR) or 1 / d (INVERSE), in @c units.
 */
struct xrt_dp_lift_depth
{
	uint32_t struct_size;
	//! DXGI_FORMAT of @c resource: DXGI_FORMAT_R32_FLOAT (41) or _R16_FLOAT (54).
	uint32_t format;
	//! ID3D11Resource* (a 2D texture) on the lift device, owned by the DP,
	//! valid until the NEXT lift_convert on this stream (the runtime copies it
	//! out before then). NULL = no depth for this conversion.
	void *resource;
	uint32_t width;  //!< texels
	uint32_t height; //!< texels
	//! XRT_DP_LIFT_DEPTH_RELATIVE / XRT_DP_LIFT_DEPTH_METRIC (metres).
	uint32_t units;
	//! XRT_DP_LIFT_DEPTH_ENCODING_*.
	uint32_t encoding;
	float value_scale;  //!< decode: d = value_scale * sample + value_offset (0 is read as 1)
	float value_offset; //!< (see value_scale)
	//! The input frame the depth refers to (w x h of the lift_convert call).
	uint32_t source_width;
	uint32_t source_height;
	//! Pinhole intrinsics the module assumed, in DEPTH-MAP pixels (u right,
	//! v down, origin top-left corner); focal <= 0 = unknown.
	float focal_x_px;
	float focal_y_px;
	float principal_x_px;
	float principal_y_px;
	//! Decoded depth range of this map, in @c units; 0 = unknown.
	float near_depth;
	float far_depth;
	//! The decoded depth (in @c units) the conversion put at ZERO disparity —
	//! on the screen plane — after resolving xrt_dp_lift_params::convergence
	//! (including AUTO). <= 0 = unknown (the runtime then reports no display
	//! transform).
	float convergence_depth;
	//! Non-zero = produced by the SAME inference as the views the preceding
	//! lift_convert returned (not a second pass, not a cached older frame).
	uint32_t same_inference;
	//! The module's own frame / inference counter, informational (logged).
	uint64_t vendor_frame_id;

	/*
	 * ── Relief mapping (ADR-046 Amendment 1, XRT_DP_LIFT_HAS_DEPTH_RELIEF) ──
	 * Appended; written only when struct_size covers them.
	 */

	//! Non-zero = @c relief_scale / @c relief_offset are valid for THIS
	//! conversion. 0 = the module renders without a physical display mapping
	//! (e.g. dimensionless disparity): the runtime then places nothing on depth.
	uint32_t relief_valid;
	//! Where the woven views PRESENT a texel, in metres in front of the screen
	//! plane (toward the viewer; screen = 0, behind = negative):
	//!   z = relief_scale * (1 / depth) + relief_offset
	//! with depth the texel's DECODED depth (see value_scale / encoding). Must
	//! reflect everything that moved the content this conversion — its
	//! convergence (including AUTO), relief thickness, strength — so it is
	//! re-reported on every lift_get_depth. The point lies on the ray from the
	//! midpoint of the conversion's viewpoints through the texel's position on
	//! the lifted rect.
	float relief_scale;
	float relief_offset; //!< metres; see relief_scale
};

/*!
 * xrt_dp_lift_depth carries a relief mapping (ADR-046 Amendment 1: the
 * depth-aware cursor on lifted content). Append-only, ADR-020: no ABI bump.
 */
#define XRT_DP_LIFT_HAS_DEPTH_RELIEF 1

/*!
 * @name Viewpoint policy (ADR-048) — xrt_dp_lift_params appended fields
 *
 * Announced by XRT_DP_LIFT_HAS_VIEWPOINT_POLICY (append-only, ADR-020: no ABI
 * bump). A plug-in built against it reads the fields only when
 * xrt_dp_lift_params::struct_size covers them; a runtime that predates them
 * passes a shorter struct_size and display-frame viewpoints (panel centre).
 * @{
 */
#define XRT_DP_LIFT_HAS_VIEWPOINT_POLICY 1

//! xrt_dp_lift_params::viewpoint_frame: viewpoints are relative to the PANEL
//! centre (the pre-ADR-048 contract; what a short struct_size implies).
#define XRT_DP_LIFT_VIEWPOINT_FRAME_DISPLAY 0u
//! xrt_dp_lift_params::viewpoint_frame: viewpoints are relative to the CENTRE
//! of the lifted rect (or the submitting window), display axes, metres.
#define XRT_DP_LIFT_VIEWPOINT_FRAME_RECT 1u

//! xrt_dp_lift_params::axis_mode — values match XR_LIFT_AXIS_MODE_*_DXR.
#define XRT_DP_LIFT_AXIS_X 1u   //!< horizontal look-around only (default)
#define XRT_DP_LIFT_AXIS_XY 2u  //!< + vertical
#define XRT_DP_LIFT_AXIS_XYZ 3u //!< + distance
/*! @} */

//! Upper bound on explicit viewpoints the runtime passes to lift_convert.
#define XRT_DP_LIFT_MAX_EXPLICIT_VIEWPOINTS 8

/*!
 * Per-frame conversion parameters (runtime-filled).
 *
 * @c focal_px (appended) is the input image's focal length in pixels; <= 0 =
 * unknown.
 *
 * @c convergence is the RELATIVE depth placed at the display plane (zero
 * disparity), normalised to [0, 1] over the frame's depth range: 0 = the
 * nearest content sits on the glass (everything else behind it), 1 = the
 * farthest does (everything pops out), 0.5 = the middle of the range. Any
 * negative value = AUTO: the module picks it. The plug-in maps this to its
 * model's own units and calibrates it on its panel; the runtime never
 * interprets it beyond clamping to [0, 1] (or passing a negative through).
 */
struct xrt_dp_lift_params
{
	uint32_t struct_size;
	float convergence;   //!< relative depth at the display plane, [0,1]; < 0 = AUTO
	float strength;      //!< disparity scale; 1 = the module's calibrated budget
	uint32_t inpaint;    //!< non-zero = fill disocclusions
	uint32_t view_count; //!< views to produce (2 for SBS, N for NVIEW; ignored otherwise)
	/*!
	 * Focal length of the submitted image, in PIXELS of the input (w x h) —
	 * what a photo → Gaussians module (SHARP-class) takes as its intrinsics
	 * input. <= 0 = unknown: the module assumes its default field of view.
	 * Appended (read only when struct_size covers it); DEPTH / SBS / NVIEW
	 * modules ignore it.
	 */
	float focal_px;

	/*
	 * ── Viewpoint policy (ADR-048, XRT_DP_LIFT_HAS_VIEWPOINT_POLICY) ────────
	 * Appended; read only when struct_size covers them. The RUNTIME owns the
	 * policy — rebasing to the rect, ipd / parallax factors, axis masking,
	 * clamping and recentering are already applied to the viewpoints
	 * lift_convert receives. The plug-in only translates them into its
	 * module's units: normalise by baseline_m (not a fixed eye distance),
	 * honour every component it is sent (axis_mode says which ones the runtime
	 * let through — the others are already 0 / at the reference distance), and
	 * map the offset range onto its own range using max_offset_m and the rect
	 * size.
	 */

	//! Physical width of the lifted region (rect / window) in metres; 0 = unknown.
	float rect_width_m;
	//! Physical height of the lifted region in metres; 0 = unknown.
	float rect_height_m;
	//! Distance between the outermost viewpoints of the pair the runtime
	//! resolved (eye separation after the ipd factor), metres; 0 = no
	//! viewpoints / unknown (use the module's own default).
	float baseline_m;
	//! XRT_DP_LIFT_AXIS_*: which midpoint components follow the viewer.
	uint32_t axis_mode;
	//! The clamp the runtime applied to the x / y offset of the viewpoints'
	//! midpoint from the rect centre, metres; 0 = unclamped.
	float max_offset_m;
	//! XRT_DP_LIFT_VIEWPOINT_FRAME_*: origin of the viewpoints passed to
	//! lift_convert. Always RECT from a runtime that fills this field.
	uint32_t viewpoint_frame;

	/*
	 * ── App rig (ADR-048 Addendum A, XRT_DP_LIFT_HAS_APP_RIG) ──────────────
	 * Appended; read only when struct_size covers them.
	 */

	//! XRT_DP_LIFT_VIEWPOINTS_*: where the viewpoints came from. With
	//! DISPLAY_RIG / CAMERA_RIG they are the app's rig eyes (the xrLocateViews
	//! math, the rect as the screen) — the same eyes the app renders its own
	//! 3D from, so a module must reproduce them as given (no extra gain).
	uint32_t viewpoint_source;
	//! The reference viewing distance the runtime used (axis pinning, rig
	//! nominal viewer), metres from the rect plane; 0 = unknown.
	float nominal_z_m;
};

/*!
 * @name App rig (ADR-048 Addendum A) — xrt_dp_lift_params appended fields
 * Announced by XRT_DP_LIFT_HAS_APP_RIG (append-only, ADR-020: no ABI bump).
 * @{
 */
#define XRT_DP_LIFT_HAS_APP_RIG 1

#define XRT_DP_LIFT_VIEWPOINTS_TRACKED 0u     //!< tracked eyes + the viewpoint policy
#define XRT_DP_LIFT_VIEWPOINTS_EXPLICIT 1u    //!< the app's explicit viewpoints
#define XRT_DP_LIFT_VIEWPOINTS_DISPLAY_RIG 2u //!< tracked eyes through the app's display rig
#define XRT_DP_LIFT_VIEWPOINTS_CAMERA_RIG 3u  //!< tracked eyes through the app's camera rig
/*! @} */

/*!
 * Pre-set @p caps for a lift_get_caps call: zero it and stamp struct_size.
 */
static inline void
xrt_dp_lift_caps_init(struct xrt_dp_lift_caps *caps)
{
	uint8_t *p = (uint8_t *)caps;
	for (uint32_t i = 0; i < (uint32_t)sizeof(*caps); i++) {
		p[i] = 0;
	}
	caps->struct_size = (uint32_t)sizeof(*caps);
}

/*!
 * Pre-set @p d for a lift_get_depth call: zero it and stamp struct_size.
 */
static inline void
xrt_dp_lift_depth_init(struct xrt_dp_lift_depth *d)
{
	uint8_t *p = (uint8_t *)d;
	for (uint32_t i = 0; i < (uint32_t)sizeof(*d); i++) {
		p[i] = 0;
	}
	d->struct_size = (uint32_t)sizeof(*d);
}

#ifdef __cplusplus
}
#endif
