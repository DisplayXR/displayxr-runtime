// Copyright 2018-2024, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header defining all API functions.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup oxr_api
 */

#pragma once

#include "oxr_extension_support.h"

#ifdef __cplusplus
extern "C" {
#endif


/*!
 * @defgroup oxr_api OpenXR entrypoints
 *
 * Gets called from the client application, does most verification and routes
 * calls into @ref oxr_main functions.
 *
 * @ingroup oxr
 * @{
 */


/*
 *
 * oxr_api_negotiate.c
 *
 */

//! OpenXR API function @ep{xrGetInstanceProcAddr}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetInstanceProcAddr(XrInstance instance, const char *name, PFN_xrVoidFunction *function);

//! OpenXR API function @ep{xrEnumerateApiLayerProperties}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrEnumerateApiLayerProperties(uint32_t propertyCapacityInput,
                                  uint32_t *propertyCountOutput,
                                  XrApiLayerProperties *properties);


/*
 *
 * oxr_api_instance.c
 *
 */

#ifdef OXR_HAVE_KHR_loader_init
//! OpenXR API function @ep{xrInitializeLoaderKHR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrInitializeLoaderKHR(const XrLoaderInitInfoBaseHeaderKHR *loaderInitInfo);
#endif // OXR_HAVE_KHR_loader_init

//! OpenXR API function @ep{xrEnumerateInstanceExtensionProperties}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrEnumerateInstanceExtensionProperties(const char *layerName,
                                           uint32_t propertyCapacityInput,
                                           uint32_t *propertyCountOutput,
                                           XrExtensionProperties *properties);

//! OpenXR API function @ep{xrCreateInstance}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCreateInstance(const XrInstanceCreateInfo *createInfo, XrInstance *instance);

//! OpenXR API function @ep{xrDestroyInstance}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrDestroyInstance(XrInstance instance);

//! OpenXR API function @ep{xrGetInstanceProperties}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetInstanceProperties(XrInstance instance, XrInstanceProperties *instanceProperties);

//! OpenXR API function @ep{xrPollEvent}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrPollEvent(XrInstance instance, XrEventDataBuffer *eventData);

//! OpenXR API function @ep{xrResultToString}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrResultToString(XrInstance instance, XrResult value, char buffer[XR_MAX_RESULT_STRING_SIZE]);

//! OpenXR API function @ep{xrStructureTypeToString}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrStructureTypeToString(XrInstance instance, XrStructureType value, char buffer[XR_MAX_STRUCTURE_NAME_SIZE]);

//! OpenXR API function @ep{xrStringToPath}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrStringToPath(XrInstance instance, const char *pathString, XrPath *path);

//! OpenXR API function @ep{xrPathToString}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrPathToString(
    XrInstance instance, XrPath path, uint32_t bufferCapacityInput, uint32_t *bufferCountOutput, char *buffer);

//! OpenXR API function @ep{xrConvertTimespecTimeToTimeKHR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrConvertTimespecTimeToTimeKHR(XrInstance instance, const struct timespec *timespecTime, XrTime *time);

//! OpenXR API function @ep{xrConvertTimeToTimespecTimeKHR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrConvertTimeToTimespecTimeKHR(XrInstance instance, XrTime time, struct timespec *timespecTime);

#ifdef XR_USE_PLATFORM_WIN32
//! OpenXR API function @ep{xrConvertWin32PerformanceCounterToTimeKHR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrConvertWin32PerformanceCounterToTimeKHR(XrInstance instance,
                                              const LARGE_INTEGER *performanceCounter,
                                              XrTime *time);

//! OpenXR API function @ep{xrConvertTimeToWin32PerformanceCounterKHR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrConvertTimeToWin32PerformanceCounterKHR(XrInstance instance, XrTime time, LARGE_INTEGER *performanceCounter);
#endif // XR_USE_PLATFORM_WIN32

#ifdef OXR_HAVE_KHR_extended_struct_name_lengths
//! OpenXR API function @ep{xrStructureTypeToString2KHR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrStructureTypeToString2KHR(XrInstance instance,
                                XrStructureType value,
                                char buffer[XR_MAX_STRUCTURE_NAME_SIZE_EXTENDED_KHR]);
#endif // OXR_HAVE_KHR_extended_struct_name_lengths

/*
 *
 * oxr_api_system.c
 *
 */

//! OpenXR API function @ep{xrGetSystem}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetSystem(XrInstance instance, const XrSystemGetInfo *getInfo, XrSystemId *systemId);

//! OpenXR API function @ep{xrGetSystemProperties}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetSystemProperties(XrInstance instance, XrSystemId systemId, XrSystemProperties *properties);

//! OpenXR API function @ep{xrEnumerateViewConfigurations}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrEnumerateViewConfigurations(XrInstance instance,
                                  XrSystemId systemId,
                                  uint32_t viewConfigurationTypeCapacityInput,
                                  uint32_t *viewConfigurationTypeCountOutput,
                                  XrViewConfigurationType *viewConfigurationTypes);

//! OpenXR API function @ep{xrGetViewConfigurationProperties}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetViewConfigurationProperties(XrInstance instance,
                                     XrSystemId systemId,
                                     XrViewConfigurationType viewConfigurationType,
                                     XrViewConfigurationProperties *configurationProperties);

//! OpenXR API function @ep{xrEnumerateViewConfigurationViews}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrEnumerateViewConfigurationViews(XrInstance instance,
                                      XrSystemId systemId,
                                      XrViewConfigurationType viewConfigurationType,
                                      uint32_t viewCapacityInput,
                                      uint32_t *viewCountOutput,
                                      XrViewConfigurationView *views);

