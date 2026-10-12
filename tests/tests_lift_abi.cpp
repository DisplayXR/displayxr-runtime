// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_lift v3 app ABI pinned (ADR-053 P1a): every struct sizeof /
 *         offsetof, every enum value and every constant of spec 3.
 *
 * The browser, the web SDK's native path and the engine plug-ins vendor
 * XR_DXR_lift.h byte-for-byte; spec 4 / 5 (ADR-053 P3 / P6) only APPEND to
 * it. This test exists so that appending can never shift a v3 layout: every
 * number below is the shipped spec-3 value. If one fails, the change broke
 * shipped binaries — revert it, do not update the number.
 *
 * Layouts are pinned for 64-bit targets (x64 / arm64: 8-byte pointers and
 * handles, 8-byte-aligned 64-bit integers). Enum values and constants hold
 * everywhere.
 */

#include "catch_amalgamated.hpp"

#include <openxr/openxr.h>
#include <openxr/XR_DXR_lift.h>

#include <cstddef>
#include <cstring>

#define PIN_OFF(T, F, V) CHECK(offsetof(T, F) == (V))

TEST_CASE("lift ABI v3: constants and type values", "[lift_abi]")
{
	STATIC_REQUIRE(XR_DXR_lift_SPEC_VERSION >= 3);
	CHECK(std::strcmp(XR_DXR_LIFT_EXTENSION_NAME, "XR_DXR_lift") == 0);

	STATIC_REQUIRE(XR_TYPE_LIFT_PROPERTIES_DXR == (XrStructureType)1004999270);
	STATIC_REQUIRE(XR_TYPE_LIFT_STREAM_CREATE_INFO_DXR == (XrStructureType)1004999271);
	STATIC_REQUIRE(XR_TYPE_LIFT_FRAME_SUBMIT_INFO_DXR == (XrStructureType)1004999272);
	STATIC_REQUIRE(XR_TYPE_LIFT_OPTIONS_DXR == (XrStructureType)1004999273);
	STATIC_REQUIRE(XR_TYPE_LIFT_RESULT_DXR == (XrStructureType)1004999274);
	STATIC_REQUIRE(XR_TYPE_WEAVE_SUBMIT_LIFT_RECTS_DXR == (XrStructureType)1004999275);
	STATIC_REQUIRE(XR_TYPE_WEAVE_RECT_LIFT_DXR == (XrStructureType)1004999276);
	STATIC_REQUIRE(XR_TYPE_LIFT_BLOB_DXR == (XrStructureType)1004999277);
	STATIC_REQUIRE(XR_OBJECT_TYPE_LIFT_STREAM_DXR == (XrObjectType)1004999278);
	STATIC_REQUIRE(XR_LIFT_NOT_READY_DXR == (XrResult)1004999279);
	STATIC_REQUIRE(XR_TYPE_LIFT_STREAM_STATS_DXR == (XrStructureType)1004999280);
	STATIC_REQUIRE(XR_TYPE_LIFT_VIEW_CONTROL_DXR == (XrStructureType)1004999281);
	STATIC_REQUIRE(XR_TYPE_LIFT_RESULT_VIEWPOINTS_DXR == (XrStructureType)1004999282);
	STATIC_REQUIRE(XR_TYPE_LIFT_DEPTH_REQUEST_DXR == (XrStructureType)1004999283);
	STATIC_REQUIRE(XR_TYPE_LIFT_DEPTH_RESULT_DXR == (XrStructureType)1004999284);
	STATIC_REQUIRE(XR_TYPE_LIFT_DEPTH_PROPERTIES_DXR == (XrStructureType)1004999285);

	STATIC_REQUIRE(XR_LIFT_BACKEND_NAME_MAX_SIZE_DXR == 32);
	STATIC_REQUIRE(XR_LIFT_MAX_VIEWS_DXR == 8);
	STATIC_REQUIRE(XR_WEAVE_SUBMIT_MAX_LIFT_RECTS_DXR == 8);
	CHECK(XR_LIFT_CONVERGENCE_AUTO_DXR == -1.0f);
}

