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
#include <type_traits>

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

/*
 * v3 entry points: the function table is frozen too. Each PFN keeps its exact
 * spec-3 signature (a vendored header with a changed parameter would still
 * compile against a stale loader table and crash at the call).
 */
TEST_CASE("lift ABI v3: the function table is frozen", "[lift_abi]")
{
	STATIC_REQUIRE(
	    std::is_same<PFN_xrGetLiftPropertiesDXR, XrResult(XRAPI_PTR *)(XrSession, XrLiftPropertiesDXR *)>::value);
	STATIC_REQUIRE(
	    std::is_same<PFN_xrCreateLiftStreamDXR, XrResult(XRAPI_PTR *)(XrSession, const XrLiftStreamCreateInfoDXR *,
	                                                                  XrLiftStreamDXR *)>::value);
	STATIC_REQUIRE(std::is_same<PFN_xrDestroyLiftStreamDXR, XrResult(XRAPI_PTR *)(XrLiftStreamDXR)>::value);
	STATIC_REQUIRE(
	    std::is_same<PFN_xrSubmitLiftFrameDXR,
	                 XrResult(XRAPI_PTR *)(XrLiftStreamDXR, const XrLiftFrameSubmitInfoDXR *, uint64_t *)>::value);
	STATIC_REQUIRE(
	    std::is_same<PFN_xrAcquireLiftResultDXR, XrResult(XRAPI_PTR *)(XrLiftStreamDXR, XrLiftResultDXR *)>::value);
	STATIC_REQUIRE(
	    std::is_same<PFN_xrAcquireLiftBlobDXR, XrResult(XRAPI_PTR *)(XrLiftStreamDXR, XrLiftBlobDXR *)>::value);
	STATIC_REQUIRE(std::is_same<PFN_xrSetLiftStreamPriorityDXR,
	                            XrResult(XRAPI_PTR *)(XrLiftStreamDXR, XrLiftPriorityDXR)>::value);
	STATIC_REQUIRE(std::is_same<PFN_xrGetLiftStreamStatsDXR,
	                            XrResult(XRAPI_PTR *)(XrLiftStreamDXR, XrLiftStreamStatsDXR *)>::value);
	// The prototypes match their PFNs (the loader table and the symbols agree).
	STATIC_REQUIRE(std::is_same<decltype(&xrGetLiftPropertiesDXR), PFN_xrGetLiftPropertiesDXR>::value);
	STATIC_REQUIRE(std::is_same<decltype(&xrCreateLiftStreamDXR), PFN_xrCreateLiftStreamDXR>::value);
	STATIC_REQUIRE(std::is_same<decltype(&xrDestroyLiftStreamDXR), PFN_xrDestroyLiftStreamDXR>::value);
	STATIC_REQUIRE(std::is_same<decltype(&xrSubmitLiftFrameDXR), PFN_xrSubmitLiftFrameDXR>::value);
	STATIC_REQUIRE(std::is_same<decltype(&xrAcquireLiftResultDXR), PFN_xrAcquireLiftResultDXR>::value);
	STATIC_REQUIRE(std::is_same<decltype(&xrAcquireLiftBlobDXR), PFN_xrAcquireLiftBlobDXR>::value);
	STATIC_REQUIRE(std::is_same<decltype(&xrSetLiftStreamPriorityDXR), PFN_xrSetLiftStreamPriorityDXR>::value);
	STATIC_REQUIRE(std::is_same<decltype(&xrGetLiftStreamStatsDXR), PFN_xrGetLiftStreamStatsDXR>::value);
}


/*
 *
 * Spec 4 (ADR-053 P3, #2054): pinned at introduction. Spec 4 is unreleased, so
 * these may still move until the release that ships it; after that they are
 * frozen exactly like the v3 numbers above.
 *
 */

