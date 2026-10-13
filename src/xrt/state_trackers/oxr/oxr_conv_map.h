// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_lift spec 4 <-> conversion wire: the one translation step
 *         (ADR-053 P3, #2054). Pure functions, no state: the state tracker
 *         fills the app structs from the wire structs here, and the tests pin
 *         the depth-kind contract and the v3-observable shifts through them.
 *
 * Enum values already equal the plug-in ABI's (oxr_plugin_stub.c asserts each
 * pair), so most of this is a plain copy; what is NOT a copy is written out:
 * the output-flag vocabulary, the input-kind value / bit pair, and the
 * decoded-range / intrinsics rules of XrLiftDepthInfoDXR (units are never
 * invented).
 *
 * @ingroup oxr_main
 */

#pragma once

#include "xrt/xrt_conv_wire.h"
#include "xrt/xrt_conversion_plugin.h"

#include <openxr/openxr.h>
#include <openxr/XR_DXR_lift.h>

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * The decoding table of XrLiftDepthInfoDXR (normative, see the header): an
 * inverse kind's decoded value v is an inverse depth.
 */
static inline bool
oxr_conv_depth_kind_is_inverse(uint32_t kind)
{
	return kind == XRT_CONV_DEPTH_METRIC_INVERSE || kind == XRT_CONV_DEPTH_AFFINE_INVERSE ||
	       kind == XRT_CONV_DEPTH_AFFINE_DISPARITY || kind == XRT_CONV_DEPTH_STEREO_DISPARITY_PX;
}

//! The depth reading d of a decoded value v (v itself, or 1 / v for an inverse kind); false when v <= 0.
static inline bool
oxr_conv_depth_reading(uint32_t kind, float v, float *out_d)
{
	if (!(v > 0.0f) || !isfinite(v)) {
		return false;
	}
	*out_d = oxr_conv_depth_kind_is_inverse(kind) ? 1.0f / v : v;
	return true;
}

//! The inverse-depth coordinate q of a decoded value v (v for an inverse kind, else 1 / v); false when v <= 0.
static inline bool
oxr_conv_inverse_coord(uint32_t kind, float v, float *out_q)
{
	if (!(v > 0.0f) || !isfinite(v)) {
		return false;
	}
	*out_q = oxr_conv_depth_kind_is_inverse(kind) ? v : 1.0f / v;
	return true;
}

//! The tasks a spec-4 app may name (spec-5 and reserved ones fail validation).
static inline bool
oxr_conv_task_is_spec4(XrLiftTaskDXR task)
{
	return task == XR_LIFT_TASK_DEPTH_MONO_DXR || task == XR_LIFT_TASK_DEPTH_VIDEO_DXR ||
	       task == XR_LIFT_TASK_VIEWS_MONO_DXR || task == XR_LIFT_TASK_SPLATS_MONO_DXR;
}

//! The v3 mode that matches a task (the graceful-degradation mode), or 0.
static inline bool
oxr_conv_mode_matches_task(uint32_t mode, XrLiftTaskDXR task)
{
	switch (task) {
	case XR_LIFT_TASK_DEPTH_MONO_DXR:
	case XR_LIFT_TASK_DEPTH_VIDEO_DXR: return mode == XR_LIFT_MODE_DEPTH_DXR;
	case XR_LIFT_TASK_VIEWS_MONO_DXR: return mode == XR_LIFT_MODE_SBS_DXR || mode == XR_LIFT_MODE_NVIEW_DXR;
	case XR_LIFT_TASK_SPLATS_MONO_DXR: return mode == XR_LIFT_MODE_GAUSSIANS_DXR;
	default: return false;
	}
}

