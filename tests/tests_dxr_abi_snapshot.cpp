// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ABI pin for the published XR_DXR_* extensions — GENERATED, do not edit.
 *
 * Regenerate with scripts/gen_dxr_abi_snapshot.py after a DELIBERATE change.
 * A failure here means a struct layout, an XrStructureType value or a
 * SPEC_VERSION moved; shipped apps (and consumers that hand-copy the values)
 * are compiled against the old one. See
 * docs/roadmap/khronos-proposal-binding-cleanup.md §4 Stage 1.
 */

#include "catch_amalgamated.hpp"

#include <cstddef>
#include <cstdint>
#include <openxr/openxr.h>
#include <openxr/XR_DXR_cocoa_window_binding.h>
#include <openxr/XR_DXR_win32_window_binding.h>
#include <openxr/XR_DXR_xlib_window_binding.h>
#include <openxr/XR_DXR_wayland_surface_binding.h>
#include <openxr/XR_DXR_android_surface_binding.h>
#include <openxr/XR_DXR_display_info.h>
#include <openxr/XR_DXR_view_rig.h>
#include <openxr/XR_DXR_local_3d_zone.h>
#include <openxr/XR_DXR_display_zones.h>

// ---- XR_DXR_cocoa_window_binding.h
static_assert(XR_DXR_cocoa_window_binding_SPEC_VERSION == 8, "XR_DXR_cocoa_window_binding_SPEC_VERSION moved");
static_assert(XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_DXR == (XrStructureType)1004999004, "XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_DXR renumbered");
static_assert(XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_DXR == (XrStructureType)1004999002, "XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_DXR renumbered");
static_assert(XR_TYPE_COCOA_WINDOW_PLACEMENT_INFO_DXR == (XrStructureType)1004999330, "XR_TYPE_COCOA_WINDOW_PLACEMENT_INFO_DXR renumbered");
#if UINTPTR_MAX == UINT64_MAX
static_assert(sizeof(XrCompositionLayerWindowSpaceDXR) == 80, "XrCompositionLayerWindowSpaceDXR size changed");
static_assert(offsetof(XrCompositionLayerWindowSpaceDXR, type) == 0, "XrCompositionLayerWindowSpaceDXR::type moved");
static_assert(offsetof(XrCompositionLayerWindowSpaceDXR, next) == 8, "XrCompositionLayerWindowSpaceDXR::next moved");
static_assert(offsetof(XrCompositionLayerWindowSpaceDXR, layerFlags) == 16, "XrCompositionLayerWindowSpaceDXR::layerFlags moved");
static_assert(offsetof(XrCompositionLayerWindowSpaceDXR, subImage) == 24, "XrCompositionLayerWindowSpaceDXR::subImage moved");
static_assert(offsetof(XrCompositionLayerWindowSpaceDXR, x) == 56, "XrCompositionLayerWindowSpaceDXR::x moved");
static_assert(offsetof(XrCompositionLayerWindowSpaceDXR, y) == 60, "XrCompositionLayerWindowSpaceDXR::y moved");
static_assert(offsetof(XrCompositionLayerWindowSpaceDXR, width) == 64, "XrCompositionLayerWindowSpaceDXR::width moved");
static_assert(offsetof(XrCompositionLayerWindowSpaceDXR, height) == 68, "XrCompositionLayerWindowSpaceDXR::height moved");
static_assert(offsetof(XrCompositionLayerWindowSpaceDXR, disparity) == 72, "XrCompositionLayerWindowSpaceDXR::disparity moved");
static_assert(sizeof(XrCocoaWindowBindingCreateInfoDXR) == 56, "XrCocoaWindowBindingCreateInfoDXR size changed");
static_assert(offsetof(XrCocoaWindowBindingCreateInfoDXR, type) == 0, "XrCocoaWindowBindingCreateInfoDXR::type moved");
static_assert(offsetof(XrCocoaWindowBindingCreateInfoDXR, next) == 8, "XrCocoaWindowBindingCreateInfoDXR::next moved");
static_assert(offsetof(XrCocoaWindowBindingCreateInfoDXR, viewHandle) == 16, "XrCocoaWindowBindingCreateInfoDXR::viewHandle moved");
static_assert(offsetof(XrCocoaWindowBindingCreateInfoDXR, readbackCallback) == 24, "XrCocoaWindowBindingCreateInfoDXR::readbackCallback moved");
static_assert(offsetof(XrCocoaWindowBindingCreateInfoDXR, readbackUserdata) == 32, "XrCocoaWindowBindingCreateInfoDXR::readbackUserdata moved");
static_assert(offsetof(XrCocoaWindowBindingCreateInfoDXR, sharedIOSurface) == 40, "XrCocoaWindowBindingCreateInfoDXR::sharedIOSurface moved");
static_assert(offsetof(XrCocoaWindowBindingCreateInfoDXR, transparentBackgroundEnabled) == 48, "XrCocoaWindowBindingCreateInfoDXR::transparentBackgroundEnabled moved");
static_assert(sizeof(XrCocoaWindowPlacementInfoDXR) == 24, "XrCocoaWindowPlacementInfoDXR size changed");
static_assert(offsetof(XrCocoaWindowPlacementInfoDXR, type) == 0, "XrCocoaWindowPlacementInfoDXR::type moved");
static_assert(offsetof(XrCocoaWindowPlacementInfoDXR, next) == 8, "XrCocoaWindowPlacementInfoDXR::next moved");
static_assert(offsetof(XrCocoaWindowPlacementInfoDXR, flags) == 16, "XrCocoaWindowPlacementInfoDXR::flags moved");
#endif

