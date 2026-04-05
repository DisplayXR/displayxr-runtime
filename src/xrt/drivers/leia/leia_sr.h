#pragma once

#include "xrt/xrt_results.h"
#include "xrt/xrt_config_vulkan.h" // VK_USE_PLATFORM_WIN32_KHR etc.
#include "xrt/xrt_windows.h"       // windows.h with proper defines (before vulkan.h)
#include "leia_types.h"

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Forward declaration for the SR context holder
struct leiasr;

/*!
 * Create a leiasr instance with Vulkan weaver support.
 * Used for per-session rendering with SR interlacing.
 *
 * @param maxTime Maximum time in seconds to wait for SR to become ready
 * @param device Vulkan device
 * @param physicalDevice Vulkan physical device
 * @param graphicsQueue Vulkan graphics queue
 * @param commandPool Vulkan command pool
 * @param windowHandle Window handle (HWND on Windows), NULL for fullscreen
 * @param[out] out Pointer to receive the created leiasr instance
 * @return XRT_SUCCESS on success
 */
xrt_result_t
leiasr_create(double maxTime,
              VkDevice device,
              VkPhysicalDevice physicalDevice,
              VkQueue graphicsQueue,
              VkCommandPool commandPool,
              void *windowHandle,
              struct leiasr **out);

/*!
 * Destroy a leiasr instance.
 * Frees all resources.
 *
 * @param leiasr The instance to destroy (can be NULL)
 */
void
leiasr_destroy(struct leiasr *leiasr);

/*!
 * Perform SR weaving to interlace left/right views.
 *
 * @param leiasr The leiasr instance
 * @param commandBuffer Vulkan command buffer to record weaving commands
 * @param leftImageView Left eye image view
 * @param rightImageView Right eye image view
 * @param viewport Viewport rectangle
 * @param imageWidth Source image width
 * @param imageHeight Source image height
 * @param imageFormat Source image format
 * @param framebuffer Target framebuffer (can be VK_NULL_HANDLE if SR handles it)
 * @param framebufferWidth Target framebuffer width
 * @param framebufferHeight Target framebuffer height
 * @param framebufferFormat Target framebuffer format
 */
void
leiasr_weave(struct leiasr *leiasr,
             VkCommandBuffer commandBuffer,
             VkImageView leftImageView,
             VkImageView rightImageView,
             VkRect2D viewport,
             int imageWidth,
             int imageHeight,
             VkFormat imageFormat,
             VkFramebuffer framebuffer,
             int framebufferWidth,
             int framebufferHeight,
             VkFormat framebufferFormat);

/*!
 * Get predicted eye positions from the weaver's LookaroundFilter.
 * This uses the weaver's internal prediction filter tuned for
 * application-specific latency.
 *
 * @param leiasr The leiasr instance (must have a weaver)
 * @param[out] out_eye_pos Pointer to receive the eye positions (in meters)
 * @return true if valid eye positions are available, false otherwise
 */
bool
leiasr_get_predicted_eye_positions(struct leiasr *leiasr, struct leiasr_eye_pair *out_eye_pos);

/*!
 * Check if the leiasr instance has a weaver (and thus supports getPredictedEyePositions).
 *
 * @param leiasr The leiasr instance
 * @return true if the instance has a weaver
 */
bool
leiasr_has_weaver(struct leiasr *leiasr);

/*!
 * Get the display dimensions for Kooima FOV calculation.
 * The dimensions are cached from SR::Display during initialization.
 *
 * @param leiasr The leiasr instance
 * @param[out] out_dims Pointer to receive the display dimensions (in meters)
 * @return true if valid display dimensions are available, false otherwise
 */
bool
leiasr_get_display_dimensions(struct leiasr *leiasr, struct leiasr_display_dimensions *out_dims);

/*!
 * Log diagnostic info about the window position relative to the SR display.
 * Queries the window's actual screen position via Win32 and compares with
 * the SR display location. Useful for diagnosing interlacing alignment issues.
 *
 * @param leiasr The leiasr instance
 * @param windowHandle Window handle (HWND) to query position for
 */
void
leiasr_log_window_diagnostics(struct leiasr *leiasr, void *windowHandle);

/*!
 * Get display pixel resolution, screen position, and physical size.
 * Used for computing window metrics (adaptive FOV and eye offset).
 *
 * @param leiasr The leiasr instance
 * @param[out] out_display_pixel_width Display width in pixels.
 * @param[out] out_display_pixel_height Display height in pixels.
 * @param[out] out_display_screen_left Display left edge in screen coords.
 * @param[out] out_display_screen_top Display top edge in screen coords.
 * @param[out] out_display_width_m Display physical width in meters.
 * @param[out] out_display_height_m Display physical height in meters.
 * @return true if all values are valid.
 */
bool
leiasr_get_display_pixel_info(struct leiasr *leiasr,
                               uint32_t *out_display_pixel_width,
                               uint32_t *out_display_pixel_height,
                               int32_t *out_display_screen_left,
                               int32_t *out_display_screen_top,
                               float *out_display_width_m,
                               float *out_display_height_m);

/*!
 * Get the recommended view texture dimensions from the SR display.
 * These dimensions are queried from the SR SDK during initialization and should
 * be used for creating swapchains and the compositor stereo texture.
 *
 * @param leiasr The leiasr instance.
 * @param[out] out_width Recommended width per view (single eye).
 * @param[out] out_height Recommended height per view.
 * @return true if valid dimensions are available, false otherwise.
 */
bool
leiasr_get_recommended_view_dimensions(struct leiasr *leiasr,
                                        uint32_t *out_width,
                                        uint32_t *out_height);

/*!
 * Get window metrics for adaptive FOV and eye position adjustment.
 * Combines cached display info with live GetClientRect/ClientToScreen
 * on the stored window handle to compute window geometry in meters.
 *
 * @param leiasr The leiasr instance (must have valid windowHandle).
 * @param[out] out_metrics Pointer to receive the window metrics.
 * @return true if valid metrics are available, false otherwise.
 */
bool
leiasr_get_window_metrics(struct leiasr *leiasr,
                           struct leiasr_window_metrics *out_metrics);

/*!
 * Request display mode switch (2D/3D) via SR SwitchableLensHint.
 *
 * @param leiasr The leiasr instance.
 * @param enable_3d true to switch to 3D mode, false for 2D mode.
 * @return true on success.
 */
bool
leiasr_request_display_mode(struct leiasr *leiasr, bool enable_3d);

/*!
 * Check if the SR display supports 2D/3D mode switching.
 *
 * @param leiasr The leiasr instance.
 * @return true if SwitchableLensHint is available.
 */
bool
leiasr_supports_display_mode_switch(struct leiasr *leiasr);

/*!
 * Query hardware 3D display state from SR SwitchableLensHint.
 *
 * @param leiasr The leiasr instance.
 * @param[out] out_is_3d true if lens is currently enabled (3D mode).
 * @return true if query succeeded (SwitchableLensHint available).
 */
bool
leiasr_get_hardware_3d_state(struct leiasr *leiasr, bool *out_is_3d);

#ifdef __cplusplus
}
#endif