//! OpenXR API function @ep{xrEnumerateEnvironmentBlendModes}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrEnumerateEnvironmentBlendModes(XrInstance instance,
                                     XrSystemId systemId,
                                     XrViewConfigurationType viewConfigurationType,
                                     uint32_t environmentBlendModeCapacityInput,
                                     uint32_t *environmentBlendModeCountOutput,
                                     XrEnvironmentBlendMode *environmentBlendModes);

#ifdef XR_USE_GRAPHICS_API_OPENGL
//! OpenXR API function @ep{xrGetOpenGLGraphicsRequirementsKHR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetOpenGLGraphicsRequirementsKHR(XrInstance instance,
                                       XrSystemId systemId,
                                       XrGraphicsRequirementsOpenGLKHR *graphicsRequirements);
#endif

#ifdef XR_USE_GRAPHICS_API_OPENGL_ES
//! OpenXR API function @ep{xrGetOpenGLESGraphicsRequirementsKHR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetOpenGLESGraphicsRequirementsKHR(XrInstance instance,
                                         XrSystemId systemId,
                                         XrGraphicsRequirementsOpenGLESKHR *graphicsRequirements);
#endif

#ifdef XR_USE_GRAPHICS_API_VULKAN
//! OpenXR API function @ep{xrGetVulkanInstanceExtensionsKHR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetVulkanInstanceExtensionsKHR(XrInstance instance,
                                     XrSystemId systemId,
                                     uint32_t namesCapacityInput,
                                     uint32_t *namesCountOutput,
                                     char *namesString);

//! OpenXR API function @ep{xrGetVulkanDeviceExtensionsKHR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetVulkanDeviceExtensionsKHR(XrInstance instance,
                                   XrSystemId systemId,
                                   uint32_t namesCapacityInput,
                                   uint32_t *namesCountOutput,
                                   char *namesString);

//! OpenXR API function @ep{xrGetVulkanGraphicsDeviceKHR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetVulkanGraphicsDeviceKHR(XrInstance instance,
                                 XrSystemId systemId,
                                 VkInstance vkInstance,
                                 VkPhysicalDevice *vkPhysicalDevice);

//! OpenXR API function @ep{xrGetVulkanGraphicsDeviceKHR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetVulkanGraphicsDevice2KHR(XrInstance instance,
                                  const XrVulkanGraphicsDeviceGetInfoKHR *getInfo,
                                  VkPhysicalDevice *vkPhysicalDevice);

//! OpenXR API function @ep{xrGetVulkanGraphicsRequirementsKHR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetVulkanGraphicsRequirementsKHR(XrInstance instance,
                                       XrSystemId systemId,
                                       XrGraphicsRequirementsVulkanKHR *graphicsRequirements);

//! OpenXR API function @ep{xrGetVulkanGraphicsRequirements2KHR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetVulkanGraphicsRequirements2KHR(XrInstance instance,
                                        XrSystemId systemId,
                                        XrGraphicsRequirementsVulkan2KHR *graphicsRequirements);

//! OpenXR API function @ep{xrCreateVulkanInstanceKHR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCreateVulkanInstanceKHR(XrInstance instance,
                              const XrVulkanInstanceCreateInfoKHR *createInfo,
                              VkInstance *vulkanInstance,
                              VkResult *vulkanResult);

//! OpenXR API function @ep{xrCreateVulkanDeviceKHR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCreateVulkanDeviceKHR(XrInstance instance,
                            const XrVulkanDeviceCreateInfoKHR *createInfo,
                            VkDevice *vulkanDevice,
                            VkResult *vulkanResult);
#endif

#ifdef XR_USE_GRAPHICS_API_D3D11

//! OpenXR API function @ep{xrGetD3D11GraphicsRequirementsKHR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetD3D11GraphicsRequirementsKHR(XrInstance instance,
                                      XrSystemId systemId,
                                      XrGraphicsRequirementsD3D11KHR *graphicsRequirements);
#endif // XR_USE_GRAPHICS_API_D3D11

#ifdef XR_USE_GRAPHICS_API_D3D12

//! OpenXR API function @ep{xrGetD3D11GraphicsRequirementsKHR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetD3D12GraphicsRequirementsKHR(XrInstance instance,
                                      XrSystemId systemId,
                                      XrGraphicsRequirementsD3D12KHR *graphicsRequirements);
#endif // XR_USE_GRAPHICS_API_D3D12

#ifdef XR_USE_GRAPHICS_API_METAL

//! OpenXR API function @ep{xrGetMetalGraphicsRequirementsKHR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetMetalGraphicsRequirementsKHR(XrInstance instance,
                                      XrSystemId systemId,
                                      XrGraphicsRequirementsMetalKHR *graphicsRequirements);
#endif // XR_USE_GRAPHICS_API_METAL

/*
 *
 * oxr_api_session.c
 *
 */

//! OpenXR API function @ep{xrCreateSession}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCreateSession(XrInstance instance, const XrSessionCreateInfo *createInfo, XrSession *session);

//! OpenXR API function @ep{xrDestroySession}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrDestroySession(XrSession session);

//! OpenXR API function @ep{xrBeginSession}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrBeginSession(XrSession session, const XrSessionBeginInfo *beginInfo);

//! OpenXR API function @ep{xrEndSession}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrEndSession(XrSession session);

//! OpenXR API function @ep{xrWaitFrame}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrWaitFrame(XrSession session, const XrFrameWaitInfo *frameWaitInfo, XrFrameState *frameState);