TEST_CASE("lift ABI v4: type values sit in the claimed 1004999340-369 block", "[lift_abi]")
{
	STATIC_REQUIRE(XR_TYPE_LIFT_TASK_PROPERTIES_DXR == (XrStructureType)1004999341);
	STATIC_REQUIRE(XR_TYPE_LIFT_TASK_STREAM_CREATE_INFO_DXR == (XrStructureType)1004999342);
	STATIC_REQUIRE(XR_TYPE_LIFT_FRAME_IMAGES_DXR == (XrStructureType)1004999343);
	STATIC_REQUIRE(XR_TYPE_LIFT_IMAGE_DXR == (XrStructureType)1004999344);
	STATIC_REQUIRE(XR_TYPE_LIFT_IMAGE_DMABUF_DESC_DXR == (XrStructureType)1004999345);
	STATIC_REQUIRE(XR_TYPE_LIFT_RESULT_EXPORT_DXR == (XrStructureType)1004999346);
	STATIC_REQUIRE(XR_TYPE_LIFT_DEPTH_INFO_DXR == (XrStructureType)1004999347);
	STATIC_REQUIRE(XR_TYPE_LIFT_VIEWS_INFO_DXR == (XrStructureType)1004999348);
	STATIC_REQUIRE(XR_TYPE_LIFT_FRAME_CONTROL_DXR == (XrStructureType)1004999349);
	STATIC_REQUIRE(XR_TYPE_EVENT_DATA_LIFT_TASK_STATE_CHANGED_DXR == (XrStructureType)1004999350);
	STATIC_REQUIRE(XR_TYPE_EVENT_DATA_LIFT_PROVIDERS_CHANGED_DXR == (XrStructureType)1004999351);

	// Image kinds 1..6 equal XrWeaveHandleKindDXR (same meaning).
	STATIC_REQUIRE(XR_LIFT_IMAGE_KIND_D3D11_NT_DXR == 1 && XR_LIFT_IMAGE_KIND_D3D11_DXGI_DXR == 2 &&
	               XR_LIFT_IMAGE_KIND_IOSURFACE_DXR == 3 && XR_LIFT_IMAGE_KIND_AHARDWAREBUFFER_DXR == 4 &&
	               XR_LIFT_IMAGE_KIND_DMABUF_DXR == 5 && XR_LIFT_IMAGE_KIND_OPAQUE_FD_DXR == 6 &&
	               XR_LIFT_IMAGE_KIND_D3D12_NT_DXR == 7 && XR_LIFT_IMAGE_KIND_VK_OPAQUE_WIN32_DXR == 8 &&
	               XR_LIFT_IMAGE_KIND_CPU_DXR == 16);
	// Every image kind fits a 64-bit kind mask (imageKinds / exportKinds).
	STATIC_REQUIRE(XR_LIFT_IMAGE_KIND_CPU_DXR < 64);
	CHECK(XR_LIFT_FRAME_CONTROL_DISCONTINUITY_BIT_DXR == 0x1u);
	CHECK(XR_LIFT_OUTPUT_AUX_DEPTH_BIT_DXR == 0x1u);
	CHECK(XR_LIFT_OUTPUT_COMPOSED_BIT_DXR == 0x80u);
}