TEST_CASE("lift ABI v3: enum and flag values", "[lift_abi]")
{
	CHECK(XR_LIFT_MODE_DEPTH_BIT_DXR == 0x1u);
	CHECK(XR_LIFT_MODE_SBS_BIT_DXR == 0x2u);
	CHECK(XR_LIFT_MODE_NVIEW_BIT_DXR == 0x4u);
	CHECK(XR_LIFT_MODE_GAUSSIANS_BIT_DXR == 0x8u);
	STATIC_REQUIRE(sizeof(XrLiftModeFlagsDXR) == 8);

	STATIC_REQUIRE(XR_LIFT_MODE_DEPTH_DXR == 1 && XR_LIFT_MODE_SBS_DXR == 2 && XR_LIFT_MODE_NVIEW_DXR == 4 &&
	               XR_LIFT_MODE_GAUSSIANS_DXR == 8);
	STATIC_REQUIRE(XR_LIFT_DEPTH_SEMANTICS_RELATIVE_DXR == 0 && XR_LIFT_DEPTH_SEMANTICS_METRIC_DXR == 1);
	STATIC_REQUIRE(XR_LIFT_DEPTH_ENCODING_LINEAR_DXR == 0 && XR_LIFT_DEPTH_ENCODING_INVERSE_DXR == 1);
	STATIC_REQUIRE(XR_LIFT_STATE_UNAVAILABLE_DXR == 0 && XR_LIFT_STATE_ACTIVATING_DXR == 1 &&
	               XR_LIFT_STATE_READY_DXR == 2);
	STATIC_REQUIRE(XR_LIFT_CONTENT_HINT_VIDEO_DXR == 0 && XR_LIFT_CONTENT_HINT_PHOTO_DXR == 1);
	STATIC_REQUIRE(XR_LIFT_VIEWPOINT_SOURCE_TRACKED_DXR == 0 && XR_LIFT_VIEWPOINT_SOURCE_EXPLICIT_DXR == 1);
	STATIC_REQUIRE(XR_LIFT_PRIORITY_PAUSED_DXR == 0 && XR_LIFT_PRIORITY_LOW_DXR == 1 &&
	               XR_LIFT_PRIORITY_NORMAL_DXR == 2 && XR_LIFT_PRIORITY_HIGH_DXR == 3);
	STATIC_REQUIRE(XR_LIFT_AXIS_MODE_X_DXR == 1 && XR_LIFT_AXIS_MODE_XY_DXR == 2 && XR_LIFT_AXIS_MODE_XYZ_DXR == 3);
	STATIC_REQUIRE(XR_LIFT_RECENTER_MODE_OFF_DXR == 0 && XR_LIFT_RECENTER_MODE_EASE_BACK_DXR == 1);
	STATIC_REQUIRE(XR_LIFT_BLOB_FORMAT_PLY_3DGS_DXR == 1 && XR_LIFT_BLOB_FORMAT_SOG_DXR == 2);

	// Every v3 enum is 32-bit (MAX_ENUM 0x7FFFFFFF).
	STATIC_REQUIRE(sizeof(XrLiftModeDXR) == 4 && sizeof(XrLiftDepthSemanticsDXR) == 4 &&
	               sizeof(XrLiftDepthEncodingDXR) == 4 && sizeof(XrLiftStateDXR) == 4 &&
	               sizeof(XrLiftContentHintDXR) == 4 && sizeof(XrLiftViewpointSourceDXR) == 4 &&
	               sizeof(XrLiftPriorityDXR) == 4 && sizeof(XrLiftAxisModeDXR) == 4 &&
	               sizeof(XrLiftRecenterModeDXR) == 4 && sizeof(XrLiftBlobFormatDXR) == 4);
}