//! OpenXR API function @ep{xrBeginFrame}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrBeginFrame(XrSession session, const XrFrameBeginInfo *frameBeginInfo);

//! OpenXR API function @ep{xrEndFrame}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrEndFrame(XrSession session, const XrFrameEndInfo *frameEndInfo);

//! OpenXR API function @ep{xrRequestExitSession}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrRequestExitSession(XrSession session);

//! OpenXR API function @ep{xrLocateViews}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrLocateViews(XrSession session,
                  const XrViewLocateInfo *viewLocateInfo,
                  XrViewState *viewState,
                  uint32_t viewCapacityInput,
                  uint32_t *viewCountOutput,
                  XrView *views);

#ifdef OXR_HAVE_KHR_visibility_mask
//! OpenXR API function @ep{xrGetVisibilityMaskKHR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetVisibilityMaskKHR(XrSession session,
                           XrViewConfigurationType viewConfigurationType,
                           uint32_t viewIndex,
                           XrVisibilityMaskTypeKHR visibilityMaskType,
                           XrVisibilityMaskKHR *visibilityMask);
#endif // OXR_HAVE_KHR_visibility_mask

#ifdef OXR_HAVE_KHR_android_thread_settings
//! OpenXR API function @ep{xrSetAndroidApplicationThreadKHR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetAndroidApplicationThreadKHR(XrSession session, XrAndroidThreadTypeKHR threadType, uint32_t threadId);
#endif // OXR_HAVE_KHR_android_thread_settings

#ifdef OXR_HAVE_EXT_performance_settings
//! OpenXR API function @ep{xrPerfSettingsSetPerformanceLevelEXT}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrPerfSettingsSetPerformanceLevelEXT(XrSession session,
                                         XrPerfSettingsDomainEXT domain,
                                         XrPerfSettingsLevelEXT level);
#endif // OXR_HAVE_EXT_performance_settings

#ifdef OXR_HAVE_EXT_thermal_query
//! OpenXR API function @ep{xrThermalGetTemperatureTrendEXT}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrThermalGetTemperatureTrendEXT(XrSession session,
                                    XrPerfSettingsDomainEXT domain,
                                    XrPerfSettingsNotificationLevelEXT *notificationLevel,
                                    float *tempHeadroom,
                                    float *tempSlope);
#endif // OXR_HAVE_EXT_thermal_query


/*
 *
 * oxr_api_space.c
 *
 */

//! OpenXR API function @ep{xrEnumerateReferenceSpaces}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrEnumerateReferenceSpaces(XrSession session,
                               uint32_t spaceCapacityInput,
                               uint32_t *spaceCountOutput,
                               XrReferenceSpaceType *spaces);

//! OpenXR API function @ep{xrGetReferenceSpaceBoundsRect}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetReferenceSpaceBoundsRect(XrSession session, XrReferenceSpaceType referenceSpaceType, XrExtent2Df *bounds);

//! OpenXR API function @ep{xrCreateReferenceSpace}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCreateReferenceSpace(XrSession session, const XrReferenceSpaceCreateInfo *createInfo, XrSpace *space);

//! OpenXR API function @ep{xrLocateSpace}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime time, XrSpaceLocation *location);

//! OpenXR API function @ep{xrDestroySpace}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrDestroySpace(XrSpace space);


/*
 *
 * oxr_api_swapchain.c
 *
 */

//! OpenXR API function @ep{xrEnumerateSwapchainFormats}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrEnumerateSwapchainFormats(XrSession session,
                                uint32_t formatCapacityInput,
                                uint32_t *formatCountOutput,
                                int64_t *formats);

//! OpenXR API function @ep{xrCreateSwapchain}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo *createInfo, XrSwapchain *swapchain);

//! OpenXR API function @ep{xrDestroySwapchain}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrDestroySwapchain(XrSwapchain swapchain);

//! OpenXR API function @ep{xrEnumerateSwapchainImages}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrEnumerateSwapchainImages(XrSwapchain swapchain,
                               uint32_t imageCapacityInput,
                               uint32_t *imageCountOutput,
                               XrSwapchainImageBaseHeader *images);

//! OpenXR API function @ep{xrAcquireSwapchainImage}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrAcquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo *acquireInfo, uint32_t *index);

//! OpenXR API function @ep{xrWaitSwapchainImage}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrWaitSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageWaitInfo *waitInfo);

//! OpenXR API function @ep{xrReleaseSwapchainImage}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo *releaseInfo);


/*
 *
 * oxr_api_debug.c
 *
 */

//! OpenXR API function @ep{xrSetDebugUtilsObjectNameEXT}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetDebugUtilsObjectNameEXT(XrInstance instance, const XrDebugUtilsObjectNameInfoEXT *nameInfo);

//! OpenXR API function @ep{xrCreateDebugUtilsMessengerEXT}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCreateDebugUtilsMessengerEXT(XrInstance instance,
                                   const XrDebugUtilsMessengerCreateInfoEXT *createInfo,
                                   XrDebugUtilsMessengerEXT *messenger);

//! OpenXR API function @ep{xrDestroyDebugUtilsMessengerEXT}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrDestroyDebugUtilsMessengerEXT(XrDebugUtilsMessengerEXT messenger);

//! OpenXR API function @ep{xrSubmitDebugUtilsMessageEXT}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSubmitDebugUtilsMessageEXT(XrInstance instance,
                                 XrDebugUtilsMessageSeverityFlagsEXT messageSeverity,
                                 XrDebugUtilsMessageTypeFlagsEXT messageTypes,
                                 const XrDebugUtilsMessengerCallbackDataEXT *callbackData);

