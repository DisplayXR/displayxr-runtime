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

/*!
 * Display dimensions in meters for Kooima FOV calculation.
 */
struct leiasr_display_dimensions
{
	float width_m;   //!< Screen width in meters
	float height_m;  //!< Screen height in meters
	bool valid;      //!< True if the dimensions are valid
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
 * @param[out] out_eye_pair Pointer to receive the eye positions (in meters)
 * @return true if valid eye positions are available, false otherwise
 */
bool
leiasr_get_predicted_eye_positions(struct leiasr *leiasr, struct leiasr_eye_pair *out_eye_pair);

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

#ifdef __cplusplus
}
#endif