//! XR_LIFT_OUTPUT_* bits a task desc offers (same rule as the service's create check).
static inline XrLiftOutputFlagsDXR
oxr_conv_output_flags(const struct xrt_conv_wire_task_desc *d)
{
	XrLiftOutputFlagsDXR f = 0;
	const bool views = d->task == XRT_CONV_TASK_VIEWS_MONO;
	const bool depth = (d->outputs & XRT_CONV_OUT_DEPTH) != 0;
	if (views && depth) {
		f |= XR_LIFT_OUTPUT_AUX_DEPTH_BIT_DXR;
	}
	if (depth && (d->depth_kind == XRT_CONV_DEPTH_METRIC || d->depth_kind == XRT_CONV_DEPTH_METRIC_INVERSE)) {
		f |= XR_LIFT_OUTPUT_METRIC_BIT_DXR;
	}
	if (depth && d->intrinsics_source != XRT_CONV_INTRINSICS_NONE) {
		f |= XR_LIFT_OUTPUT_INTRINSICS_BIT_DXR;
	}
	if ((d->flags & XRT_CONV_CAP_TEMPORAL) != 0) {
		f |= XR_LIFT_OUTPUT_TEMPORAL_BIT_DXR;
	}
	if (views && (d->flags & XRT_CONV_CAP_EXPLICIT_VIEWPOINTS) != 0) {
		f |= XR_LIFT_OUTPUT_EXPLICIT_VIEWPOINTS_BIT_DXR;
	}
	if ((d->flags & XRT_CONV_CAP_SUGGESTS_CONVERGENCE) != 0) {
		f |= XR_LIFT_OUTPUT_CONVERGENCE_HINT_BIT_DXR;
	}
	if ((d->outputs & XRT_CONV_OUT_CONFIDENCE) != 0) {
		f |= XR_LIFT_OUTPUT_CONFIDENCE_BIT_DXR;
	}
	if ((d->desc_flags & XRT_CONV_WIRE_DESC_COMPOSED) != 0) {
		f |= XR_LIFT_OUTPUT_COMPOSED_BIT_DXR;
	}
	return f;
}

/*!
 * One task desc (+ the session's image / export kind masks) -> the app's
 * XrLiftTaskPropertiesDXR. type / next are left as the caller set them.
 */
static inline void
oxr_conv_task_properties_from_wire(const struct xrt_conv_wire_task_desc *d,
                                   uint64_t image_kinds,
                                   uint64_t export_kinds,
                                   XrLiftTaskPropertiesDXR *p)
{
	p->task = (XrLiftTaskDXR)d->task;
	p->state = (XrLiftTaskStateDXR)d->state;
	p->failureReason = (XrLiftFailureReasonDXR)d->failure_reason;
	p->progress = d->progress;
	p->lanes = 0;
	if ((d->lanes & XRT_CONV_LANE_STREAM) != 0) {
		p->lanes |= XR_LIFT_LANE_STREAM_BIT_DXR;
	}
	if ((d->lanes & XRT_CONV_LANE_JOB) != 0) {
		p->lanes |= XR_LIFT_LANE_JOB_BIT_DXR;
	}
	// ABI input kinds are bits from 1 << 0; the app's are values from 1.
	p->inputKinds = (XrLiftInputKindFlagsDXR)(d->input_kinds & 0x7u) << 1;
	p->outputs = oxr_conv_output_flags(d);
	p->depthKinds = 0;
	if ((d->outputs & XRT_CONV_OUT_DEPTH) != 0 && d->depth_kind != XRT_CONV_DEPTH_UNKNOWN && d->depth_kind < 64) {
		p->depthKinds = 1ull << d->depth_kind;
	}
	p->blobFormats = 0;
	for (uint32_t b = 1; b < 32; b++) {
		// splat_formats is a bit set of (1 << XRT_CONV_BLOB_*); spec 4 names PLY / SOG only.
		if ((d->splat_formats & (1u << b)) != 0 && (b == XRT_CONV_BLOB_PLY_3DGS || b == XRT_CONV_BLOB_SOG)) {
			p->blobFormats |= 1ull << b;
		}
	}
	p->imageKinds = image_kinds;
	p->exportKinds = export_kinds;
	p->maxStreams = (d->lanes & XRT_CONV_LANE_STREAM) != 0 ? d->max_streams : 0;
	p->maxViews = d->task == XRT_CONV_TASK_VIEWS_MONO
	                  ? (d->max_views > XR_LIFT_MAX_VIEWS_DXR ? XR_LIFT_MAX_VIEWS_DXR : d->max_views)
	                  : 0;
	p->maxInputExtent.width = (int32_t)d->max_input_edge;
	p->maxInputExtent.height = (int32_t)d->max_input_edge;
	p->typicalLatency = (XrDuration)d->est_latency_ms * 1000000;
	p->warmEstimate = (XrDuration)d->est_warmup_ms * 1000000;
	p->licenceClass = (XrLiftLicenceClassDXR)d->licence_class;
}

/*!
 * A result's depth twin -> XrLiftDepthInfoDXR. Units are never invented: an
 * AFFINE kind reports near / far / intrinsics only as the provider did, and
 * zero intrinsics read UNKNOWN whatever the claimed source.
 */