// ---- XR_DXR_win32_window_binding.h
static_assert(XR_DXR_win32_window_binding_SPEC_VERSION == 9, "XR_DXR_win32_window_binding_SPEC_VERSION moved");
static_assert(XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_DXR == (XrStructureType)1004999001, "XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_DXR renumbered");
static_assert(XR_TYPE_WIN32_TEXTURE_TARGET_BINDING_DXR == (XrStructureType)1004999015, "XR_TYPE_WIN32_TEXTURE_TARGET_BINDING_DXR renumbered");
static_assert(XR_TYPE_WIN32_TEXTURE_TARGET_FRAME_DXR == (XrStructureType)1004999016, "XR_TYPE_WIN32_TEXTURE_TARGET_FRAME_DXR renumbered");
static_assert(XR_TYPE_SYSTEM_WIN32_TEXTURE_TARGET_PROPERTIES_DXR == (XrStructureType)1004999017, "XR_TYPE_SYSTEM_WIN32_TEXTURE_TARGET_PROPERTIES_DXR renumbered");
static_assert(XR_TYPE_WIN32_TEXTURE_TARGET_STATE_DXR == (XrStructureType)1004999018, "XR_TYPE_WIN32_TEXTURE_TARGET_STATE_DXR renumbered");
#if UINTPTR_MAX == UINT64_MAX
static_assert(sizeof(XrWin32WindowBindingCreateInfoDXR) == 56, "XrWin32WindowBindingCreateInfoDXR size changed");
static_assert(offsetof(XrWin32WindowBindingCreateInfoDXR, type) == 0, "XrWin32WindowBindingCreateInfoDXR::type moved");
static_assert(offsetof(XrWin32WindowBindingCreateInfoDXR, next) == 8, "XrWin32WindowBindingCreateInfoDXR::next moved");
static_assert(offsetof(XrWin32WindowBindingCreateInfoDXR, windowHandle) == 16, "XrWin32WindowBindingCreateInfoDXR::windowHandle moved");
static_assert(offsetof(XrWin32WindowBindingCreateInfoDXR, readbackCallback) == 24, "XrWin32WindowBindingCreateInfoDXR::readbackCallback moved");
static_assert(offsetof(XrWin32WindowBindingCreateInfoDXR, readbackUserdata) == 32, "XrWin32WindowBindingCreateInfoDXR::readbackUserdata moved");
static_assert(offsetof(XrWin32WindowBindingCreateInfoDXR, sharedTextureHandle) == 40, "XrWin32WindowBindingCreateInfoDXR::sharedTextureHandle moved");
static_assert(offsetof(XrWin32WindowBindingCreateInfoDXR, transparentBackgroundEnabled) == 48, "XrWin32WindowBindingCreateInfoDXR::transparentBackgroundEnabled moved");
static_assert(sizeof(XrSystemWin32TextureTargetPropertiesDXR) == 24, "XrSystemWin32TextureTargetPropertiesDXR size changed");
static_assert(offsetof(XrSystemWin32TextureTargetPropertiesDXR, type) == 0, "XrSystemWin32TextureTargetPropertiesDXR::type moved");
static_assert(offsetof(XrSystemWin32TextureTargetPropertiesDXR, next) == 8, "XrSystemWin32TextureTargetPropertiesDXR::next moved");
static_assert(offsetof(XrSystemWin32TextureTargetPropertiesDXR, supportsTextureTarget) == 16, "XrSystemWin32TextureTargetPropertiesDXR::supportsTextureTarget moved");
static_assert(offsetof(XrSystemWin32TextureTargetPropertiesDXR, windowlessPhase) == 20, "XrSystemWin32TextureTargetPropertiesDXR::windowlessPhase moved");
static_assert(sizeof(XrWin32TextureTargetBindingDXR) == 40, "XrWin32TextureTargetBindingDXR size changed");
static_assert(offsetof(XrWin32TextureTargetBindingDXR, type) == 0, "XrWin32TextureTargetBindingDXR::type moved");
static_assert(offsetof(XrWin32TextureTargetBindingDXR, next) == 8, "XrWin32TextureTargetBindingDXR::next moved");
static_assert(offsetof(XrWin32TextureTargetBindingDXR, referenceWindow) == 16, "XrWin32TextureTargetBindingDXR::referenceWindow moved");
static_assert(offsetof(XrWin32TextureTargetBindingDXR, initialTargetRect) == 24, "XrWin32TextureTargetBindingDXR::initialTargetRect moved");
static_assert(sizeof(XrWin32TextureTargetFrameDXR) == 48, "XrWin32TextureTargetFrameDXR size changed");
static_assert(offsetof(XrWin32TextureTargetFrameDXR, type) == 0, "XrWin32TextureTargetFrameDXR::type moved");
static_assert(offsetof(XrWin32TextureTargetFrameDXR, next) == 8, "XrWin32TextureTargetFrameDXR::next moved");
static_assert(offsetof(XrWin32TextureTargetFrameDXR, referenceWindow) == 16, "XrWin32TextureTargetFrameDXR::referenceWindow moved");
static_assert(offsetof(XrWin32TextureTargetFrameDXR, targetRect) == 24, "XrWin32TextureTargetFrameDXR::targetRect moved");
static_assert(offsetof(XrWin32TextureTargetFrameDXR, flags) == 40, "XrWin32TextureTargetFrameDXR::flags moved");
static_assert(sizeof(XrWin32TextureTargetStateDXR) == 56, "XrWin32TextureTargetStateDXR size changed");
static_assert(offsetof(XrWin32TextureTargetStateDXR, type) == 0, "XrWin32TextureTargetStateDXR::type moved");
static_assert(offsetof(XrWin32TextureTargetStateDXR, next) == 8, "XrWin32TextureTargetStateDXR::next moved");
static_assert(offsetof(XrWin32TextureTargetStateDXR, flags) == 16, "XrWin32TextureTargetStateDXR::flags moved");
static_assert(offsetof(XrWin32TextureTargetStateDXR, wovenScreenOrigin) == 24, "XrWin32TextureTargetStateDXR::wovenScreenOrigin moved");
static_assert(offsetof(XrWin32TextureTargetStateDXR, wovenTargetRect) == 32, "XrWin32TextureTargetStateDXR::wovenTargetRect moved");
static_assert(offsetof(XrWin32TextureTargetStateDXR, wovenDisplayTime) == 48, "XrWin32TextureTargetStateDXR::wovenDisplayTime moved");
#endif

// ---- XR_DXR_xlib_window_binding.h
#if defined(__linux__) && !defined(__ANDROID__)
static_assert(XR_DXR_xlib_window_binding_SPEC_VERSION == 2, "XR_DXR_xlib_window_binding_SPEC_VERSION moved");
static_assert(XR_TYPE_XLIB_WINDOW_BINDING_CREATE_INFO_DXR == (XrStructureType)1004999200, "XR_TYPE_XLIB_WINDOW_BINDING_CREATE_INFO_DXR renumbered");
#if UINTPTR_MAX == UINT64_MAX
static_assert(sizeof(XrXlibWindowBindingCreateInfoDXR) == 40, "XrXlibWindowBindingCreateInfoDXR size changed");
static_assert(offsetof(XrXlibWindowBindingCreateInfoDXR, type) == 0, "XrXlibWindowBindingCreateInfoDXR::type moved");
static_assert(offsetof(XrXlibWindowBindingCreateInfoDXR, next) == 8, "XrXlibWindowBindingCreateInfoDXR::next moved");
static_assert(offsetof(XrXlibWindowBindingCreateInfoDXR, xDisplay) == 16, "XrXlibWindowBindingCreateInfoDXR::xDisplay moved");
static_assert(offsetof(XrXlibWindowBindingCreateInfoDXR, window) == 24, "XrXlibWindowBindingCreateInfoDXR::window moved");
static_assert(offsetof(XrXlibWindowBindingCreateInfoDXR, transparentBackgroundEnabled) == 32, "XrXlibWindowBindingCreateInfoDXR::transparentBackgroundEnabled moved");
#endif
#endif // defined(__linux__) && !defined(__ANDROID__)

// ---- XR_DXR_wayland_surface_binding.h
#if defined(__linux__) && !defined(__ANDROID__)
static_assert(XR_DXR_wayland_surface_binding_SPEC_VERSION == 2, "XR_DXR_wayland_surface_binding_SPEC_VERSION moved");
static_assert(XR_TYPE_WAYLAND_SURFACE_BINDING_CREATE_INFO_DXR == (XrStructureType)1004999250, "XR_TYPE_WAYLAND_SURFACE_BINDING_CREATE_INFO_DXR renumbered");
static_assert(XR_TYPE_WAYLAND_SURFACE_GEOMETRY_DXR == (XrStructureType)1004999251, "XR_TYPE_WAYLAND_SURFACE_GEOMETRY_DXR renumbered");
#if UINTPTR_MAX == UINT64_MAX
static_assert(sizeof(XrWaylandSurfaceBindingCreateInfoDXR) == 40, "XrWaylandSurfaceBindingCreateInfoDXR size changed");
static_assert(offsetof(XrWaylandSurfaceBindingCreateInfoDXR, type) == 0, "XrWaylandSurfaceBindingCreateInfoDXR::type moved");
static_assert(offsetof(XrWaylandSurfaceBindingCreateInfoDXR, next) == 8, "XrWaylandSurfaceBindingCreateInfoDXR::next moved");
static_assert(offsetof(XrWaylandSurfaceBindingCreateInfoDXR, wlDisplay) == 16, "XrWaylandSurfaceBindingCreateInfoDXR::wlDisplay moved");
static_assert(offsetof(XrWaylandSurfaceBindingCreateInfoDXR, wlSurface) == 24, "XrWaylandSurfaceBindingCreateInfoDXR::wlSurface moved");
static_assert(offsetof(XrWaylandSurfaceBindingCreateInfoDXR, transparentBackgroundEnabled) == 32, "XrWaylandSurfaceBindingCreateInfoDXR::transparentBackgroundEnabled moved");
static_assert(sizeof(XrWaylandSurfaceGeometryDXR) == 32, "XrWaylandSurfaceGeometryDXR size changed");
static_assert(offsetof(XrWaylandSurfaceGeometryDXR, type) == 0, "XrWaylandSurfaceGeometryDXR::type moved");
static_assert(offsetof(XrWaylandSurfaceGeometryDXR, next) == 8, "XrWaylandSurfaceGeometryDXR::next moved");
static_assert(offsetof(XrWaylandSurfaceGeometryDXR, width) == 16, "XrWaylandSurfaceGeometryDXR::width moved");
static_assert(offsetof(XrWaylandSurfaceGeometryDXR, height) == 20, "XrWaylandSurfaceGeometryDXR::height moved");
static_assert(offsetof(XrWaylandSurfaceGeometryDXR, refreshMilliHertz) == 24, "XrWaylandSurfaceGeometryDXR::refreshMilliHertz moved");
#endif
#endif // defined(__linux__) && !defined(__ANDROID__)

