// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Memory-type choice for importing a D3D11 shared texture into Vulkan.
 * @ingroup comp_vk_native
 */

#pragma once

#include "xrt/xrt_config_os.h"

#ifdef XRT_OS_WINDOWS

// Include after the file's own <windows.h> and
// VK_USE_PLATFORM_WIN32_KHR + <vulkan/vulkan_win32.h>, so this header does not
// change how either is configured.
#include "vk/vk_helpers.h"

#include <stdint.h>

/*!
 * Pick the memoryTypeIndex for importing @p handle as dedicated memory of an
 * image whose requirements allow @p image_type_bits.
 *
 * An import must pick from VkMemoryWin32HandlePropertiesKHR::memoryTypeBits as
 * well as from the image's requirements
 * (VUID-VkMemoryAllocateInfo-memoryTypeIndex-00645): taking the first image
 * bit alone chose type 0 on an RTX 4070 where the handle allows only type 1.
 * Both constraints apply, so intersect. Falls back to the image bits when the
 * proc is missing or the query fails, which is the old behaviour.
 *
 * The proc is resolved locally rather than added to vk_bundle: the bundle
 * crosses the plug-in ABI boundary (ADR-020). Same rule as
 * import_shared_d3d11_texture() in comp_vk_native_compositor.c.
 *
 * @return the index, or UINT32_MAX if no type is legal.
 */
static inline uint32_t
comp_vk_native_import_memory_type(struct vk_bundle *vk,
                                  uint32_t image_type_bits,
                                  VkExternalMemoryHandleTypeFlagBits handle_type,
                                  HANDLE handle)
{
	uint32_t legal = image_type_bits;

	PFN_vkGetMemoryWin32HandlePropertiesKHR get_handle_props =
	    (PFN_vkGetMemoryWin32HandlePropertiesKHR)vk->vkGetDeviceProcAddr(vk->device,
	                                                                     "vkGetMemoryWin32HandlePropertiesKHR");
	if (get_handle_props != NULL) {
		VkMemoryWin32HandlePropertiesKHR hp = {};
		hp.sType = VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR;
		if (get_handle_props(vk->device, handle_type, handle, &hp) == VK_SUCCESS && hp.memoryTypeBits != 0) {
			legal = hp.memoryTypeBits & image_type_bits;
			if (legal == 0) {
				legal = hp.memoryTypeBits;
			}
		}
	}

	VkPhysicalDeviceMemoryProperties mem_props = {};
	vk->vkGetPhysicalDeviceMemoryProperties(vk->physical_device, &mem_props);
	for (uint32_t k = 0; k < mem_props.memoryTypeCount; k++) {
		if ((legal & (1u << k)) != 0) {
			return k;
		}
	}
	return UINT32_MAX;
}

#endif // XRT_OS_WINDOWS