TEST_CASE("lift ABI v3: struct layouts (64-bit)", "[lift_abi]")
{
	if (sizeof(void *) != 8) {
		SKIP("v3 layouts are pinned for 64-bit targets");
	}

	CHECK(sizeof(XrLiftPropertiesDXR) == 80);
	PIN_OFF(XrLiftPropertiesDXR, type, 0);
	PIN_OFF(XrLiftPropertiesDXR, next, 8);
	PIN_OFF(XrLiftPropertiesDXR, supportedModes, 16);
	PIN_OFF(XrLiftPropertiesDXR, maxStreams, 24);
	PIN_OFF(XrLiftPropertiesDXR, maxViews, 28);
	PIN_OFF(XrLiftPropertiesDXR, depthSemantics, 32);
	PIN_OFF(XrLiftPropertiesDXR, state, 36);
	PIN_OFF(XrLiftPropertiesDXR, backend, 40);
	PIN_OFF(XrLiftPropertiesDXR, typicalLatency, 72);

	CHECK(sizeof(XrLiftDepthPropertiesDXR) == 24);
	PIN_OFF(XrLiftDepthPropertiesDXR, type, 0);
	PIN_OFF(XrLiftDepthPropertiesDXR, next, 8);
	PIN_OFF(XrLiftDepthPropertiesDXR, auxDepthSupported, 16);
	PIN_OFF(XrLiftDepthPropertiesDXR, auxDepthSemantics, 20);

	CHECK(sizeof(XrLiftStreamCreateInfoDXR) == 32);
	PIN_OFF(XrLiftStreamCreateInfoDXR, type, 0);
	PIN_OFF(XrLiftStreamCreateInfoDXR, next, 8);
	PIN_OFF(XrLiftStreamCreateInfoDXR, mode, 16);
	PIN_OFF(XrLiftStreamCreateInfoDXR, contentHint, 20);
	PIN_OFF(XrLiftStreamCreateInfoDXR, inputScale, 24);

	CHECK(sizeof(XrLiftDepthRequestDXR) == 24);
	PIN_OFF(XrLiftDepthRequestDXR, type, 0);
	PIN_OFF(XrLiftDepthRequestDXR, next, 8);
	PIN_OFF(XrLiftDepthRequestDXR, preferredFormat, 16);

	CHECK(sizeof(XrLiftOptionsDXR) == 56);
	PIN_OFF(XrLiftOptionsDXR, type, 0);
	PIN_OFF(XrLiftOptionsDXR, next, 8);
	PIN_OFF(XrLiftOptionsDXR, convergence, 16);
	PIN_OFF(XrLiftOptionsDXR, strength, 20);
	PIN_OFF(XrLiftOptionsDXR, inpaint, 24);
	PIN_OFF(XrLiftOptionsDXR, viewpointSource, 28);
	PIN_OFF(XrLiftOptionsDXR, viewCount, 32);
	PIN_OFF(XrLiftOptionsDXR, viewpoints, 40);
	PIN_OFF(XrLiftOptionsDXR, focalPx, 48);

	CHECK(sizeof(XrLiftViewControlDXR) == 48);
	PIN_OFF(XrLiftViewControlDXR, type, 0);
	PIN_OFF(XrLiftViewControlDXR, next, 8);
	PIN_OFF(XrLiftViewControlDXR, ipdFactor, 16);
	PIN_OFF(XrLiftViewControlDXR, parallaxFactor, 20);
	PIN_OFF(XrLiftViewControlDXR, axisMode, 24);
	PIN_OFF(XrLiftViewControlDXR, maxOffsetMeters, 28);
	PIN_OFF(XrLiftViewControlDXR, recenterMode, 32);
	PIN_OFF(XrLiftViewControlDXR, recenterHoldSeconds, 36);
	PIN_OFF(XrLiftViewControlDXR, recenterTimeConstantSeconds, 40);

	CHECK(sizeof(XrLiftResultViewpointsDXR) == 136);
	PIN_OFF(XrLiftResultViewpointsDXR, type, 0);
	PIN_OFF(XrLiftResultViewpointsDXR, next, 8);
	PIN_OFF(XrLiftResultViewpointsDXR, viewpointCountOutput, 16);
	PIN_OFF(XrLiftResultViewpointsDXR, viewpoints, 20);
	PIN_OFF(XrLiftResultViewpointsDXR, rectCenter, 116);
	PIN_OFF(XrLiftResultViewpointsDXR, rectSize, 128);

	CHECK(sizeof(XrLiftDepthResultDXR) == 208);
	PIN_OFF(XrLiftDepthResultDXR, type, 0);
	PIN_OFF(XrLiftDepthResultDXR, next, 8);
	PIN_OFF(XrLiftDepthResultDXR, depthValid, 16);
	PIN_OFF(XrLiftDepthResultDXR, depthTexture, 24);
	PIN_OFF(XrLiftDepthResultDXR, fence, 32);
	PIN_OFF(XrLiftDepthResultDXR, fenceValue, 40);
	PIN_OFF(XrLiftDepthResultDXR, extent, 48);
	PIN_OFF(XrLiftDepthResultDXR, format, 56);
	PIN_OFF(XrLiftDepthResultDXR, units, 64);
	PIN_OFF(XrLiftDepthResultDXR, encoding, 68);
	PIN_OFF(XrLiftDepthResultDXR, valueScale, 72);
	PIN_OFF(XrLiftDepthResultDXR, valueOffset, 76);
	PIN_OFF(XrLiftDepthResultDXR, nearDepth, 80);
	PIN_OFF(XrLiftDepthResultDXR, farDepth, 84);
	PIN_OFF(XrLiftDepthResultDXR, sameInference, 88);
	PIN_OFF(XrLiftDepthResultDXR, frameId, 96);
	PIN_OFF(XrLiftDepthResultDXR, intrinsicsValid, 104);
	PIN_OFF(XrLiftDepthResultDXR, focalLengthPx, 108);
	PIN_OFF(XrLiftDepthResultDXR, principalPointPx, 116);
	PIN_OFF(XrLiftDepthResultDXR, convergenceDepth, 124);
	PIN_OFF(XrLiftDepthResultDXR, viewpoint, 128);
	PIN_OFF(XrLiftDepthResultDXR, transformValid, 140);
	PIN_OFF(XrLiftDepthResultDXR, depthToDisplay, 144);

	CHECK(sizeof(XrLiftFrameSubmitInfoDXR) == 48);
	PIN_OFF(XrLiftFrameSubmitInfoDXR, type, 0);
	PIN_OFF(XrLiftFrameSubmitInfoDXR, next, 8);
	PIN_OFF(XrLiftFrameSubmitInfoDXR, inputTexture, 16);
	PIN_OFF(XrLiftFrameSubmitInfoDXR, inputIsDxgi, 24);
	PIN_OFF(XrLiftFrameSubmitInfoDXR, extent, 28);
	PIN_OFF(XrLiftFrameSubmitInfoDXR, sourceTime, 40);

	CHECK(sizeof(XrLiftResultDXR) == 88);
	PIN_OFF(XrLiftResultDXR, type, 0);
	PIN_OFF(XrLiftResultDXR, next, 8);
	PIN_OFF(XrLiftResultDXR, frameId, 16);
	PIN_OFF(XrLiftResultDXR, sourceTime, 24);
	PIN_OFF(XrLiftResultDXR, outputTexture, 32);
	PIN_OFF(XrLiftResultDXR, fence, 40);
	PIN_OFF(XrLiftResultDXR, fenceValue, 48);
	PIN_OFF(XrLiftResultDXR, extent, 56);
	PIN_OFF(XrLiftResultDXR, format, 64);
	PIN_OFF(XrLiftResultDXR, viewCount, 72);
	PIN_OFF(XrLiftResultDXR, latency, 80);

	CHECK(sizeof(XrLiftBlobDXR) == 56);
	PIN_OFF(XrLiftBlobDXR, type, 0);
	PIN_OFF(XrLiftBlobDXR, next, 8);
	PIN_OFF(XrLiftBlobDXR, frameId, 16);
	PIN_OFF(XrLiftBlobDXR, sourceTime, 24);
	PIN_OFF(XrLiftBlobDXR, format, 32);
	PIN_OFF(XrLiftBlobDXR, byteCapacityInput, 36);
	PIN_OFF(XrLiftBlobDXR, byteCountOutput, 40);
	PIN_OFF(XrLiftBlobDXR, bytes, 48);

	CHECK(sizeof(XrWeaveRectLiftDXR) == 32);
	PIN_OFF(XrWeaveRectLiftDXR, type, 0);
	PIN_OFF(XrWeaveRectLiftDXR, next, 8);
	PIN_OFF(XrWeaveRectLiftDXR, rectIndex, 16);
	PIN_OFF(XrWeaveRectLiftDXR, stream, 24);

	CHECK(sizeof(XrWeaveSubmitLiftRectsDXR) == 32);
	PIN_OFF(XrWeaveSubmitLiftRectsDXR, type, 0);
	PIN_OFF(XrWeaveSubmitLiftRectsDXR, next, 8);
	PIN_OFF(XrWeaveSubmitLiftRectsDXR, liftCount, 16);
	PIN_OFF(XrWeaveSubmitLiftRectsDXR, lifts, 24);

	CHECK(sizeof(XrLiftStreamStatsDXR) == 96);
	PIN_OFF(XrLiftStreamStatsDXR, type, 0);
	PIN_OFF(XrLiftStreamStatsDXR, next, 8);
	PIN_OFF(XrLiftStreamStatsDXR, priority, 16);
	PIN_OFF(XrLiftStreamStatsDXR, framesSubmitted, 24);
	PIN_OFF(XrLiftStreamStatsDXR, framesConverted, 32);
	PIN_OFF(XrLiftStreamStatsDXR, framesDropped, 40);
	PIN_OFF(XrLiftStreamStatsDXR, framesFailed, 48);
	PIN_OFF(XrLiftStreamStatsDXR, latencyLast, 56);
	PIN_OFF(XrLiftStreamStatsDXR, latencyAverage, 64);
	PIN_OFF(XrLiftStreamStatsDXR, latencyMin, 72);
	PIN_OFF(XrLiftStreamStatsDXR, latencyMax, 80);
	PIN_OFF(XrLiftStreamStatsDXR, conversionRate, 88);
}