// ---- XR_DXR_android_surface_binding.h
#if defined(__ANDROID__)
static_assert(XR_DXR_android_surface_binding_SPEC_VERSION == 3, "XR_DXR_android_surface_binding_SPEC_VERSION moved");
static_assert(XR_TYPE_ANDROID_SURFACE_BINDING_CREATE_INFO_DXR == (XrStructureType)1004999005, "XR_TYPE_ANDROID_SURFACE_BINDING_CREATE_INFO_DXR renumbered");
static_assert(XR_TYPE_ANDROID_WINDOW_GEOMETRY_DXR == (XrStructureType)1004999220, "XR_TYPE_ANDROID_WINDOW_GEOMETRY_DXR renumbered");
static_assert(XR_TYPE_EVENT_DATA_ANDROID_WINDOW_LAYOUT_HINT_DXR == (XrStructureType)1004999221, "XR_TYPE_EVENT_DATA_ANDROID_WINDOW_LAYOUT_HINT_DXR renumbered");
static_assert(XR_TYPE_ANDROID_INPUT_EVENT_DXR == (XrStructureType)1004999222, "XR_TYPE_ANDROID_INPUT_EVENT_DXR renumbered");
#if UINTPTR_MAX == UINT64_MAX
static_assert(sizeof(XrAndroidSurfaceBindingCreateInfoDXR) == 48, "XrAndroidSurfaceBindingCreateInfoDXR size changed");
static_assert(offsetof(XrAndroidSurfaceBindingCreateInfoDXR, type) == 0, "XrAndroidSurfaceBindingCreateInfoDXR::type moved");
static_assert(offsetof(XrAndroidSurfaceBindingCreateInfoDXR, next) == 8, "XrAndroidSurfaceBindingCreateInfoDXR::next moved");
static_assert(offsetof(XrAndroidSurfaceBindingCreateInfoDXR, nativeWindow) == 16, "XrAndroidSurfaceBindingCreateInfoDXR::nativeWindow moved");
static_assert(offsetof(XrAndroidSurfaceBindingCreateInfoDXR, surface) == 24, "XrAndroidSurfaceBindingCreateInfoDXR::surface moved");
static_assert(offsetof(XrAndroidSurfaceBindingCreateInfoDXR, screenOffsetX) == 32, "XrAndroidSurfaceBindingCreateInfoDXR::screenOffsetX moved");
static_assert(offsetof(XrAndroidSurfaceBindingCreateInfoDXR, screenOffsetY) == 36, "XrAndroidSurfaceBindingCreateInfoDXR::screenOffsetY moved");
static_assert(offsetof(XrAndroidSurfaceBindingCreateInfoDXR, transparentBackgroundEnabled) == 40, "XrAndroidSurfaceBindingCreateInfoDXR::transparentBackgroundEnabled moved");
static_assert(sizeof(XrAndroidWindowGeometryDXR) == 48, "XrAndroidWindowGeometryDXR size changed");
static_assert(offsetof(XrAndroidWindowGeometryDXR, type) == 0, "XrAndroidWindowGeometryDXR::type moved");
static_assert(offsetof(XrAndroidWindowGeometryDXR, next) == 8, "XrAndroidWindowGeometryDXR::next moved");
static_assert(offsetof(XrAndroidWindowGeometryDXR, windowRect) == 16, "XrAndroidWindowGeometryDXR::windowRect moved");
static_assert(offsetof(XrAndroidWindowGeometryDXR, panelExtent) == 32, "XrAndroidWindowGeometryDXR::panelExtent moved");
static_assert(offsetof(XrAndroidWindowGeometryDXR, displayId) == 40, "XrAndroidWindowGeometryDXR::displayId moved");
static_assert(sizeof(XrEventDataAndroidWindowLayoutHintDXR) == 64, "XrEventDataAndroidWindowLayoutHintDXR size changed");
static_assert(offsetof(XrEventDataAndroidWindowLayoutHintDXR, type) == 0, "XrEventDataAndroidWindowLayoutHintDXR::type moved");
static_assert(offsetof(XrEventDataAndroidWindowLayoutHintDXR, next) == 8, "XrEventDataAndroidWindowLayoutHintDXR::next moved");
static_assert(offsetof(XrEventDataAndroidWindowLayoutHintDXR, session) == 16, "XrEventDataAndroidWindowLayoutHintDXR::session moved");
static_assert(offsetof(XrEventDataAndroidWindowLayoutHintDXR, active) == 24, "XrEventDataAndroidWindowLayoutHintDXR::active moved");
static_assert(offsetof(XrEventDataAndroidWindowLayoutHintDXR, scale) == 28, "XrEventDataAndroidWindowLayoutHintDXR::scale moved");
static_assert(offsetof(XrEventDataAndroidWindowLayoutHintDXR, layoutSize) == 32, "XrEventDataAndroidWindowLayoutHintDXR::layoutSize moved");
static_assert(offsetof(XrEventDataAndroidWindowLayoutHintDXR, bufferSize) == 40, "XrEventDataAndroidWindowLayoutHintDXR::bufferSize moved");
static_assert(offsetof(XrEventDataAndroidWindowLayoutHintDXR, physicalRect) == 48, "XrEventDataAndroidWindowLayoutHintDXR::physicalRect moved");
static_assert(sizeof(XrAndroidInputEventDXR) == 56, "XrAndroidInputEventDXR size changed");
static_assert(offsetof(XrAndroidInputEventDXR, type) == 0, "XrAndroidInputEventDXR::type moved");
static_assert(offsetof(XrAndroidInputEventDXR, next) == 8, "XrAndroidInputEventDXR::next moved");
static_assert(offsetof(XrAndroidInputEventDXR, action) == 16, "XrAndroidInputEventDXR::action moved");
static_assert(offsetof(XrAndroidInputEventDXR, pointerId) == 20, "XrAndroidInputEventDXR::pointerId moved");
static_assert(offsetof(XrAndroidInputEventDXR, pointerCount) == 24, "XrAndroidInputEventDXR::pointerCount moved");
static_assert(offsetof(XrAndroidInputEventDXR, position) == 28, "XrAndroidInputEventDXR::position moved");
static_assert(offsetof(XrAndroidInputEventDXR, surfaceExtent) == 36, "XrAndroidInputEventDXR::surfaceExtent moved");
static_assert(offsetof(XrAndroidInputEventDXR, eventTimeNs) == 48, "XrAndroidInputEventDXR::eventTimeNs moved");
#endif
#endif // defined(__ANDROID__)