//! OpenXR API function @ep{xrSessionBeginDebugUtilsLabelRegionEXT}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSessionBeginDebugUtilsLabelRegionEXT(XrSession session, const XrDebugUtilsLabelEXT *labelInfo);

//! OpenXR API function @ep{xrSessionEndDebugUtilsLabelRegionEXT}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSessionEndDebugUtilsLabelRegionEXT(XrSession session);

//! OpenXR API function @ep{xrSessionInsertDebugUtilsLabelEXT}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSessionInsertDebugUtilsLabelEXT(XrSession session, const XrDebugUtilsLabelEXT *labelInfo);


/*
 *
 * oxr_api_action.c
 *
 */

//! OpenXR API function @ep{xrCreateActionSpace}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCreateActionSpace(XrSession session, const XrActionSpaceCreateInfo *createInfo, XrSpace *space);

//! OpenXR API function @ep{xrCreateActionSet}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCreateActionSet(XrInstance instance, const XrActionSetCreateInfo *createInfo, XrActionSet *actionSet);

//! OpenXR API function @ep{xrDestroyActionSet}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrDestroyActionSet(XrActionSet actionSet);

//! OpenXR API function @ep{xrCreateAction}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCreateAction(XrActionSet actionSet, const XrActionCreateInfo *createInfo, XrAction *action);

//! OpenXR API function @ep{xrDestroyAction}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrDestroyAction(XrAction action);

//! OpenXR API function @ep{xrSuggestInteractionProfileBindings}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSuggestInteractionProfileBindings(XrInstance instance,
                                        const XrInteractionProfileSuggestedBinding *suggestedBindings);

//! OpenXR API function @ep{xrAttachSessionActionSets}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrAttachSessionActionSets(XrSession session, const XrSessionActionSetsAttachInfo *bindInfo);

//! OpenXR API function @ep{xrGetCurrentInteractionProfile}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetCurrentInteractionProfile(XrSession session,
                                   XrPath topLevelUserPath,
                                   XrInteractionProfileState *interactionProfile);

//! OpenXR API function @ep{xrGetActionStateBoolean}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetActionStateBoolean(XrSession session, const XrActionStateGetInfo *getInfo, XrActionStateBoolean *data);

//! OpenXR API function @ep{xrGetActionStateFloat}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetActionStateFloat(XrSession session, const XrActionStateGetInfo *getInfo, XrActionStateFloat *data);

//! OpenXR API function @ep{xrGetActionStateVector2f}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetActionStateVector2f(XrSession session, const XrActionStateGetInfo *getInfo, XrActionStateVector2f *data);

//! OpenXR API function @ep{xrGetActionStatePose}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetActionStatePose(XrSession session, const XrActionStateGetInfo *getInfo, XrActionStatePose *data);

//! OpenXR API function @ep{xrSyncActions}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSyncActions(XrSession session, const XrActionsSyncInfo *syncInfo);

//! OpenXR API function @ep{xrEnumerateBoundSourcesForAction}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrEnumerateBoundSourcesForAction(XrSession session,
                                     const XrBoundSourcesForActionEnumerateInfo *enumerateInfo,
                                     uint32_t sourceCapacityInput,
                                     uint32_t *sourceCountOutput,
                                     XrPath *sources);

//! OpenXR API function @ep{xrGetInputSourceLocalizedName}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetInputSourceLocalizedName(XrSession session,
                                  const XrInputSourceLocalizedNameGetInfo *getInfo,
                                  uint32_t bufferCapacityInput,
                                  uint32_t *bufferCountOutput,
                                  char *buffer);

//! OpenXR API function @ep{xrApplyHapticFeedback}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrApplyHapticFeedback(XrSession session,
                          const XrHapticActionInfo *hapticActionInfo,
                          const XrHapticBaseHeader *hapticEvent);

//! OpenXR API function @ep{xrStopHapticFeedback}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrStopHapticFeedback(XrSession session, const XrHapticActionInfo *hapticActionInfo);

//! OpenXR API function @ep{xrCreateHandTrackerEXT}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCreateHandTrackerEXT(XrSession session,
                           const XrHandTrackerCreateInfoEXT *createInfo,
                           XrHandTrackerEXT *handTracker);

//! OpenXR API function @ep{xrDestroyHandTrackerEXT}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrDestroyHandTrackerEXT(XrHandTrackerEXT handTracker);

//! OpenXR API function @ep{xrLocateHandJointsEXT}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrLocateHandJointsEXT(XrHandTrackerEXT handTracker,
                          const XrHandJointsLocateInfoEXT *locateInfo,
                          XrHandJointLocationsEXT *locations);

//! OpenXR API function @ep{xrApplyForceFeedbackCurlMNDX}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrApplyForceFeedbackCurlMNDX(XrHandTrackerEXT handTracker, const XrForceFeedbackCurlApplyLocationsMNDX *locations);


//! OpenXR API function @ep{xrEnumerateDisplayRefreshRatesFB}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrEnumerateDisplayRefreshRatesFB(XrSession session,
                                     uint32_t displayRefreshRateCapacityInput,
                                     uint32_t *displayRefreshRateCountOutput,
                                     float *displayRefreshRates);

//! OpenXR API function @ep{xrGetDisplayRefreshRateFB}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetDisplayRefreshRateFB(XrSession session, float *displayRefreshRate);

//! OpenXR API function @ep{xrRequestDisplayRefreshRateFB}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrRequestDisplayRefreshRateFB(XrSession session, float displayRefreshRate);

