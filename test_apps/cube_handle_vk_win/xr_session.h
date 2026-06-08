// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management for Vulkan with XR_EXT_win32_window_binding
 */

#pragma once

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include "xr_session_common.h"
#include <openxr/XR_EXT_view_rig.h>
#include <openxr/XR_EXT_local_3d_zone.h>

// XR_EXT_view_rig (#396 W7) available + enabled on the instance. App-local
// (not on the shared XrSessionManager); promote to xr_session_common when
// more consumers adopt it.
extern bool g_hasViewRigExt;

// #439 Phase 3 — XR_EXT_local_3d_zone harness (header v3 carries the Local2D
// composition-layer + view-size-changed types). App-local for the same reason
// as the view-rig flag; populated by InitializeOpenXR, drives the VK handle-app
// panel modes (DXR_LOCAL2D_PANEL / +DXR_LOCAL2D_MASK / +DXR_LOCAL2D_PANEL2).
struct ZoneMaskHarness {
    bool available = false;
    PFN_xrCreateLocal3DZoneMaskEXT pfnCreate = nullptr;
    PFN_xrSetLocal3DZoneFromRectsEXT pfnSetRects = nullptr;
    PFN_xrSubmitLocal3DZoneEXT pfnSubmit = nullptr;
    PFN_xrDestroyLocal3DZoneMaskEXT pfnDestroy = nullptr;
    XrLocal3DZoneMaskEXT mask = XR_NULL_HANDLE;
};
extern ZoneMaskHarness g_zone;

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