// ---- XR_DXR_display_info.h
static_assert(XR_DXR_display_info_SPEC_VERSION == 23, "XR_DXR_display_info_SPEC_VERSION moved");
static_assert(XR_TYPE_DISPLAY_INFO_DXR == (XrStructureType)1004999003, "XR_TYPE_DISPLAY_INFO_DXR renumbered");
static_assert(XR_TYPE_DISPLAY_DESKTOP_POSITION_DXR == (XrStructureType)1004999210, "XR_TYPE_DISPLAY_DESKTOP_POSITION_DXR renumbered");
static_assert(XR_TYPE_DISPLAY_DESKTOP_INFO_DXR == (XrStructureType)1004999211, "XR_TYPE_DISPLAY_DESKTOP_INFO_DXR renumbered");
static_assert(XR_TYPE_EYE_TRACKING_MODE_CAPABILITIES_DXR == (XrStructureType)1004999006, "XR_TYPE_EYE_TRACKING_MODE_CAPABILITIES_DXR renumbered");
static_assert(XR_TYPE_VIEW_EYE_TRACKING_STATE_DXR == (XrStructureType)1004999007, "XR_TYPE_VIEW_EYE_TRACKING_STATE_DXR renumbered");
static_assert(XR_TYPE_DISPLAY_RENDERING_MODE_INFO_DXR == (XrStructureType)1004999008, "XR_TYPE_DISPLAY_RENDERING_MODE_INFO_DXR renumbered");
static_assert(XR_TYPE_EVENT_DATA_RENDERING_MODE_CHANGED_DXR == (XrStructureType)1004999010, "XR_TYPE_EVENT_DATA_RENDERING_MODE_CHANGED_DXR renumbered");
static_assert(XR_TYPE_EVENT_DATA_HARDWARE_DISPLAY_STATE_CHANGED_DXR == (XrStructureType)1004999011, "XR_TYPE_EVENT_DATA_HARDWARE_DISPLAY_STATE_CHANGED_DXR renumbered");
static_assert(XR_TYPE_DISPLAY_RENDERING_MODE_TRACKING_INFO_DXR == (XrStructureType)1004999012, "XR_TYPE_DISPLAY_RENDERING_MODE_TRACKING_INFO_DXR renumbered");
static_assert(XR_TYPE_EVENT_DATA_EYE_TRACKING_STATE_CHANGED_DXR == (XrStructureType)1004999013, "XR_TYPE_EVENT_DATA_EYE_TRACKING_STATE_CHANGED_DXR renumbered");
static_assert(XR_TYPE_EVENT_DATA_DISPLAY_MODE_REQUEST_DENIED_DXR == (XrStructureType)1004999014, "XR_TYPE_EVENT_DATA_DISPLAY_MODE_REQUEST_DENIED_DXR renumbered");
static_assert(XR_TYPE_VIEW_ACTIVITY_STATE_DXR == (XrStructureType)1004999213, "XR_TYPE_VIEW_ACTIVITY_STATE_DXR renumbered");
static_assert(XR_TYPE_DISPLAY_DXR == (XrStructureType)1004999214, "XR_TYPE_DISPLAY_DXR renumbered");
static_assert(XR_TYPE_DISPLAY_SPACE_CREATE_INFO_DXR == (XrStructureType)1004999215, "XR_TYPE_DISPLAY_SPACE_CREATE_INFO_DXR renumbered");
static_assert(XR_TYPE_SESSION_DISPLAY_BINDING_DXR == (XrStructureType)1004999216, "XR_TYPE_SESSION_DISPLAY_BINDING_DXR renumbered");
static_assert(XR_TYPE_VIEW_DISPLAY_BINDINGS_DXR == (XrStructureType)1004999217, "XR_TYPE_VIEW_DISPLAY_BINDINGS_DXR renumbered");
static_assert(XR_TYPE_EVENT_DATA_DISPLAYS_CHANGED_DXR == (XrStructureType)1004999218, "XR_TYPE_EVENT_DATA_DISPLAYS_CHANGED_DXR renumbered");
#if UINTPTR_MAX == UINT64_MAX
static_assert(sizeof(XrDisplayInfoDXR) == 56, "XrDisplayInfoDXR size changed");
static_assert(offsetof(XrDisplayInfoDXR, type) == 0, "XrDisplayInfoDXR::type moved");
static_assert(offsetof(XrDisplayInfoDXR, next) == 8, "XrDisplayInfoDXR::next moved");
static_assert(offsetof(XrDisplayInfoDXR, displaySizeMeters) == 16, "XrDisplayInfoDXR::displaySizeMeters moved");
static_assert(offsetof(XrDisplayInfoDXR, nominalViewerPositionInDisplaySpace) == 24, "XrDisplayInfoDXR::nominalViewerPositionInDisplaySpace moved");
static_assert(offsetof(XrDisplayInfoDXR, recommendedViewScaleX) == 36, "XrDisplayInfoDXR::recommendedViewScaleX moved");
static_assert(offsetof(XrDisplayInfoDXR, recommendedViewScaleY) == 40, "XrDisplayInfoDXR::recommendedViewScaleY moved");
static_assert(offsetof(XrDisplayInfoDXR, displayPixelWidth) == 44, "XrDisplayInfoDXR::displayPixelWidth moved");
static_assert(offsetof(XrDisplayInfoDXR, displayPixelHeight) == 48, "XrDisplayInfoDXR::displayPixelHeight moved");
static_assert(sizeof(XrDisplayDesktopPositionDXR) == 24, "XrDisplayDesktopPositionDXR size changed");
static_assert(offsetof(XrDisplayDesktopPositionDXR, type) == 0, "XrDisplayDesktopPositionDXR::type moved");
static_assert(offsetof(XrDisplayDesktopPositionDXR, next) == 8, "XrDisplayDesktopPositionDXR::next moved");
static_assert(offsetof(XrDisplayDesktopPositionDXR, left) == 16, "XrDisplayDesktopPositionDXR::left moved");
static_assert(offsetof(XrDisplayDesktopPositionDXR, top) == 20, "XrDisplayDesktopPositionDXR::top moved");
static_assert(sizeof(XrDisplayDesktopInfoDXR) == 168, "XrDisplayDesktopInfoDXR size changed");
static_assert(offsetof(XrDisplayDesktopInfoDXR, type) == 0, "XrDisplayDesktopInfoDXR::type moved");
static_assert(offsetof(XrDisplayDesktopInfoDXR, next) == 8, "XrDisplayDesktopInfoDXR::next moved");
static_assert(offsetof(XrDisplayDesktopInfoDXR, desktopRect) == 16, "XrDisplayDesktopInfoDXR::desktopRect moved");
static_assert(offsetof(XrDisplayDesktopInfoDXR, deviceName) == 32, "XrDisplayDesktopInfoDXR::deviceName moved");
static_assert(offsetof(XrDisplayDesktopInfoDXR, isPrimary) == 160, "XrDisplayDesktopInfoDXR::isPrimary moved");
static_assert(offsetof(XrDisplayDesktopInfoDXR, isPanelConfirmed) == 164, "XrDisplayDesktopInfoDXR::isPanelConfirmed moved");
static_assert(sizeof(XrEyeTrackingModeCapabilitiesDXR) == 32, "XrEyeTrackingModeCapabilitiesDXR size changed");
static_assert(offsetof(XrEyeTrackingModeCapabilitiesDXR, type) == 0, "XrEyeTrackingModeCapabilitiesDXR::type moved");
static_assert(offsetof(XrEyeTrackingModeCapabilitiesDXR, next) == 8, "XrEyeTrackingModeCapabilitiesDXR::next moved");
static_assert(offsetof(XrEyeTrackingModeCapabilitiesDXR, supportedModes) == 16, "XrEyeTrackingModeCapabilitiesDXR::supportedModes moved");
static_assert(offsetof(XrEyeTrackingModeCapabilitiesDXR, defaultMode) == 24, "XrEyeTrackingModeCapabilitiesDXR::defaultMode moved");
static_assert(sizeof(XrViewEyeTrackingStateDXR) == 24, "XrViewEyeTrackingStateDXR size changed");
static_assert(offsetof(XrViewEyeTrackingStateDXR, type) == 0, "XrViewEyeTrackingStateDXR::type moved");
static_assert(offsetof(XrViewEyeTrackingStateDXR, next) == 8, "XrViewEyeTrackingStateDXR::next moved");
static_assert(offsetof(XrViewEyeTrackingStateDXR, isTracking) == 16, "XrViewEyeTrackingStateDXR::isTracking moved");
static_assert(offsetof(XrViewEyeTrackingStateDXR, activeMode) == 20, "XrViewEyeTrackingStateDXR::activeMode moved");
static_assert(sizeof(XrDisplayRenderingModeInfoDXR) == 320, "XrDisplayRenderingModeInfoDXR size changed");
static_assert(offsetof(XrDisplayRenderingModeInfoDXR, type) == 0, "XrDisplayRenderingModeInfoDXR::type moved");
static_assert(offsetof(XrDisplayRenderingModeInfoDXR, next) == 8, "XrDisplayRenderingModeInfoDXR::next moved");
static_assert(offsetof(XrDisplayRenderingModeInfoDXR, modeIndex) == 16, "XrDisplayRenderingModeInfoDXR::modeIndex moved");
static_assert(offsetof(XrDisplayRenderingModeInfoDXR, modeName) == 20, "XrDisplayRenderingModeInfoDXR::modeName moved");
static_assert(offsetof(XrDisplayRenderingModeInfoDXR, viewCount) == 276, "XrDisplayRenderingModeInfoDXR::viewCount moved");
static_assert(offsetof(XrDisplayRenderingModeInfoDXR, viewScaleX) == 280, "XrDisplayRenderingModeInfoDXR::viewScaleX moved");
static_assert(offsetof(XrDisplayRenderingModeInfoDXR, viewScaleY) == 284, "XrDisplayRenderingModeInfoDXR::viewScaleY moved");
static_assert(offsetof(XrDisplayRenderingModeInfoDXR, hardwareDisplay3D) == 288, "XrDisplayRenderingModeInfoDXR::hardwareDisplay3D moved");
static_assert(offsetof(XrDisplayRenderingModeInfoDXR, tileColumns) == 292, "XrDisplayRenderingModeInfoDXR::tileColumns moved");
static_assert(offsetof(XrDisplayRenderingModeInfoDXR, tileRows) == 296, "XrDisplayRenderingModeInfoDXR::tileRows moved");
static_assert(offsetof(XrDisplayRenderingModeInfoDXR, viewWidthPixels) == 300, "XrDisplayRenderingModeInfoDXR::viewWidthPixels moved");
static_assert(offsetof(XrDisplayRenderingModeInfoDXR, viewHeightPixels) == 304, "XrDisplayRenderingModeInfoDXR::viewHeightPixels moved");
static_assert(offsetof(XrDisplayRenderingModeInfoDXR, isActive) == 308, "XrDisplayRenderingModeInfoDXR::isActive moved");
static_assert(offsetof(XrDisplayRenderingModeInfoDXR, isRequestable) == 312, "XrDisplayRenderingModeInfoDXR::isRequestable moved");
static_assert(sizeof(XrEventDataRenderingModeChangedDXR) == 32, "XrEventDataRenderingModeChangedDXR size changed");
static_assert(offsetof(XrEventDataRenderingModeChangedDXR, type) == 0, "XrEventDataRenderingModeChangedDXR::type moved");
static_assert(offsetof(XrEventDataRenderingModeChangedDXR, next) == 8, "XrEventDataRenderingModeChangedDXR::next moved");
static_assert(offsetof(XrEventDataRenderingModeChangedDXR, session) == 16, "XrEventDataRenderingModeChangedDXR::session moved");
static_assert(offsetof(XrEventDataRenderingModeChangedDXR, previousModeIndex) == 24, "XrEventDataRenderingModeChangedDXR::previousModeIndex moved");
static_assert(offsetof(XrEventDataRenderingModeChangedDXR, currentModeIndex) == 28, "XrEventDataRenderingModeChangedDXR::currentModeIndex moved");
static_assert(sizeof(XrEventDataHardwareDisplayStateChangedDXR) == 32, "XrEventDataHardwareDisplayStateChangedDXR size changed");
static_assert(offsetof(XrEventDataHardwareDisplayStateChangedDXR, type) == 0, "XrEventDataHardwareDisplayStateChangedDXR::type moved");
static_assert(offsetof(XrEventDataHardwareDisplayStateChangedDXR, next) == 8, "XrEventDataHardwareDisplayStateChangedDXR::next moved");
static_assert(offsetof(XrEventDataHardwareDisplayStateChangedDXR, session) == 16, "XrEventDataHardwareDisplayStateChangedDXR::session moved");
static_assert(offsetof(XrEventDataHardwareDisplayStateChangedDXR, hardwareDisplay3D) == 24, "XrEventDataHardwareDisplayStateChangedDXR::hardwareDisplay3D moved");
static_assert(sizeof(XrDisplayRenderingModeTrackingInfoDXR) == 24, "XrDisplayRenderingModeTrackingInfoDXR size changed");
static_assert(offsetof(XrDisplayRenderingModeTrackingInfoDXR, type) == 0, "XrDisplayRenderingModeTrackingInfoDXR::type moved");
static_assert(offsetof(XrDisplayRenderingModeTrackingInfoDXR, next) == 8, "XrDisplayRenderingModeTrackingInfoDXR::next moved");
static_assert(offsetof(XrDisplayRenderingModeTrackingInfoDXR, hasTracking) == 16, "XrDisplayRenderingModeTrackingInfoDXR::hasTracking moved");
static_assert(sizeof(XrEventDataEyeTrackingStateChangedDXR) == 32, "XrEventDataEyeTrackingStateChangedDXR size changed");
static_assert(offsetof(XrEventDataEyeTrackingStateChangedDXR, type) == 0, "XrEventDataEyeTrackingStateChangedDXR::type moved");
static_assert(offsetof(XrEventDataEyeTrackingStateChangedDXR, next) == 8, "XrEventDataEyeTrackingStateChangedDXR::next moved");
static_assert(offsetof(XrEventDataEyeTrackingStateChangedDXR, session) == 16, "XrEventDataEyeTrackingStateChangedDXR::session moved");
static_assert(offsetof(XrEventDataEyeTrackingStateChangedDXR, isTracking) == 24, "XrEventDataEyeTrackingStateChangedDXR::isTracking moved");
static_assert(offsetof(XrEventDataEyeTrackingStateChangedDXR, activeMode) == 28, "XrEventDataEyeTrackingStateChangedDXR::activeMode moved");
static_assert(sizeof(XrEventDataDisplayModeRequestDeniedDXR) == 40, "XrEventDataDisplayModeRequestDeniedDXR size changed");
static_assert(offsetof(XrEventDataDisplayModeRequestDeniedDXR, type) == 0, "XrEventDataDisplayModeRequestDeniedDXR::type moved");
static_assert(offsetof(XrEventDataDisplayModeRequestDeniedDXR, next) == 8, "XrEventDataDisplayModeRequestDeniedDXR::next moved");
static_assert(offsetof(XrEventDataDisplayModeRequestDeniedDXR, session) == 16, "XrEventDataDisplayModeRequestDeniedDXR::session moved");
static_assert(offsetof(XrEventDataDisplayModeRequestDeniedDXR, requestedModeIndex) == 24, "XrEventDataDisplayModeRequestDeniedDXR::requestedModeIndex moved");
static_assert(offsetof(XrEventDataDisplayModeRequestDeniedDXR, requestedHardware3D) == 28, "XrEventDataDisplayModeRequestDeniedDXR::requestedHardware3D moved");
static_assert(offsetof(XrEventDataDisplayModeRequestDeniedDXR, reason) == 32, "XrEventDataDisplayModeRequestDeniedDXR::reason moved");
static_assert(sizeof(XrViewActivityStateDXR) == 24, "XrViewActivityStateDXR size changed");
static_assert(offsetof(XrViewActivityStateDXR, type) == 0, "XrViewActivityStateDXR::type moved");
static_assert(offsetof(XrViewActivityStateDXR, next) == 8, "XrViewActivityStateDXR::next moved");
static_assert(offsetof(XrViewActivityStateDXR, activeViewCount) == 16, "XrViewActivityStateDXR::activeViewCount moved");
static_assert(sizeof(XrDisplayDXR) == 280, "XrDisplayDXR size changed");
static_assert(offsetof(XrDisplayDXR, type) == 0, "XrDisplayDXR::type moved");
static_assert(offsetof(XrDisplayDXR, next) == 8, "XrDisplayDXR::next moved");
static_assert(offsetof(XrDisplayDXR, displayId) == 16, "XrDisplayDXR::displayId moved");
static_assert(offsetof(XrDisplayDXR, info) == 24, "XrDisplayDXR::info moved");
static_assert(offsetof(XrDisplayDXR, desktopRect) == 80, "XrDisplayDXR::desktopRect moved");
static_assert(offsetof(XrDisplayDXR, nativePixelWidth) == 96, "XrDisplayDXR::nativePixelWidth moved");
static_assert(offsetof(XrDisplayDXR, nativePixelHeight) == 100, "XrDisplayDXR::nativePixelHeight moved");
static_assert(offsetof(XrDisplayDXR, desktopScale) == 104, "XrDisplayDXR::desktopScale moved");
static_assert(offsetof(XrDisplayDXR, eyeTracking) == 112, "XrDisplayDXR::eyeTracking moved");
static_assert(offsetof(XrDisplayDXR, flags) == 144, "XrDisplayDXR::flags moved");
static_assert(offsetof(XrDisplayDXR, vendorPluginId) == 152, "XrDisplayDXR::vendorPluginId moved");
static_assert(offsetof(XrDisplayDXR, deviceName) == 216, "XrDisplayDXR::deviceName moved");
static_assert(sizeof(XrDisplaySpaceCreateInfoDXR) == 56, "XrDisplaySpaceCreateInfoDXR size changed");
static_assert(offsetof(XrDisplaySpaceCreateInfoDXR, type) == 0, "XrDisplaySpaceCreateInfoDXR::type moved");
static_assert(offsetof(XrDisplaySpaceCreateInfoDXR, next) == 8, "XrDisplaySpaceCreateInfoDXR::next moved");
static_assert(offsetof(XrDisplaySpaceCreateInfoDXR, displayId) == 16, "XrDisplaySpaceCreateInfoDXR::displayId moved");
static_assert(offsetof(XrDisplaySpaceCreateInfoDXR, poseInDisplaySpace) == 24, "XrDisplaySpaceCreateInfoDXR::poseInDisplaySpace moved");
static_assert(sizeof(XrSessionDisplayBindingDXR) == 24, "XrSessionDisplayBindingDXR size changed");
static_assert(offsetof(XrSessionDisplayBindingDXR, type) == 0, "XrSessionDisplayBindingDXR::type moved");
static_assert(offsetof(XrSessionDisplayBindingDXR, next) == 8, "XrSessionDisplayBindingDXR::next moved");
static_assert(offsetof(XrSessionDisplayBindingDXR, displayId) == 16, "XrSessionDisplayBindingDXR::displayId moved");
static_assert(sizeof(XrViewDisplayBindingDXR) == 32, "XrViewDisplayBindingDXR size changed");
static_assert(offsetof(XrViewDisplayBindingDXR, displayId) == 0, "XrViewDisplayBindingDXR::displayId moved");
static_assert(offsetof(XrViewDisplayBindingDXR, segmentRect) == 8, "XrViewDisplayBindingDXR::segmentRect moved");
static_assert(offsetof(XrViewDisplayBindingDXR, firstView) == 24, "XrViewDisplayBindingDXR::firstView moved");
static_assert(offsetof(XrViewDisplayBindingDXR, viewCount) == 28, "XrViewDisplayBindingDXR::viewCount moved");
static_assert(sizeof(XrViewDisplayBindingsDXR) == 32, "XrViewDisplayBindingsDXR size changed");
static_assert(offsetof(XrViewDisplayBindingsDXR, type) == 0, "XrViewDisplayBindingsDXR::type moved");
static_assert(offsetof(XrViewDisplayBindingsDXR, next) == 8, "XrViewDisplayBindingsDXR::next moved");
static_assert(offsetof(XrViewDisplayBindingsDXR, bindingCapacityInput) == 16, "XrViewDisplayBindingsDXR::bindingCapacityInput moved");
static_assert(offsetof(XrViewDisplayBindingsDXR, bindingCountOutput) == 20, "XrViewDisplayBindingsDXR::bindingCountOutput moved");
static_assert(offsetof(XrViewDisplayBindingsDXR, bindings) == 24, "XrViewDisplayBindingsDXR::bindings moved");
static_assert(sizeof(XrEventDataDisplaysChangedDXR) == 32, "XrEventDataDisplaysChangedDXR size changed");
static_assert(offsetof(XrEventDataDisplaysChangedDXR, type) == 0, "XrEventDataDisplaysChangedDXR::type moved");
static_assert(offsetof(XrEventDataDisplaysChangedDXR, next) == 8, "XrEventDataDisplaysChangedDXR::next moved");
static_assert(offsetof(XrEventDataDisplaysChangedDXR, session) == 16, "XrEventDataDisplaysChangedDXR::session moved");
static_assert(offsetof(XrEventDataDisplaysChangedDXR, displayCount) == 24, "XrEventDataDisplaysChangedDXR::displayCount moved");
#endif