//! OpenXR API function @ep{xrGetDeviceSampleRateFB}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetDeviceSampleRateFB(XrSession session,
                            const XrHapticActionInfo *hapticActionInfo,
                            XrDevicePcmSampleRateGetInfoFB *deviceSampleRate);

//! OpenXR API function @ep{xrLocateSpacesKHR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrLocateSpacesKHR(XrSession session, const XrSpacesLocateInfoKHR *locateInfo, XrSpaceLocationsKHR *spaceLocations);

//! OpenXR API function @ep{xrLocateSpaces}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrLocateSpaces(XrSession session, const XrSpacesLocateInfo *locateInfo, XrSpaceLocations *spaceLocations);

#ifdef OXR_HAVE_EXT_plane_detection
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCreatePlaneDetectorEXT(XrSession session,
                             const XrPlaneDetectorCreateInfoEXT *createInfo,
                             XrPlaneDetectorEXT *planeDetector);

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrDestroyPlaneDetectorEXT(XrPlaneDetectorEXT planeDetector);

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrBeginPlaneDetectionEXT(XrPlaneDetectorEXT planeDetector, const XrPlaneDetectorBeginInfoEXT *beginInfo);

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetPlaneDetectionStateEXT(XrPlaneDetectorEXT planeDetector, XrPlaneDetectionStateEXT *state);

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetPlaneDetectionsEXT(XrPlaneDetectorEXT planeDetector,
                            const XrPlaneDetectorGetInfoEXT *info,
                            XrPlaneDetectorLocationsEXT *locations);

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetPlanePolygonBufferEXT(XrPlaneDetectorEXT planeDetector,
                               uint64_t planeId,
                               uint32_t polygonBufferIndex,
                               XrPlaneDetectorPolygonBufferEXT *polygonBuffer);
#endif // OXR_HAVE_EXT_plane_detection

/*
 *
 * oxr_api_passthrough.c
 *
 */
#ifdef OXR_HAVE_FB_passthrough
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCreateGeometryInstanceFB(XrSession session,
                               const XrGeometryInstanceCreateInfoFB *createInfo,
                               XrGeometryInstanceFB *outGeometryInstance);
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCreatePassthroughFB(XrSession session,
                          const XrPassthroughCreateInfoFB *createInfo,
                          XrPassthroughFB *outPassthrough);
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCreatePassthroughLayerFB(XrSession session,
                               const XrPassthroughLayerCreateInfoFB *createInfo,
                               XrPassthroughLayerFB *outLayer);
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrDestroyGeometryInstanceFB(XrGeometryInstanceFB instance);
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrDestroyPassthroughFB(XrPassthroughFB passthrough);
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrDestroyPassthroughLayerFB(XrPassthroughLayerFB layer);
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGeometryInstanceSetTransformFB(XrGeometryInstanceFB instance,
                                     const XrGeometryInstanceTransformFB *transformation);
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrPassthroughLayerPauseFB(XrPassthroughLayerFB layer);
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrPassthroughLayerResumeFB(XrPassthroughLayerFB layer);
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrPassthroughLayerSetStyleFB(XrPassthroughLayerFB layer, const XrPassthroughStyleFB *style);
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrPassthroughPauseFB(XrPassthroughFB passthrough);
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrPassthroughStartFB(XrPassthroughFB passthrough);
#endif

#ifdef OXR_HAVE_HTC_facial_tracking
//! OpenXR API function @ep{xrCreateFacialTrackerHTC}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCreateFacialTrackerHTC(XrSession session,
                             const XrFacialTrackerCreateInfoHTC *createInfo,
                             XrFacialTrackerHTC *facialTracker);

//! OpenXR API function @ep{xrDestroyFacialTrackerHTC}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrDestroyFacialTrackerHTC(XrFacialTrackerHTC facialTracker);

//! OpenXR API function @ep{xrGetFacialExpressionsHTC}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetFacialExpressionsHTC(XrFacialTrackerHTC facialTracker, XrFacialExpressionsHTC *facialExpressions);
#endif

#ifdef OXR_HAVE_FB_body_tracking
//! OpenXR API function @ep{xrCreateBodyTrackerFB}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCreateBodyTrackerFB(XrSession session, const XrBodyTrackerCreateInfoFB *createInfo, XrBodyTrackerFB *bodyTracker);

//! OpenXR API function @ep{xrDestroyBodyTrackerFB}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrDestroyBodyTrackerFB(XrBodyTrackerFB bodyTracker);

//! OpenXR API function @ep{xrGetBodySkeletonFB}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetBodySkeletonFB(XrBodyTrackerFB bodyTracker, XrBodySkeletonFB *skeleton);

//! OpenXR API function @ep{xrLocateBodyJointsFB}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrLocateBodyJointsFB(XrBodyTrackerFB bodyTracker,
                         const XrBodyJointsLocateInfoFB *locateInfo,
                         XrBodyJointLocationsFB *locations);
#endif

#ifdef OXR_HAVE_FB_face_tracking2
//! OpenXR API function @ep{xrCreateFaceTracker2FB}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCreateFaceTracker2FB(XrSession session,
                           const XrFaceTrackerCreateInfo2FB *createInfo,
                           XrFaceTracker2FB *faceTracker);

//! OpenXR API function @ep{xrDestroyFaceTracker2FB}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrDestroyFaceTracker2FB(XrFaceTracker2FB faceTracker);

