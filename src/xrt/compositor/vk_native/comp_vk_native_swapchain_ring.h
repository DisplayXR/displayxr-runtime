// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Acquire/wait/release image bookkeeping for the Vulkan native swapchain.
 *
 * Split out of @ref comp_vk_native_swapchain.c so the lifecycle can be unit
 * tested without a VkDevice (tests_comp_vk_native_swapchain_ring), and so the
 * rule it encodes is stated in one place:
 *
 * OpenXR lets an application hold **up to image_count** images acquired at the
 * same time (xrAcquireSwapchainImage is only required to fail once every image
 * is outstanding), and the acquire/wait/release calls are FIFO-ordered by the
 * state tracker. So the next index to hand out cannot be derived from the last
 * *released* index — with two acquires outstanding nothing has been released
 * yet and that arithmetic hands the same index out twice (#1504). Track the
 * per-image state instead, exactly as the D3D12 native swapchain does (#151).
 *
 * @ingroup comp_vk_native
 */

#pragma once

#include "xrt/xrt_results.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Maximum number of images in a swapchain.
 */
#define COMP_VK_NATIVE_MAX_SWAPCHAIN_IMAGES 8

/*!
 * Where one swapchain image is in the acquire/wait/release cycle.
 *
 * @ingroup comp_vk_native
 */
enum comp_vk_native_image_state
{
	//! Owned by the runtime, available to hand out.
	COMP_VK_NATIVE_IMAGE_FREE = 0,
	//! Handed to the app by xrAcquireSwapchainImage, not yet waited on.
	COMP_VK_NATIVE_IMAGE_ACQUIRED,
	//! Waited on by the app, not yet released.
	COMP_VK_NATIVE_IMAGE_WAITED,
};

/*!
 * Per-image acquire/wait/release state for one swapchain.
 *
 * @ingroup comp_vk_native
 */
struct comp_vk_native_swapchain_ring
{
	//! Number of images actually in the swapchain.
	uint32_t image_count;

	//! Where each image is in the cycle.
	enum comp_vk_native_image_state img_state[COMP_VK_NATIVE_MAX_SWAPCHAIN_IMAGES];

	//! Where the next acquire starts scanning, so images cycle round-robin.
	uint32_t next_hint;
};

/*!
 * Reset the ring to "every image free", ready for the first acquire.
 */
static inline void
comp_vk_native_swapchain_ring_init(struct comp_vk_native_swapchain_ring *ring, uint32_t image_count)
{
	ring->image_count = image_count;
	ring->next_hint = 0;
	for (uint32_t i = 0; i < COMP_VK_NATIVE_MAX_SWAPCHAIN_IMAGES; i++) {
		ring->img_state[i] = COMP_VK_NATIVE_IMAGE_FREE;
	}
}

/*!
 * Hand out the next free image index.
 *
 * @return XRT_SUCCESS, or XRT_ERROR_NO_IMAGE_AVAILABLE when every image is
 *         already outstanding. (Never XRT_ERROR_IPC_FAILURE: that one makes the
 *         state tracker mark the whole session lost, which a call-order slip on
 *         an otherwise healthy swapchain does not warrant.)
 */
static inline xrt_result_t
comp_vk_native_swapchain_ring_acquire(struct comp_vk_native_swapchain_ring *ring, uint32_t *out_index)
{
	for (uint32_t offset = 0; offset < ring->image_count; offset++) {
		uint32_t index = (ring->next_hint + offset) % ring->image_count;
		if (ring->img_state[index] != COMP_VK_NATIVE_IMAGE_FREE) {
			continue;
		}

		ring->img_state[index] = COMP_VK_NATIVE_IMAGE_ACQUIRED;
		ring->next_hint = (index + 1) % ring->image_count;
		*out_index = index;
		return XRT_SUCCESS;
	}

	return XRT_ERROR_NO_IMAGE_AVAILABLE;
}

/*!
 * Move an acquired image to waited.
 */
static inline xrt_result_t
comp_vk_native_swapchain_ring_wait(struct comp_vk_native_swapchain_ring *ring, uint32_t index)
{
	if (index >= ring->image_count || ring->img_state[index] != COMP_VK_NATIVE_IMAGE_ACQUIRED) {
		return XRT_ERROR_NO_IMAGE_AVAILABLE;
	}

	ring->img_state[index] = COMP_VK_NATIVE_IMAGE_WAITED;
	return XRT_SUCCESS;
}

/*!
 * Return a waited image to the free pool.
 */
static inline xrt_result_t
comp_vk_native_swapchain_ring_release(struct comp_vk_native_swapchain_ring *ring, uint32_t index)
{
	if (index >= ring->image_count || ring->img_state[index] != COMP_VK_NATIVE_IMAGE_WAITED) {
		return XRT_ERROR_NO_IMAGE_AVAILABLE;
	}

	ring->img_state[index] = COMP_VK_NATIVE_IMAGE_FREE;
	return XRT_SUCCESS;
}

/*!
 * How many images the app currently holds (acquired or waited). Diagnostics only.
 */
static inline uint32_t
comp_vk_native_swapchain_ring_outstanding(const struct comp_vk_native_swapchain_ring *ring)
{
	uint32_t n = 0;
	for (uint32_t i = 0; i < ring->image_count; i++) {
		if (ring->img_state[i] != COMP_VK_NATIVE_IMAGE_FREE) {
			n++;
		}
	}
	return n;
}

#ifdef __cplusplus
}
#endif