// ---- XR_DXR_view_rig.h
static_assert(XR_DXR_view_rig_SPEC_VERSION == 4, "XR_DXR_view_rig_SPEC_VERSION moved");
static_assert(XR_TYPE_DISPLAY_RIG_DXR == (XrStructureType)1004999140, "XR_TYPE_DISPLAY_RIG_DXR renumbered");
static_assert(XR_TYPE_CAMERA_RIG_DXR == (XrStructureType)1004999141, "XR_TYPE_CAMERA_RIG_DXR renumbered");
static_assert(XR_TYPE_VIEW_DISPLAY_RAW_DXR == (XrStructureType)1004999142, "XR_TYPE_VIEW_DISPLAY_RAW_DXR renumbered");
#if UINTPTR_MAX == UINT64_MAX
static_assert(sizeof(XrDisplayRigDXR) == 64, "XrDisplayRigDXR size changed");
static_assert(offsetof(XrDisplayRigDXR, type) == 0, "XrDisplayRigDXR::type moved");
static_assert(offsetof(XrDisplayRigDXR, next) == 8, "XrDisplayRigDXR::next moved");
static_assert(offsetof(XrDisplayRigDXR, pose) == 16, "XrDisplayRigDXR::pose moved");
static_assert(offsetof(XrDisplayRigDXR, virtualDisplayHeight) == 44, "XrDisplayRigDXR::virtualDisplayHeight moved");
static_assert(offsetof(XrDisplayRigDXR, ipdFactor) == 48, "XrDisplayRigDXR::ipdFactor moved");
static_assert(offsetof(XrDisplayRigDXR, parallaxFactor) == 52, "XrDisplayRigDXR::parallaxFactor moved");
static_assert(offsetof(XrDisplayRigDXR, perspectiveFactor) == 56, "XrDisplayRigDXR::perspectiveFactor moved");
static_assert(sizeof(XrCameraRigDXR) == 64, "XrCameraRigDXR size changed");
static_assert(offsetof(XrCameraRigDXR, type) == 0, "XrCameraRigDXR::type moved");
static_assert(offsetof(XrCameraRigDXR, next) == 8, "XrCameraRigDXR::next moved");
static_assert(offsetof(XrCameraRigDXR, pose) == 16, "XrCameraRigDXR::pose moved");
static_assert(offsetof(XrCameraRigDXR, ipdFactor) == 44, "XrCameraRigDXR::ipdFactor moved");
static_assert(offsetof(XrCameraRigDXR, parallaxFactor) == 48, "XrCameraRigDXR::parallaxFactor moved");
static_assert(offsetof(XrCameraRigDXR, convergenceDiopters) == 52, "XrCameraRigDXR::convergenceDiopters moved");
static_assert(offsetof(XrCameraRigDXR, verticalFov) == 56, "XrCameraRigDXR::verticalFov moved");
static_assert(offsetof(XrCameraRigDXR, metersToVirtual) == 60, "XrCameraRigDXR::metersToVirtual moved");
static_assert(sizeof(XrViewDisplayRawDXR) == 184, "XrViewDisplayRawDXR size changed");
static_assert(offsetof(XrViewDisplayRawDXR, type) == 0, "XrViewDisplayRawDXR::type moved");
static_assert(offsetof(XrViewDisplayRawDXR, next) == 8, "XrViewDisplayRawDXR::next moved");
static_assert(offsetof(XrViewDisplayRawDXR, rawEyes) == 16, "XrViewDisplayRawDXR::rawEyes moved");
static_assert(offsetof(XrViewDisplayRawDXR, eyeCountOutput) == 112, "XrViewDisplayRawDXR::eyeCountOutput moved");
static_assert(offsetof(XrViewDisplayRawDXR, displayPlanePose) == 116, "XrViewDisplayRawDXR::displayPlanePose moved");
static_assert(offsetof(XrViewDisplayRawDXR, canvasRectPx) == 144, "XrViewDisplayRawDXR::canvasRectPx moved");
static_assert(offsetof(XrViewDisplayRawDXR, canvasSizeMeters) == 160, "XrViewDisplayRawDXR::canvasSizeMeters moved");
static_assert(offsetof(XrViewDisplayRawDXR, sampleTimeNs) == 168, "XrViewDisplayRawDXR::sampleTimeNs moved");
static_assert(offsetof(XrViewDisplayRawDXR, isTracking) == 176, "XrViewDisplayRawDXR::isTracking moved");
#endif

