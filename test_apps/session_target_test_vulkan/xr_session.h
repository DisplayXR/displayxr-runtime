// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management for XR_EXT_session_target testing (Vulkan)
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <Unknwn.h>  // For IUnknown, needed by OpenXR MSFT extensions

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

// Must define graphics API before including OpenXR platform header
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_EXT_session_target.h>

#include "vulkan_renderer.h"
#include <vector>
#include <string>

struct SwapchainInfo {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    int64_t format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<XrSwapchainImageVulkanKHR> images;
};

struct XrSessionManager {
    // OpenXR handles
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    XrSpace viewSpace = XR_NULL_HANDLE;

    // Swapchains (one per eye for stereo)
    SwapchainInfo swapchains[2];

    // View configuration
    XrViewConfigurationType viewConfigType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    std::vector<XrViewConfigurationView> configViews;

    // Session state
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning = false;
    bool exitRequested = false;

    // Extension support
    bool hasSessionTargetExt = false;
    bool hasVulkanExt = false;

    // Window handle for session target
    HWND windowHandle = nullptr;

    // Eye tracking data (from views)
    float eyePosX = 0.0f;
    float eyePosY = 0.0f;
    bool eyeTrackingActive = false;

    // Frame timing
    XrTime predictedDisplayTime = 0;
    XrDuration predictedDisplayPeriod = 0;

    // Function pointers for Vulkan extension
    PFN_xrGetVulkanInstanceExtensionsKHR xrGetVulkanInstanceExtensionsKHR = nullptr;
    PFN_xrGetVulkanDeviceExtensionsKHR xrGetVulkanDeviceExtensionsKHR = nullptr;
    PFN_xrGetVulkanGraphicsDeviceKHR xrGetVulkanGraphicsDeviceKHR = nullptr;
    PFN_xrGetVulkanGraphicsRequirementsKHR xrGetVulkanGraphicsRequirementsKHR = nullptr;
};

// Get required Vulkan instance extensions for OpenXR
// Call this BEFORE creating Vulkan instance
bool GetVulkanInstanceExtensions(XrSessionManager& xr, std::vector<const char*>& extensions);

// Initialize OpenXR instance and check for extensions
// Must be called AFTER Vulkan instance is created
bool InitializeOpenXR(XrSessionManager& xr, VkInstance vkInstance);

// Get the physical device that OpenXR wants to use
bool GetVulkanPhysicalDevice(XrSessionManager& xr, VkInstance vkInstance, VkPhysicalDevice& physicalDevice);

// Get required Vulkan device extensions for OpenXR
bool GetVulkanDeviceExtensions(XrSessionManager& xr, VkInstance vkInstance, VkPhysicalDevice physicalDevice, std::vector<const char*>& extensions);

// Create session with Vulkan device and window handle (using XR_EXT_session_target)
bool CreateSession(
    XrSessionManager& xr,
    VkInstance vkInstance,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    uint32_t queueFamilyIndex,
    uint32_t queueIndex,
    HWND hwnd
);

// Create reference spaces
bool CreateSpaces(XrSessionManager& xr);

// Create swapchains for rendering
bool CreateSwapchains(XrSessionManager& xr);

// Get the Vulkan images from swapchains
// Call this after CreateSwapchains
std::vector<VkImage> GetSwapchainImages(XrSessionManager& xr, int eye);

// Poll for OpenXR events and update session state
bool PollEvents(XrSessionManager& xr);

// Begin a frame - returns true if should render
bool BeginFrame(XrSessionManager& xr, XrFrameState& frameState);

// Get view poses for rendering
bool LocateViews(
    XrSessionManager& xr,
    XrTime displayTime,
    Mat4& leftViewMatrix,
    Mat4& leftProjMatrix,
    Mat4& rightViewMatrix,
    Mat4& rightProjMatrix
);

// Acquire swapchain image for rendering
bool AcquireSwapchainImage(XrSessionManager& xr, int eye, uint32_t& imageIndex);

// Release swapchain image after rendering
bool ReleaseSwapchainImage(XrSessionManager& xr, int eye);

// End frame and submit layers
bool EndFrame(XrSessionManager& xr, XrTime displayTime, const XrCompositionLayerProjectionView* views);

// Clean up OpenXR resources
void CleanupOpenXR(XrSessionManager& xr);

// Get session state as string for display
const char* GetSessionStateString(XrSessionState state);

// Request session exit
void RequestExit(XrSessionManager& xr);