//! OpenXR API function @ep{xrGetFaceExpressionWeights2FB}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetFaceExpressionWeights2FB(XrFaceTracker2FB faceTracker,
                                  const XrFaceExpressionInfo2FB *expressionInfo,
                                  XrFaceExpressionWeights2FB *expressionWeights);
#endif

/*
 *
 * oxr_api_xdev.c
 *
 */

#ifdef OXR_HAVE_MNDX_xdev_space
//! OpenXR API function @ep{xrCreateXDevListMNDX}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCreateXDevListMNDX(XrSession session, const XrCreateXDevListInfoMNDX *info, XrXDevListMNDX *xdevList);

//! OpenXR API function @ep{xrGetXDevListGenerationNumberMNDX}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetXDevListGenerationNumberMNDX(XrXDevListMNDX session, uint64_t *outGeneration);

//! OpenXR API function @ep{xrEnumerateXDevsMNDX}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrEnumerateXDevsMNDX(XrXDevListMNDX xdevList,
                         uint32_t xdevCapacityInput,
                         uint32_t *xdevCountOutput,
                         XrXDevIdMNDX *xdevs);

//! OpenXR API function @ep{xrGetXDevProperty}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetXDevPropertiesMNDX(XrXDevListMNDX xdevList, const XrGetXDevInfoMNDX *info, XrXDevPropertiesMNDX *properties);

//! OpenXR API function @ep{xrDestroyXDevListMNDX}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrDestroyXDevListMNDX(XrXDevListMNDX xdevList);

//! OpenXR API function @ep{xrCreateXDevSpace}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCreateXDevSpaceMNDX(XrSession session, const XrCreateXDevSpaceInfoMNDX *createInfo, XrSpace *space);
#endif

/*
 *
 * oxr_api_body_tracking.c
 *
 */

#ifdef OXR_HAVE_META_body_tracking_calibration
//! OpenXR API function @ep{xrResetBodyTrackingCalibrationMETA}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrResetBodyTrackingCalibrationMETA(XrBodyTrackerFB bodyTracker);

//! OpenXR API function @ep{xrSuggestBodyTrackingCalibrationOverrideMETA}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSuggestBodyTrackingCalibrationOverrideMETA(XrBodyTrackerFB bodyTracker,
                                                 const XrBodyTrackingCalibrationInfoMETA *calibrationInfo);
#endif

#ifdef OXR_HAVE_DXR_display_info
//! OpenXR API function @ep{xrRequestDisplayModeDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrRequestDisplayModeEXT(XrSession session, XrDisplayModeDXR displayMode);
//! OpenXR API function @ep{xrRequestEyeTrackingModeDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrRequestEyeTrackingModeEXT(XrSession session, XrEyeTrackingModeDXR mode);
//! OpenXR API function @ep{xrRequestDisplayRenderingModeDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrRequestDisplayRenderingModeEXT(XrSession session, uint32_t modeIndex);
//! OpenXR API function @ep{xrEnumerateDisplayRenderingModesDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrEnumerateDisplayRenderingModesEXT(XrSession session,
                                        uint32_t modeCapacityInput,
                                        uint32_t *modeCountOutput,
                                        XrDisplayRenderingModeInfoDXR *modes);
//! OpenXR API function @ep{xrSetWorkspaceViewRigDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetWorkspaceViewRigEXT(XrSession session, const void *rig);

#endif

#ifdef OXR_HAVE_DXR_spatial_workspace
//! OpenXR API function @ep{xrActivateSpatialWorkspaceDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrActivateSpatialWorkspaceEXT(XrSession session);
//! OpenXR API function @ep{xrDeactivateSpatialWorkspaceDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrDeactivateSpatialWorkspaceEXT(XrSession session);
//! OpenXR API function @ep{xrGetSpatialWorkspaceStateDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetSpatialWorkspaceStateEXT(XrSession session, XrBool32 *out_active);
//! OpenXR API function @ep{xrAddWorkspaceCaptureClientDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrAddWorkspaceCaptureClientEXT(XrSession session,
                                   uint64_t nativeWindow,
                                   const char *nameOptional,
                                   XrWorkspaceClientId *outClientId);
//! OpenXR API function @ep{xrRemoveWorkspaceCaptureClientDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrRemoveWorkspaceCaptureClientEXT(XrSession session, XrWorkspaceClientId clientId);
//! OpenXR API function @ep{xrSetWorkspaceClientWindowPoseDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetWorkspaceClientWindowPoseEXT(XrSession session,
                                      XrWorkspaceClientId clientId,
                                      const XrPosef *pose,
                                      float widthMeters,
                                      float heightMeters);
//! OpenXR API function @ep{xrGetWorkspaceClientWindowPoseDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetWorkspaceClientWindowPoseEXT(XrSession session,
                                      XrWorkspaceClientId clientId,
                                      XrPosef *outPose,
                                      float *outWidthMeters,
                                      float *outHeightMeters);
//! OpenXR API function @ep{xrSetWorkspaceClientVisibilityDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetWorkspaceClientVisibilityEXT(XrSession session, XrWorkspaceClientId clientId, XrBool32 visible);
//! OpenXR API function @ep{xrSetWorkspaceFocusedClientDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetWorkspaceFocusedClientEXT(XrSession session, XrWorkspaceClientId clientId);
//! OpenXR API function @ep{xrSetWorkspaceReservedKeysDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetWorkspaceReservedKeysEXT(XrSession session,
                                 uint32_t keyCount,
                                 const XrWorkspaceReservedKeyDXR *keys);