// ---- XR_DXR_local_3d_zone.h
static_assert(XR_DXR_local_3d_zone_SPEC_VERSION == 5, "XR_DXR_local_3d_zone_SPEC_VERSION moved");
static_assert(XR_TYPE_LOCAL_3D_ZONE_CAPABILITIES_DXR == (XrStructureType)1004999160, "XR_TYPE_LOCAL_3D_ZONE_CAPABILITIES_DXR renumbered");
static_assert(XR_TYPE_LOCAL_3D_ZONE_MASK_CREATE_INFO_DXR == (XrStructureType)1004999161, "XR_TYPE_LOCAL_3D_ZONE_MASK_CREATE_INFO_DXR renumbered");
static_assert(XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_D3D11_DXR == (XrStructureType)1004999162, "XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_D3D11_DXR renumbered");
static_assert(XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_D3D12_DXR == (XrStructureType)1004999163, "XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_D3D12_DXR renumbered");
static_assert(XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_VULKAN_DXR == (XrStructureType)1004999164, "XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_VULKAN_DXR renumbered");
static_assert(XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_DXR == (XrStructureType)1004999165, "XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_DXR renumbered");
static_assert(XR_TYPE_EVENT_DATA_LOCAL_3D_ZONE_VIEW_SIZE_CHANGED_DXR == (XrStructureType)1004999166, "XR_TYPE_EVENT_DATA_LOCAL_3D_ZONE_VIEW_SIZE_CHANGED_DXR renumbered");
#if UINTPTR_MAX == UINT64_MAX
static_assert(sizeof(XrLocal3DZoneCapabilitiesDXR) == 40, "XrLocal3DZoneCapabilitiesDXR size changed");
static_assert(offsetof(XrLocal3DZoneCapabilitiesDXR, type) == 0, "XrLocal3DZoneCapabilitiesDXR::type moved");
static_assert(offsetof(XrLocal3DZoneCapabilitiesDXR, next) == 8, "XrLocal3DZoneCapabilitiesDXR::next moved");
static_assert(offsetof(XrLocal3DZoneCapabilitiesDXR, supported) == 16, "XrLocal3DZoneCapabilitiesDXR::supported moved");
static_assert(offsetof(XrLocal3DZoneCapabilitiesDXR, hardwareZoneGridWidth) == 20, "XrLocal3DZoneCapabilitiesDXR::hardwareZoneGridWidth moved");
static_assert(offsetof(XrLocal3DZoneCapabilitiesDXR, hardwareZoneGridHeight) == 24, "XrLocal3DZoneCapabilitiesDXR::hardwareZoneGridHeight moved");
static_assert(offsetof(XrLocal3DZoneCapabilitiesDXR, maxMaskWidth) == 28, "XrLocal3DZoneCapabilitiesDXR::maxMaskWidth moved");
static_assert(offsetof(XrLocal3DZoneCapabilitiesDXR, maxMaskHeight) == 32, "XrLocal3DZoneCapabilitiesDXR::maxMaskHeight moved");
static_assert(sizeof(XrLocal3DZoneMaskCreateInfoDXR) == 24, "XrLocal3DZoneMaskCreateInfoDXR size changed");
static_assert(offsetof(XrLocal3DZoneMaskCreateInfoDXR, type) == 0, "XrLocal3DZoneMaskCreateInfoDXR::type moved");
static_assert(offsetof(XrLocal3DZoneMaskCreateInfoDXR, next) == 8, "XrLocal3DZoneMaskCreateInfoDXR::next moved");
static_assert(offsetof(XrLocal3DZoneMaskCreateInfoDXR, maskWidth) == 16, "XrLocal3DZoneMaskCreateInfoDXR::maskWidth moved");
static_assert(offsetof(XrLocal3DZoneMaskCreateInfoDXR, maskHeight) == 20, "XrLocal3DZoneMaskCreateInfoDXR::maskHeight moved");
static_assert(sizeof(XrLocal3DZoneRenderTargetD3D11DXR) == 32, "XrLocal3DZoneRenderTargetD3D11DXR size changed");
static_assert(offsetof(XrLocal3DZoneRenderTargetD3D11DXR, type) == 0, "XrLocal3DZoneRenderTargetD3D11DXR::type moved");
static_assert(offsetof(XrLocal3DZoneRenderTargetD3D11DXR, next) == 8, "XrLocal3DZoneRenderTargetD3D11DXR::next moved");
static_assert(offsetof(XrLocal3DZoneRenderTargetD3D11DXR, renderTargetView) == 16, "XrLocal3DZoneRenderTargetD3D11DXR::renderTargetView moved");
static_assert(offsetof(XrLocal3DZoneRenderTargetD3D11DXR, width) == 24, "XrLocal3DZoneRenderTargetD3D11DXR::width moved");
static_assert(offsetof(XrLocal3DZoneRenderTargetD3D11DXR, height) == 28, "XrLocal3DZoneRenderTargetD3D11DXR::height moved");
static_assert(sizeof(XrLocal3DZoneRenderTargetD3D12DXR) == 32, "XrLocal3DZoneRenderTargetD3D12DXR size changed");
static_assert(offsetof(XrLocal3DZoneRenderTargetD3D12DXR, type) == 0, "XrLocal3DZoneRenderTargetD3D12DXR::type moved");
static_assert(offsetof(XrLocal3DZoneRenderTargetD3D12DXR, next) == 8, "XrLocal3DZoneRenderTargetD3D12DXR::next moved");
static_assert(offsetof(XrLocal3DZoneRenderTargetD3D12DXR, resource) == 16, "XrLocal3DZoneRenderTargetD3D12DXR::resource moved");
static_assert(offsetof(XrLocal3DZoneRenderTargetD3D12DXR, width) == 24, "XrLocal3DZoneRenderTargetD3D12DXR::width moved");
static_assert(offsetof(XrLocal3DZoneRenderTargetD3D12DXR, height) == 28, "XrLocal3DZoneRenderTargetD3D12DXR::height moved");
static_assert(sizeof(XrLocal3DZoneRenderTargetVulkanDXR) == 40, "XrLocal3DZoneRenderTargetVulkanDXR size changed");
static_assert(offsetof(XrLocal3DZoneRenderTargetVulkanDXR, type) == 0, "XrLocal3DZoneRenderTargetVulkanDXR::type moved");
static_assert(offsetof(XrLocal3DZoneRenderTargetVulkanDXR, next) == 8, "XrLocal3DZoneRenderTargetVulkanDXR::next moved");
static_assert(offsetof(XrLocal3DZoneRenderTargetVulkanDXR, image) == 16, "XrLocal3DZoneRenderTargetVulkanDXR::image moved");
static_assert(offsetof(XrLocal3DZoneRenderTargetVulkanDXR, imageView) == 24, "XrLocal3DZoneRenderTargetVulkanDXR::imageView moved");
static_assert(offsetof(XrLocal3DZoneRenderTargetVulkanDXR, width) == 32, "XrLocal3DZoneRenderTargetVulkanDXR::width moved");
static_assert(offsetof(XrLocal3DZoneRenderTargetVulkanDXR, height) == 36, "XrLocal3DZoneRenderTargetVulkanDXR::height moved");
static_assert(sizeof(XrCompositionLayerLocal2DDXR) == 72, "XrCompositionLayerLocal2DDXR size changed");
static_assert(offsetof(XrCompositionLayerLocal2DDXR, type) == 0, "XrCompositionLayerLocal2DDXR::type moved");
static_assert(offsetof(XrCompositionLayerLocal2DDXR, next) == 8, "XrCompositionLayerLocal2DDXR::next moved");
static_assert(offsetof(XrCompositionLayerLocal2DDXR, layerFlags) == 16, "XrCompositionLayerLocal2DDXR::layerFlags moved");
static_assert(offsetof(XrCompositionLayerLocal2DDXR, subImage) == 24, "XrCompositionLayerLocal2DDXR::subImage moved");
static_assert(offsetof(XrCompositionLayerLocal2DDXR, rect) == 56, "XrCompositionLayerLocal2DDXR::rect moved");
static_assert(sizeof(XrEventDataLocal3DZoneViewSizeChangedDXR) == 32, "XrEventDataLocal3DZoneViewSizeChangedDXR size changed");
static_assert(offsetof(XrEventDataLocal3DZoneViewSizeChangedDXR, type) == 0, "XrEventDataLocal3DZoneViewSizeChangedDXR::type moved");
static_assert(offsetof(XrEventDataLocal3DZoneViewSizeChangedDXR, next) == 8, "XrEventDataLocal3DZoneViewSizeChangedDXR::next moved");
static_assert(offsetof(XrEventDataLocal3DZoneViewSizeChangedDXR, session) == 16, "XrEventDataLocal3DZoneViewSizeChangedDXR::session moved");
static_assert(offsetof(XrEventDataLocal3DZoneViewSizeChangedDXR, recommendedImageRectWidth) == 24, "XrEventDataLocal3DZoneViewSizeChangedDXR::recommendedImageRectWidth moved");
static_assert(offsetof(XrEventDataLocal3DZoneViewSizeChangedDXR, recommendedImageRectHeight) == 28, "XrEventDataLocal3DZoneViewSizeChangedDXR::recommendedImageRectHeight moved");
#endif