TEST_CASE("lift ABI v4: struct layouts (64-bit)", "[lift_abi]")
{
	if (sizeof(void *) != 8) {
		SKIP("layouts are pinned for 64-bit targets");
	}

	CHECK(sizeof(XrLiftTaskPropertiesDXR) == 128);
	PIN_OFF(XrLiftTaskPropertiesDXR, task, 16);
	PIN_OFF(XrLiftTaskPropertiesDXR, state, 20);
	PIN_OFF(XrLiftTaskPropertiesDXR, failureReason, 24);
	PIN_OFF(XrLiftTaskPropertiesDXR, progress, 28);
	PIN_OFF(XrLiftTaskPropertiesDXR, lanes, 32);
	PIN_OFF(XrLiftTaskPropertiesDXR, inputKinds, 40);
	PIN_OFF(XrLiftTaskPropertiesDXR, outputs, 48);
	PIN_OFF(XrLiftTaskPropertiesDXR, depthKinds, 56);
	PIN_OFF(XrLiftTaskPropertiesDXR, blobFormats, 64);
	PIN_OFF(XrLiftTaskPropertiesDXR, imageKinds, 72);
	PIN_OFF(XrLiftTaskPropertiesDXR, exportKinds, 80);
	PIN_OFF(XrLiftTaskPropertiesDXR, maxStreams, 88);
	PIN_OFF(XrLiftTaskPropertiesDXR, maxViews, 92);
	PIN_OFF(XrLiftTaskPropertiesDXR, maxInputExtent, 96);
	PIN_OFF(XrLiftTaskPropertiesDXR, typicalLatency, 104);
	PIN_OFF(XrLiftTaskPropertiesDXR, warmEstimate, 112);
	PIN_OFF(XrLiftTaskPropertiesDXR, licenceClass, 120);

	CHECK(sizeof(XrEventDataLiftProvidersChangedDXR) == 24);
	PIN_OFF(XrEventDataLiftProvidersChangedDXR, session, 16);

	CHECK(sizeof(XrEventDataLiftTaskStateChangedDXR) == 48);
	PIN_OFF(XrEventDataLiftTaskStateChangedDXR, session, 16);
	PIN_OFF(XrEventDataLiftTaskStateChangedDXR, task, 24);
	PIN_OFF(XrEventDataLiftTaskStateChangedDXR, state, 28);
	PIN_OFF(XrEventDataLiftTaskStateChangedDXR, previousState, 32);
	PIN_OFF(XrEventDataLiftTaskStateChangedDXR, failureReason, 36);
	PIN_OFF(XrEventDataLiftTaskStateChangedDXR, progress, 40);
	STATIC_REQUIRE(sizeof(XrEventDataLiftTaskStateChangedDXR) <= sizeof(XrEventDataBuffer));

	CHECK(sizeof(XrLiftTaskStreamCreateInfoDXR) == 48);
	PIN_OFF(XrLiftTaskStreamCreateInfoDXR, task, 16);
	PIN_OFF(XrLiftTaskStreamCreateInfoDXR, inputKind, 20);
	PIN_OFF(XrLiftTaskStreamCreateInfoDXR, qualityHint, 24);
	PIN_OFF(XrLiftTaskStreamCreateInfoDXR, viewCount, 28);
	PIN_OFF(XrLiftTaskStreamCreateInfoDXR, requiredOutputs, 32);
	PIN_OFF(XrLiftTaskStreamCreateInfoDXR, exportKind, 40);

	CHECK(sizeof(XrLiftImageDXR) == 88);
	PIN_OFF(XrLiftImageDXR, role, 16);
	PIN_OFF(XrLiftImageDXR, kind, 20);
	PIN_OFF(XrLiftImageDXR, handle, 24);
	PIN_OFF(XrLiftImageDXR, cpuBytes, 32);
	PIN_OFF(XrLiftImageDXR, cpuRowPitch, 40);
	PIN_OFF(XrLiftImageDXR, format, 44);
	PIN_OFF(XrLiftImageDXR, colorSpace, 48);
	PIN_OFF(XrLiftImageDXR, region, 52);
	PIN_OFF(XrLiftImageDXR, syncKind, 68);
	PIN_OFF(XrLiftImageDXR, syncHandle, 72);
	PIN_OFF(XrLiftImageDXR, syncValue, 80);

	CHECK(sizeof(XrLiftImageDmabufDescDXR) == 88);
	PIN_OFF(XrLiftImageDmabufDescDXR, fd, 16);
	PIN_OFF(XrLiftImageDmabufDescDXR, drmModifier, 32);
	PIN_OFF(XrLiftImageDmabufDescDXR, planeCount, 40);
	PIN_OFF(XrLiftImageDmabufDescDXR, offsets, 44);
	PIN_OFF(XrLiftImageDmabufDescDXR, strides, 60);
	PIN_OFF(XrLiftImageDmabufDescDXR, bufferId, 80);

	CHECK(sizeof(XrLiftFrameImagesDXR) == 32);
	PIN_OFF(XrLiftFrameImagesDXR, imageCount, 16);
	PIN_OFF(XrLiftFrameImagesDXR, images, 24);

	CHECK(sizeof(XrLiftFrameControlDXR) == 24);
	PIN_OFF(XrLiftFrameControlDXR, flags, 16);

	CHECK(sizeof(XrLiftResultExportDXR) == 32);
	PIN_OFF(XrLiftResultExportDXR, kind, 16);
	PIN_OFF(XrLiftResultExportDXR, format, 20);
	PIN_OFF(XrLiftResultExportDXR, colorSpace, 24);
	PIN_OFF(XrLiftResultExportDXR, syncKind, 28);

	CHECK(sizeof(XrLiftDepthInfoDXR) == 104);
	PIN_OFF(XrLiftDepthInfoDXR, kind, 16);
	PIN_OFF(XrLiftDepthInfoDXR, measure, 20);
	PIN_OFF(XrLiftDepthInfoDXR, rawScale, 24);
	PIN_OFF(XrLiftDepthInfoDXR, rawShift, 28);
	PIN_OFF(XrLiftDepthInfoDXR, invalidValueValid, 32);
	PIN_OFF(XrLiftDepthInfoDXR, nearZ, 40);
	PIN_OFF(XrLiftDepthInfoDXR, farZ, 44);
	PIN_OFF(XrLiftDepthInfoDXR, rangeValid, 48);
	PIN_OFF(XrLiftDepthInfoDXR, rangeP95, 64);
	PIN_OFF(XrLiftDepthInfoDXR, intrinsicsSource, 68);
	PIN_OFF(XrLiftDepthInfoDXR, focalLengthPx, 72);
	PIN_OFF(XrLiftDepthInfoDXR, principalPointPx, 80);
	PIN_OFF(XrLiftDepthInfoDXR, convention, 88);
	PIN_OFF(XrLiftDepthInfoDXR, temporallyConsistent, 92);
	PIN_OFF(XrLiftDepthInfoDXR, discontinuity, 96);
	PIN_OFF(XrLiftDepthInfoDXR, baselineMeters, 100);

	CHECK(sizeof(XrLiftViewsInfoDXR) == 136);
	PIN_OFF(XrLiftViewsInfoDXR, layout, 16);
	PIN_OFF(XrLiftViewsInfoDXR, viewCount, 20);
	PIN_OFF(XrLiftViewsInfoDXR, viewExtent, 32);
	PIN_OFF(XrLiftViewsInfoDXR, eyeOffsetsValid, 40);
	PIN_OFF(XrLiftViewsInfoDXR, eyeOffsetMeters, 44);
	PIN_OFF(XrLiftViewsInfoDXR, baselineMeters, 76);
	PIN_OFF(XrLiftViewsInfoDXR, cameraModel, 100);
	PIN_OFF(XrLiftViewsInfoDXR, convergenceApplied, 108);
	PIN_OFF(XrLiftViewsInfoDXR, reliefValid, 120);
	PIN_OFF(XrLiftViewsInfoDXR, reliefOffset, 128);
	STATIC_REQUIRE(sizeof(((XrLiftViewsInfoDXR *)0)->eyeOffsetMeters) / sizeof(float) == XR_LIFT_MAX_VIEWS_DXR);
}

TEST_CASE("lift ABI v4: entry points", "[lift_abi]")
{
	STATIC_REQUIRE(
	    std::is_same<PFN_xrEnumerateLiftTasksDXR,
	                 XrResult(XRAPI_PTR *)(XrSession, uint32_t, uint32_t *, XrLiftTaskPropertiesDXR *)>::value);
	STATIC_REQUIRE(
	    std::is_same<PFN_xrRequestLiftTaskWarmDXR, XrResult(XRAPI_PTR *)(XrSession, XrLiftTaskDXR)>::value);
	STATIC_REQUIRE(std::is_same<decltype(&xrEnumerateLiftTasksDXR), PFN_xrEnumerateLiftTasksDXR>::value);
	STATIC_REQUIRE(std::is_same<decltype(&xrRequestLiftTaskWarmDXR), PFN_xrRequestLiftTaskWarmDXR>::value);
}
