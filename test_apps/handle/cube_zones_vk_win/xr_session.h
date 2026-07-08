// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management for the XR_EXT_display_zones exerciser —
 *         native Vulkan HANDLE leg (array / single-pass-instanced stereo).
 *
 * Cloned from cube_handle_vk_win (a working Vulkan HANDLE app: the runtime owns
 * presentation via XR_EXT_win32_window_binding + the VK native compositor) and
 * extended with XR_EXT_display_zones (ADR-027) detection + entry points,
 * mirroring the D3D12 handle-class zones reference cube_zones_d3d12_win. The
 * zones extension composes XR_EXT_local_3d_zone (the wish-mask tiers) and
 * XR_EXT_view_rig (per-zone framing), so it is enabled only when both
 * prerequisites are also available.
 */

#pragma once

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include "xr_session_common.h"
#include <openxr/XR_EXT_view_rig.h>
#include <openxr/XR_EXT_local_3d_zone.h>
#include <openxr/XR_EXT_display_zones.h>

// INV-1.3 (#715): 3D panel top-left in virtual-desktop pixels (top-down,
// origin = primary top-left); (0,0) = primary/unknown. Filled by
// InitializeOpenXR from the XrDisplayDesktopPositionEXT chain (spec v16).
extern int32_t g_displayScreenLeft;
extern int32_t g_displayScreenTop;

// XR_EXT_view_rig (#396 W7) available + enabled on the instance. App-local
// (not on the shared XrSessionManager); promote to xr_session_common when
// more consumers adopt it.
extern bool g_hasViewRigExt;

// XR_EXT_local_3d_zone harness (mask handle + entry points). Prerequisite for
// XR_EXT_display_zones; the zones path here uses AUTO wish (wishMask = NULL,
// runtime auto-derives), so the mask entry points are resolved but the mask is
// left unused — enabling the extension is what the runtime gates on.
struct ZoneMaskHarness {
    bool available = false;
    PFN_xrCreateLocal3DZoneMaskEXT pfnCreate = nullptr;
    PFN_xrSetLocal3DZoneFromRectsEXT pfnSetRects = nullptr;
    PFN_xrSubmitLocal3DZoneEXT pfnSubmit = nullptr;
    PFN_xrDestroyLocal3DZoneMaskEXT pfnDestroy = nullptr;
    XrLocal3DZoneMaskEXT mask = XR_NULL_HANDLE;
};
extern ZoneMaskHarness g_zone;

// XR_EXT_display_zones (ADR-027) available + enabled on the instance. Only true
// when local_3d_zone + view_rig were also enabled (the extension requires
// both). The runtime advertises it under the DISPLAYXR_ZONES=1 dev gate (P2) —
// when absent the app logs an error once and submits empty frames.
extern bool g_hasDisplayZonesExt;

struct DisplayZonesHarness {
    PFN_xrGetDisplayZoneCapabilitiesEXT pfnGetCaps = nullptr;
    PFN_xrGetDisplayZoneRecommendedViewSizeEXT pfnGetViewSize = nullptr;
};
extern DisplayZonesHarness g_zones;

// Initialize OpenXR instance with Vulkan + win32_window_binding extensions
bool InitializeOpenXR(XrSessionManager& xr);

// Get Vulkan graphics requirements and set up Vulkan instance/device per OpenXR spec
bool GetVulkanGraphicsRequirements(XrSessionManager& xr);

// Create Vulkan instance with required extensions from the runtime
bool CreateVulkanInstance(XrSessionManager& xr, VkInstance& vkInstance);

// Get the physical device selected by the runtime
bool GetVulkanPhysicalDevice(XrSessionManager& xr, VkInstance vkInstance, VkPhysicalDevice& physDevice);

// Get required device extensions from the runtime
bool GetVulkanDeviceExtensions(XrSessionManager& xr, VkInstance vkInstance, VkPhysicalDevice physDevice,
    std::vector<const char*>& deviceExtensions, std::vector<std::string>& extensionStorage);

// Find a graphics queue family
bool FindGraphicsQueueFamily(VkPhysicalDevice physDevice, uint32_t& queueFamilyIndex);

// Create Vulkan logical device with required extensions
bool CreateVulkanDevice(VkPhysicalDevice physDevice, uint32_t queueFamilyIndex,
    const std::vector<const char*>& deviceExtensions,
    VkDevice& device, VkQueue& graphicsQueue);

// Create OpenXR session with Vulkan binding + win32_window_binding
bool CreateSession(XrSessionManager& xr, VkInstance vkInstance, VkPhysicalDevice physDevice,
    VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, HWND hwnd);