//! OpenXR API function @ep{xrGetWorkspaceFocusedClientDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetWorkspaceFocusedClientEXT(XrSession session, XrWorkspaceClientId *outClientId);
//! OpenXR API function @ep{xrSetWorkspaceClientFrameRateCapDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetWorkspaceClientFrameRateCapEXT(XrSession session, XrWorkspaceClientId clientId, float maxFps);
//! OpenXR API function @ep{xrEnumerateWorkspaceInputEventsDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrEnumerateWorkspaceInputEventsEXT(XrSession session,
                                       uint32_t capacityInput,
                                       uint32_t *countOutput,
                                       XrWorkspaceInputEventDXR *events);
//! OpenXR API function @ep{xrEnableWorkspacePointerCaptureDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrEnableWorkspacePointerCaptureEXT(XrSession session, uint32_t button);
//! OpenXR API function @ep{xrDisableWorkspacePointerCaptureDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrDisableWorkspacePointerCaptureEXT(XrSession session);
//! OpenXR API function @ep{xrCaptureWorkspaceFrameDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCaptureWorkspaceFrameEXT(XrSession session,
                               const XrWorkspaceCaptureRequestDXR *request,
                               XrWorkspaceCaptureResultDXR *result);
//! OpenXR API function @ep{xrEnumerateWorkspaceClientsDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrEnumerateWorkspaceClientsEXT(XrSession session,
                                   uint32_t capacityInput,
                                   uint32_t *countOutput,
                                   XrWorkspaceClientId *clientIds);
//! OpenXR API function @ep{xrGetWorkspaceClientInfoDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetWorkspaceClientInfoEXT(XrSession session,
                                XrWorkspaceClientId clientId,
                                XrWorkspaceClientInfoDXR *info);
//! OpenXR API function @ep{xrRequestWorkspaceClientExitDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrRequestWorkspaceClientExitEXT(XrSession session, XrWorkspaceClientId clientId);
//! OpenXR API function @ep{xrRequestWorkspaceClientFullscreenDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrRequestWorkspaceClientFullscreenEXT(XrSession session, XrWorkspaceClientId clientId, XrBool32 fullscreen);
//! OpenXR API function @ep{xrCreateWorkspaceClientChromeSwapchainDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCreateWorkspaceClientChromeSwapchainEXT(XrSession session,
                                              XrWorkspaceClientId clientId,
                                              const XrWorkspaceChromeSwapchainCreateInfoDXR *createInfo,
                                              XrSwapchain *swapchain);
//! OpenXR API function @ep{xrDestroyWorkspaceClientChromeSwapchainDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrDestroyWorkspaceClientChromeSwapchainEXT(XrSwapchain swapchain);
//! OpenXR API function @ep{xrSetWorkspaceClientChromeLayoutDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetWorkspaceClientChromeLayoutEXT(XrSession session,
                                        XrWorkspaceClientId clientId,
                                        const XrWorkspaceChromeLayoutDXR *layout);
//! OpenXR API function @ep{xrUpdateWorkspaceClientChromeLayerPoseDXR} (spec_version 12)
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrUpdateWorkspaceClientChromeLayerPoseEXT(XrSession session,
                                              XrWorkspaceClientId clientId,
                                              const XrPosef *poseInClient);
//! OpenXR API function @ep{xrCreateWorkspaceCursorSwapchainDXR} (spec_version 13)
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCreateWorkspaceCursorSwapchainEXT(XrSession session,
                                         const XrWorkspaceCursorSwapchainCreateInfoDXR *createInfo,
                                         XrSwapchain *swapchain);
//! OpenXR API function @ep{xrSetWorkspaceCursorDXR} (spec_version 13)
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetWorkspaceCursorEXT(XrSession session, const XrWorkspaceCursorInfoDXR *info);
//! OpenXR API function @ep{xrSetWorkspaceCursorDepthDXR} (spec_version 22)
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetWorkspaceCursorDepthEXT(XrSession session, const XrWorkspaceCursorDepthDXR *info);
//! OpenXR API function @ep{xrCreateWorkspaceOverlaySwapchainDXR} (spec_version 17)
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCreateWorkspaceOverlaySwapchainEXT(XrSession session,
                                          const XrWorkspaceOverlaySwapchainCreateInfoDXR *createInfo,
                                          XrSwapchain *swapchain);
//! OpenXR API function @ep{xrSetWorkspaceOverlayDXR} (spec_version 17)
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetWorkspaceOverlayEXT(XrSession session, const XrWorkspaceOverlayInfoDXR *info);
//! OpenXR API function @ep{xrSetWorkspaceInputGrabDXR} (spec_version 18)
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetWorkspaceInputGrabEXT(XrSession session, XrBool32 grab);
//! OpenXR API function @ep{xrAcquireWorkspaceWakeupEventDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrAcquireWorkspaceWakeupEventEXT(XrSession session, uint64_t *outNativeHandle);
//! OpenXR API function @ep{xrSetWorkspaceClientStyleDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetWorkspaceClientStyleEXT(XrSession session,
                                 XrWorkspaceClientId clientId,
                                 const XrWorkspaceClientStyleDXR *style);
#endif

#ifdef OXR_HAVE_DXR_atlas_capture
//! OpenXR API function @ep{xrCaptureAtlasDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCaptureAtlasEXT(XrSession session, const XrAtlasCaptureInfoDXR *info, XrAtlasCaptureResultDXR *result);
#endif

#ifdef OXR_HAVE_DXR_workspace_file_dialog
//! OpenXR API function @ep{xrRequestFilePickerDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrRequestFilePickerEXT(XrSession session,
                           const XrFilePickerInfoDXR *info,
                           XrAsyncRequestIdDXR *requestId);