// ---- XR_DXR_display_zones.h
static_assert(XR_DXR_display_zones_SPEC_VERSION == 3, "XR_DXR_display_zones_SPEC_VERSION moved");
static_assert(XR_TYPE_DISPLAY_ZONE_CAPABILITIES_DXR == (XrStructureType)1004999150, "XR_TYPE_DISPLAY_ZONE_CAPABILITIES_DXR renumbered");
static_assert(XR_TYPE_DISPLAY_ZONE_DXR == (XrStructureType)1004999151, "XR_TYPE_DISPLAY_ZONE_DXR renumbered");
static_assert(XR_TYPE_DISPLAY_ZONES_FRAME_END_INFO_DXR == (XrStructureType)1004999152, "XR_TYPE_DISPLAY_ZONES_FRAME_END_INFO_DXR renumbered");
static_assert(XR_TYPE_EVENT_DATA_DISPLAY_ZONE_METRICS_CHANGED_DXR == (XrStructureType)1004999153, "XR_TYPE_EVENT_DATA_DISPLAY_ZONE_METRICS_CHANGED_DXR renumbered");
static_assert(XR_TYPE_DISPLAY_ZONE_FEATHER_DXR == (XrStructureType)1004999154, "XR_TYPE_DISPLAY_ZONE_FEATHER_DXR renumbered");
#if UINTPTR_MAX == UINT64_MAX
static_assert(sizeof(XrDisplayZoneCapabilitiesDXR) == 24, "XrDisplayZoneCapabilitiesDXR size changed");
static_assert(offsetof(XrDisplayZoneCapabilitiesDXR, type) == 0, "XrDisplayZoneCapabilitiesDXR::type moved");
static_assert(offsetof(XrDisplayZoneCapabilitiesDXR, next) == 8, "XrDisplayZoneCapabilitiesDXR::next moved");
static_assert(offsetof(XrDisplayZoneCapabilitiesDXR, supported) == 16, "XrDisplayZoneCapabilitiesDXR::supported moved");
static_assert(offsetof(XrDisplayZoneCapabilitiesDXR, maxZones3D) == 20, "XrDisplayZoneCapabilitiesDXR::maxZones3D moved");
static_assert(sizeof(XrDisplayZoneDXR) == 40, "XrDisplayZoneDXR size changed");
static_assert(offsetof(XrDisplayZoneDXR, type) == 0, "XrDisplayZoneDXR::type moved");
static_assert(offsetof(XrDisplayZoneDXR, next) == 8, "XrDisplayZoneDXR::next moved");
static_assert(offsetof(XrDisplayZoneDXR, zoneId) == 16, "XrDisplayZoneDXR::zoneId moved");
static_assert(offsetof(XrDisplayZoneDXR, rect) == 20, "XrDisplayZoneDXR::rect moved");
static_assert(sizeof(XrDisplayZoneFeatherDXR) == 24, "XrDisplayZoneFeatherDXR size changed");
static_assert(offsetof(XrDisplayZoneFeatherDXR, type) == 0, "XrDisplayZoneFeatherDXR::type moved");
static_assert(offsetof(XrDisplayZoneFeatherDXR, next) == 8, "XrDisplayZoneFeatherDXR::next moved");
static_assert(offsetof(XrDisplayZoneFeatherDXR, radiusPx) == 16, "XrDisplayZoneFeatherDXR::radiusPx moved");
static_assert(sizeof(XrDisplayZonesFrameEndInfoDXR) == 32, "XrDisplayZonesFrameEndInfoDXR size changed");
static_assert(offsetof(XrDisplayZonesFrameEndInfoDXR, type) == 0, "XrDisplayZonesFrameEndInfoDXR::type moved");
static_assert(offsetof(XrDisplayZonesFrameEndInfoDXR, next) == 8, "XrDisplayZonesFrameEndInfoDXR::next moved");
static_assert(offsetof(XrDisplayZonesFrameEndInfoDXR, flags) == 16, "XrDisplayZonesFrameEndInfoDXR::flags moved");
static_assert(offsetof(XrDisplayZonesFrameEndInfoDXR, wishMask) == 24, "XrDisplayZonesFrameEndInfoDXR::wishMask moved");
static_assert(sizeof(XrEventDataDisplayZoneMetricsChangedDXR) == 24, "XrEventDataDisplayZoneMetricsChangedDXR size changed");
static_assert(offsetof(XrEventDataDisplayZoneMetricsChangedDXR, type) == 0, "XrEventDataDisplayZoneMetricsChangedDXR::type moved");
static_assert(offsetof(XrEventDataDisplayZoneMetricsChangedDXR, next) == 8, "XrEventDataDisplayZoneMetricsChangedDXR::next moved");
static_assert(offsetof(XrEventDataDisplayZoneMetricsChangedDXR, session) == 16, "XrEventDataDisplayZoneMetricsChangedDXR::session moved");
#endif

TEST_CASE("XR_DXR_* ABI snapshot compiles", "[abi][extensions]")
{
	// The pin is the static_asserts above; reaching here means it held.
	SUCCEED();
}