static inline void
oxr_conv_depth_info_from_wire(const struct xrt_conv_wire_depth_info *w, bool discontinuity, XrLiftDepthInfoDXR *o)
{
	o->kind = (XrLiftDepthKindDXR)w->kind;
	o->measure = (XrLiftDepthMeasureDXR)w->measure;
	o->rawScale = w->value_scale != 0.0f ? w->value_scale : 1.0f;
	o->rawShift = w->value_shift;
	o->invalidValueValid = isnan(w->invalid_sample) ? XR_FALSE : XR_TRUE;
	o->invalidValue = isnan(w->invalid_sample) ? 0.0f : w->invalid_sample;
	o->nearZ = w->near_value;
	o->farZ = w->far_value;
	o->rangeValid = (w->range_min != 0.0f || w->range_max != 0.0f) ? XR_TRUE : XR_FALSE;
	o->rangeMin = w->range_min;
	o->rangeMax = w->range_max;
	o->rangeP05 = w->range_p05;
	o->rangeP95 = w->range_p95;
	const bool have_k = w->fx > 0.0f && w->fy > 0.0f;
	o->intrinsicsSource =
	    have_k ? (XrLiftIntrinsicsSourceDXR)w->intrinsics_source : XR_LIFT_INTRINSICS_SOURCE_UNKNOWN_DXR;
	o->focalLengthPx.x = have_k ? w->fx : 0.0f;
	o->focalLengthPx.y = have_k ? w->fy : 0.0f;
	o->principalPointPx.x = have_k ? w->cx : 0.0f;
	o->principalPointPx.y = have_k ? w->cy : 0.0f;
	o->convention = have_k ? XR_LIFT_COORDINATE_SYSTEM_OPENCV_RDF_DXR : XR_LIFT_COORDINATE_SYSTEM_UNKNOWN_DXR;
	o->temporallyConsistent = w->temporal != 0 ? XR_TRUE : XR_FALSE;
	o->discontinuity = discontinuity ? XR_TRUE : XR_FALSE;
	o->baselineMeters = 0.0f; // PIXEL_DISPARITY is spec 5
	// Decoded units, as the provider reported (never converted to a depth).
	o->convergenceValid = (w->suggest_flags & 4u) != 0 && isfinite(w->convergence_value) ? XR_TRUE : XR_FALSE;
	o->convergenceValue = o->convergenceValid ? w->convergence_value : 0.0f;
}

/*!
 * The v3 reading of a depth kind (XrLiftDepthResultDXR units + encoding, and
 * XrLiftPropertiesDXR depth semantics): METRIC* = METRIC, every AFFINE kind =
 * RELATIVE (shift 3: AFFINE_DISPARITY is RELATIVE + INVERSE, so
 * auxDepthSupported stays true); the inverse kinds encode INVERSE.
 * PIXEL_DISPARITY never reaches a v3 result (returns false).
 */
static inline bool
oxr_conv_depth_kind_to_v3(uint32_t kind, XrLiftDepthSemanticsDXR *units, XrLiftDepthEncodingDXR *encoding)
{
	switch (kind) {
	case XRT_CONV_DEPTH_METRIC:
		*units = XR_LIFT_DEPTH_SEMANTICS_METRIC_DXR;
		*encoding = XR_LIFT_DEPTH_ENCODING_LINEAR_DXR;
		return true;
	case XRT_CONV_DEPTH_METRIC_INVERSE:
		*units = XR_LIFT_DEPTH_SEMANTICS_METRIC_DXR;
		*encoding = XR_LIFT_DEPTH_ENCODING_INVERSE_DXR;
		return true;
	case XRT_CONV_DEPTH_AFFINE_INVERSE:
	case XRT_CONV_DEPTH_AFFINE_DISPARITY:
		*units = XR_LIFT_DEPTH_SEMANTICS_RELATIVE_DXR;
		*encoding = XR_LIFT_DEPTH_ENCODING_INVERSE_DXR;
		return true;
	case XRT_CONV_DEPTH_AFFINE:
	case XRT_CONV_DEPTH_UNKNOWN:
		*units = XR_LIFT_DEPTH_SEMANTICS_RELATIVE_DXR;
		*encoding = XR_LIFT_DEPTH_ENCODING_LINEAR_DXR;
		return true;
	default: return false;
	}
}

/*!
 * A views twin (+ the export extent, + the same result's depth twin when it
 * carries depth: the relief, z = reliefScale * q + reliefOffset, lives there
 * in the same q convention) -> XrLiftViewsInfoDXR.
 */