//! OpenXR API function @ep{xrGetFilePickerRequestDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetFilePickerRequestEXT(XrSession session,
                              XrAsyncRequestIdDXR requestId,
                              uint32_t *outClientId,
                              XrFilePickerInfoDXR *outInfo);
//! OpenXR API function @ep{xrCompleteFilePickerDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCompleteFilePickerEXT(XrSession session,
                            XrAsyncRequestIdDXR requestId,
                            XrFilePickerResultDXR result,
                            const char *path);
#endif

#ifdef OXR_HAVE_DXR_mcp_tools
//! OpenXR API function @ep{xrSetMCPAppInfoDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetMCPAppInfoEXT(XrSession session, const XrMCPAppInfoDXR *info);
//! OpenXR API function @ep{xrRegisterMCPToolDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrRegisterMCPToolEXT(XrSession session, const XrMCPToolInfoDXR *tool);
//! OpenXR API function @ep{xrUnregisterMCPToolDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrUnregisterMCPToolEXT(XrSession session, const char *name);
//! OpenXR API function @ep{xrGetMCPToolCallArgsDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetMCPToolCallArgsEXT(
    XrSession session, uint64_t callId, uint32_t capacity, uint32_t *countOutput, char *buffer);
//! OpenXR API function @ep{xrSubmitMCPToolResultDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSubmitMCPToolResultEXT(XrSession session, uint64_t callId, XrBool32 success, const char *resultJson);
#endif

#ifdef OXR_HAVE_DXR_local_3d_zone
//! OpenXR API function @ep{xrGetLocal3DZoneCapabilitiesDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetLocal3DZoneCapabilitiesEXT(XrSession session, XrLocal3DZoneCapabilitiesDXR *capabilities);

//! OpenXR API function @ep{xrCreateLocal3DZoneMaskDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCreateLocal3DZoneMaskEXT(XrSession session,
                               const XrLocal3DZoneMaskCreateInfoDXR *createInfo,
                               XrLocal3DZoneMaskDXR *mask);

//! OpenXR API function @ep{xrSetLocal3DZoneWholeWindowDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetLocal3DZoneWholeWindowEXT(XrLocal3DZoneMaskDXR mask, XrBool32 enable3D);

//! OpenXR API function @ep{xrSetLocal3DZoneFromRectsDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetLocal3DZoneFromRectsEXT(XrLocal3DZoneMaskDXR mask, uint32_t rectCount, const XrRect2Di *rects);

//! OpenXR API function @ep{xrAcquireLocal3DZoneRenderTargetDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrAcquireLocal3DZoneRenderTargetEXT(XrLocal3DZoneMaskDXR mask, void *binding);

//! OpenXR API function @ep{xrSubmitLocal3DZoneDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSubmitLocal3DZoneEXT(XrLocal3DZoneMaskDXR mask);

//! OpenXR API function @ep{xrDestroyLocal3DZoneMaskDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrDestroyLocal3DZoneMaskEXT(XrLocal3DZoneMaskDXR mask);
#endif

#ifdef OXR_HAVE_DXR_display_zones
//! OpenXR API function @ep{xrGetDisplayZoneCapabilitiesDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetDisplayZoneCapabilitiesEXT(XrSession session, XrDisplayZoneCapabilitiesDXR *capabilities);

//! OpenXR API function @ep{xrGetDisplayZoneRecommendedViewSizeDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetDisplayZoneRecommendedViewSizeEXT(XrSession session,
                                           const XrRect2Di *zoneRect,
                                           XrExtent2Di *recommendedViewSize);
#endif

#ifdef OXR_HAVE_DXR_weave
//! OpenXR API function @ep{xrWeaveBindWindowDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrWeaveBindWindowEXT(XrSession session, void *windowHandle);

//! OpenXR API function @ep{xrWeaveSubmitDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrWeaveSubmitEXT(XrSession session, const XrWeaveSubmitInfoDXR *submitInfo, XrWeaveOutputDXR *output);

//! OpenXR API function @ep{xrWeaveSnapWindowRectDXR}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrWeaveSnapWindowRectEXT(XrSession session,
                            const XrRect2Di *originRect,
                            const XrRect2Di *targetRect,
                            XrRect2Di *snappedRect);
#endif

#ifdef OXR_HAVE_EXT_conformance_automation
//! OpenXR API function @ep{xrSetInputDeviceActiveEXT}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetInputDeviceActiveEXT(XrSession session, XrPath interactionProfile, XrPath topLevelPath, XrBool32 isActive);

//! OpenXR API function @ep{xrSetInputDeviceStateBoolEXT}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetInputDeviceStateBoolEXT(XrSession session, XrPath topLevelPath, XrPath inputSourcePath, XrBool32 state);

//! OpenXR API function @ep{xrSetInputDeviceStateFloatEXT}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetInputDeviceStateFloatEXT(XrSession session, XrPath topLevelPath, XrPath inputSourcePath, float state);

//! OpenXR API function @ep{xrSetInputDeviceStateVector2fEXT}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetInputDeviceStateVector2fEXT(XrSession session, XrPath topLevelPath, XrPath inputSourcePath, XrVector2f state);

//! OpenXR API function @ep{xrSetInputDeviceLocationEXT}
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetInputDeviceLocationEXT(
    XrSession session, XrPath topLevelPath, XrPath inputSourcePath, XrSpace space, XrPosef pose);
#endif

/*!
 * @}
 */


#ifdef __cplusplus
}
#endif
