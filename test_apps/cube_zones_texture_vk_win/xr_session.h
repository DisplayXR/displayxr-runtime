// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management for the XR_EXT_display_zones TEXTURE
 *         exerciser — VULKAN leg (hybrid).
 *
 * Cloned from cube_zones_texture_d3d11_win and converted to a VK/D3D11 HYBRID:
 *  - The OpenXR graphics binding is VULKAN (XrGraphicsBindingVulkanKHR) — the
 *    zone cubes render into Vulkan OpenXR swapchains.
 *  - The shared composite target the runtime writes back into is still a
 *    D3D11 KMT texture (BGRA): the runtime's VK native compositor IMPORTS that
 *    D3D11 KMT handle as VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT.
 *    The app passes the D3D11 texture HANDLE + the app HWND via
 *    XR_EXT_win32_window_binding (the texture-mode marker), exactly like the
 *    D3D11 leg.
 *
 * ADAPTER-MATCH (the novel part): the runtime dictates the VkPhysicalDevice via
 * xrGetVulkanGraphicsDeviceKHR; the D3D11 device for the shared texture MUST be
 * created on the SAME GPU (matching DXGI adapter LUID) or the KMT import fails.
 * GetVulkanPhysicalDeviceLUID() reads VkPhysicalDeviceIDProperties.deviceLUID
 * (chained into VkPhysicalDeviceProperties2) so main.cpp can pick the matching
 * DXGI adapter before creating the shared texture.
 *
 * The display-zones detection + entry points are unchanged from the D3D11 leg
 * (XR_EXT_display_zones composes XR_EXT_local_3d_zone + XR_EXT_view_rig).
 */

#pragma once

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <d3d11.h>
#include "xr_session_common.h"
#include <openxr/XR_EXT_local_3d_zone.h>
#include <openxr/XR_EXT_display_zones.h>
#include <openxr/XR_EXT_view_rig.h>

#include <string>
#include <vector>

// XR_EXT_view_rig available + enabled on the instance.
extern bool g_hasViewRigExt;

// XR_EXT_local_3d_zone harness (mask handle + entry points). The zones app
// uses the mask as the per-frame wish referenced from the xrEndFrame chain
// (XrDisplayZonesFrameEndInfoEXT) — NOT via the sticky xrSubmitLocal3DZoneEXT
// channel, which is inert in zones frames. pfnAcquire is the Tier-3 freeform
// render-target entry (optional; wish mode 2 is skipped when unresolved).
struct ZoneMaskHarness {
    bool available = false;
    PFN_xrCreateLocal3DZoneMaskEXT pfnCreate = nullptr;
    PFN_xrSetLocal3DZoneFromRectsEXT pfnSetRects = nullptr;
    PFN_xrAcquireLocal3DZoneRenderTargetEXT pfnAcquire = nullptr;
    PFN_xrSubmitLocal3DZoneEXT pfnSubmit = nullptr;
    PFN_xrDestroyLocal3DZoneMaskEXT pfnDestroy = nullptr;
    XrLocal3DZoneMaskEXT mask = XR_NULL_HANDLE;
};
extern ZoneMaskHarness g_zone;

// XR_EXT_display_zones (ADR-027) available + enabled on the instance. Only
// true when local_3d_zone + view_rig were also enabled (the extension
// requires both). The runtime advertises it under the DISPLAYXR_ZONES=1 dev
// gate (P2) — when absent the app logs an error once and runs the plain
// single-projection fallback.
extern bool g_hasDisplayZonesExt;

struct DisplayZonesHarness {
    PFN_xrGetDisplayZoneCapabilitiesEXT pfnGetCaps = nullptr;
    PFN_xrGetDisplayZoneRecommendedViewSizeEXT pfnGetViewSize = nullptr;
};
extern DisplayZonesHarness g_zones;

// Initialize OpenXR instance and detect/enable extensions (Vulkan + zones).
bool InitializeOpenXR(XrSessionManager& xr);

// Get Vulkan graphics requirements (min/max API version) per the OpenXR spec.
bool GetVulkanGraphicsRequirements(XrSessionManager& xr);

// Create Vulkan instance with the OpenXR-required instance extensions.
bool CreateVulkanInstance(XrSessionManager& xr, VkInstance& vkInstance);

// Get the physical device the runtime selected (dictates the GPU for XR).
bool GetVulkanPhysicalDevice(XrSessionManager& xr, VkInstance vkInstance, VkPhysicalDevice& physDevice);

// ADAPTER-MATCH: read the runtime's VkPhysicalDevice DXGI adapter LUID via
// VkPhysicalDeviceIDProperties.deviceLUID (chained into Properties2). main.cpp
// creates the D3D11 shared texture on the DXGI adapter whose AdapterLuid
// matches this — required for the KMT import to succeed. Returns false if the
// device doesn't report a valid LUID (deviceLUIDValid == VK_FALSE).
bool GetVulkanPhysicalDeviceLUID(VkPhysicalDevice physDevice, LUID& outLuid);

// Get required device extensions from the runtime (filtered to those the
// device actually exposes — promoted-to-core ones are skipped).
bool GetVulkanDeviceExtensions(XrSessionManager& xr, VkInstance vkInstance, VkPhysicalDevice physDevice,
    std::vector<const char*>& deviceExtensions, std::vector<std::string>& extensionStorage);

// Find a graphics queue family.
bool FindGraphicsQueueFamily(VkPhysicalDevice physDevice, uint32_t& queueFamilyIndex);

// Create Vulkan logical device + grab the graphics queue.
bool CreateVulkanDevice(VkPhysicalDevice physDevice, uint32_t queueFamilyIndex,
    const std::vector<const char*>& deviceExtensions,
    VkDevice& device, VkQueue& graphicsQueue);

// Create the OpenXR session with XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR chained to
// XrWin32WindowBindingCreateInfoEXT carrying the app's D3D11 shared-texture
// HANDLE (the runtime's composite target — the texture-mode marker) + the app
// HWND (weaver position tracking) + transparentBackgroundEnabled.
bool CreateSession(XrSessionManager& xr, VkInstance vkInstance, VkPhysicalDevice physDevice,
    VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex,
    HANDLE sharedTextureHandle, HWND appHwnd);