static inline void
oxr_conv_views_info_from_wire(const struct xrt_conv_wire_views_info *w,
                              uint32_t views_width,
                              uint32_t views_height,
                              const struct xrt_conv_wire_depth_info *depth,
                              XrLiftViewsInfoDXR *o)
{
	uint32_t n = w->view_count > XR_LIFT_MAX_VIEWS_DXR ? XR_LIFT_MAX_VIEWS_DXR : w->view_count;
	o->layout = (XrLiftViewsLayoutDXR)w->layout;
	o->viewCount = n;
	o->columns = w->layout == XRT_CONV_VIEWS_ROW ? n : 1u;
	o->rows = 1u;
	o->viewExtent.width = (int32_t)(w->layout == XRT_CONV_VIEWS_ROW && n > 0 ? views_width / n : views_width);
	o->viewExtent.height = (int32_t)views_height;
	o->eyeOffsetsValid = n > 0 ? XR_TRUE : XR_FALSE;
	float mid = 0.0f;
	for (uint32_t i = 0; i < n; i++) {
		mid += w->eye_xyz[i][0] / (float)n;
	}
	for (uint32_t i = 0; i < XR_LIFT_MAX_VIEWS_DXR; i++) {
		o->eyeOffsetMeters[i] = i < n ? w->eye_xyz[i][0] - mid : 0.0f;
	}
	o->baselineMeters = w->baseline_m > 0.0f ? w->baseline_m
	                    : n > 1              ? fabsf(w->eye_xyz[n - 1][0] - w->eye_xyz[0][0])
	                                         : 0.0f;
	o->horizontalFovRadians = w->hfov_deg > 0.0f ? w->hfov_deg * 0.017453292519943295f : 0.0f;
	o->disparityAdjust = w->zero_parallax_disparity;
	o->disparityRangeValid = XR_FALSE;
	o->disparityMin = 0.0f;
	o->disparityMax = 0.0f;
	o->cameraModel = XR_LIFT_CAMERA_MODEL_UNKNOWN_DXR;
	o->convergenceSource = XR_LIFT_CONVERGENCE_SOURCE_UNKNOWN_DXR;
	o->convergenceApplied = -1.0f;
	o->suggestedConvergence = -1.0f;
	o->suggestedStrength = -1.0f;
	const bool relief = depth != NULL && depth->relief_valid != 0 && isfinite(depth->relief_scale) &&
	                    isfinite(depth->relief_offset) && depth->relief_scale != 0.0f;
	o->reliefValid = relief ? XR_TRUE : XR_FALSE;
	o->reliefScale = relief ? depth->relief_scale : 0.0f;
	o->reliefOffset = relief ? depth->relief_offset : 0.0f;
}

//! The DXGI format of an export -> XrLiftPixelFormatDXR (0 = unknown).
static inline XrLiftPixelFormatDXR
oxr_conv_pixel_format_from_dxgi(int64_t dxgi)
{
	switch (dxgi) {
	case 28: return XR_LIFT_PIXEL_FORMAT_RGBA8_UNORM_DXR;  // DXGI_FORMAT_R8G8B8A8_UNORM
	case 29: return XR_LIFT_PIXEL_FORMAT_RGBA8_SRGB_DXR;   // _UNORM_SRGB
	case 87: return XR_LIFT_PIXEL_FORMAT_BGRA8_UNORM_DXR;  // DXGI_FORMAT_B8G8R8A8_UNORM
	case 91: return XR_LIFT_PIXEL_FORMAT_BGRA8_SRGB_DXR;   // _UNORM_SRGB
	case 10: return XR_LIFT_PIXEL_FORMAT_RGBA16_FLOAT_DXR; // DXGI_FORMAT_R16G16B16A16_FLOAT
	case 41: return XR_LIFT_PIXEL_FORMAT_R32_FLOAT_DXR;    // DXGI_FORMAT_R32_FLOAT
	case 54: return XR_LIFT_PIXEL_FORMAT_R16_FLOAT_DXR;    // DXGI_FORMAT_R16_FLOAT
	case 56: return XR_LIFT_PIXEL_FORMAT_R16_UNORM_DXR;    // DXGI_FORMAT_R16_UNORM
	case 61: return XR_LIFT_PIXEL_FORMAT_R8_UNORM_DXR;     // DXGI_FORMAT_R8_UNORM
	default: return XR_LIFT_PIXEL_FORMAT_UNKNOWN_DXR;
	}
}

#ifdef __cplusplus
}
#endif
