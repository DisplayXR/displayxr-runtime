#pragma once

#include "xrt/xrt_results.h"
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Eye position in meters (converted from SR's millimeters).
 * Position is relative to the display center.
 */
struct leiasr_eye_position
{
	float x;  //!< Horizontal position (positive = right)
	float y;  //!< Vertical position (positive = up)
	float z;  //!< Depth position (positive = toward viewer)
};

/*!
 * Eye pair containing both left and right eye positions.
 */
struct leiasr_eye_pair
{
	struct leiasr_eye_position left;   //!< Left eye position in meters
	struct leiasr_eye_position right;  //!< Right eye position in meters
	int64_t timestamp_ns;              //!< Monotonic timestamp when the eye positions were sampled
	bool valid;                        //!< True if the eye positions are valid
};

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
 * Create a leiasr instance for eye tracking only (no Vulkan/weaver).
 * This is used at the system compositor level to share eye tracking data
 * across all sessions.
 *
 * @param maxTime Maximum time in seconds to wait for SR to become ready
 * @param[out] out Pointer to receive the created leiasr instance
 * @return XRT_SUCCESS on success
 */
xrt_result_t
leiasr_create_eye_tracker_only(double maxTime, struct leiasr **out);

/*!
 * Destroy a leiasr instance.
 * Stops eye tracking if active and frees all resources.
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
 * Start the eye tracker and begin receiving eye position updates.
 * The eye tracker runs on its own thread and calls back with updates.
 *
 * @param leiasr The leiasr instance
 * @return XRT_SUCCESS if eye tracking started successfully
 */
xrt_result_t
leiasr_eye_tracker_start(struct leiasr *leiasr);

/*!
 * Stop the eye tracker and stop receiving updates.
 *
 * @param leiasr The leiasr instance
 */
void
leiasr_eye_tracker_stop(struct leiasr *leiasr);

/*!
 * Get the latest eye positions from the eye tracker.
 * This function is thread-safe and can be called from any thread.
 *
 * @param leiasr The leiasr instance
 * @param[out] out_eye_pair Pointer to receive the eye positions
 * @return true if valid eye positions are available, false otherwise
 */
bool
leiasr_get_eye_positions(struct leiasr *leiasr, struct leiasr_eye_pair *out_eye_pair);

/*!
 * Check if eye tracking is currently active.
 *
 * @param leiasr The leiasr instance
 * @return true if eye tracking is active
 */
bool
leiasr_is_eye_tracking_active(struct leiasr *leiasr);

#ifdef __cplusplus
}
#endif
