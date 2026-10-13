// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #886 — VK_LAYER_DXR_queue_lock activation routes, through the REAL loader.
 *
 * Probe half of tests_vk_queue_lock_activation.sh: create a VkInstance
 * (naming the layer when argv[1] is "named"), a VkDevice with one queue,
 * submit nothing + wait idle (a layer chained twice would deadlock on its own
 * per-queue mutex here), then resolve the two handshake entry points the
 * runtime resolves and print one line:
 *
 *   listed=<n> marker=<yes|no> activation=<0..3>
 *
 * Exit 77 (ctest SKIP) when the box has no Vulkan driver / device.
 */

#include <vulkan/vulkan.h>

#include <stdio.h>
#include <string.h>

typedef uint32_t(VKAPI_PTR *PFN_ql_marker)(void);
typedef uint32_t(VKAPI_PTR *PFN_ql_activation)(VkDevice device);

int
main(int argc, char **argv)
{
	const int named = argc > 1 && strcmp(argv[1], "named") == 0;
	const char *layers[] = {"VK_LAYER_DXR_queue_lock"};

	uint32_t lc = 0;
	if (vkEnumerateInstanceLayerProperties(&lc, NULL) != VK_SUCCESS) {
		printf("SKIP: vkEnumerateInstanceLayerProperties failed\n");
		return 77;
	}
	VkLayerProperties lp[128];
	if (lc > 128) {
		lc = 128;
	}
	vkEnumerateInstanceLayerProperties(&lc, lp);
	int listed = 0;
	for (uint32_t i = 0; i < lc; i++) {
		if (strcmp(lp[i].layerName, layers[0]) == 0) {
			listed++;
		}
	}

	VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
	VkInstanceCreateInfo ici = {
	    .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
	    .pApplicationInfo = &ai,
	    .enabledLayerCount = named ? 1 : 0,
	    .ppEnabledLayerNames = layers,
	};
	VkInstance inst = VK_NULL_HANDLE;
	VkResult r = vkCreateInstance(&ici, NULL, &inst);
	if (r == VK_ERROR_INCOMPATIBLE_DRIVER) {
		printf("SKIP: no Vulkan driver\n");
		return 77;
	}
	if (r != VK_SUCCESS) {
		printf("listed=%d createInstance=%d\n", listed, (int)r);
		return 1;
	}
	uint32_t n = 1;
	VkPhysicalDevice pd = VK_NULL_HANDLE;
	r = vkEnumeratePhysicalDevices(inst, &n, &pd);
	if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n == 0) {
		printf("SKIP: no Vulkan physical device\n");
		vkDestroyInstance(inst, NULL);
		return 77;
	}
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {
	    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
	    .queueFamilyIndex = 0,
	    .queueCount = 1,
	    .pQueuePriorities = &prio,
	};
	VkDeviceCreateInfo dci = {
	    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
	    .queueCreateInfoCount = 1,
	    .pQueueCreateInfos = &qci,
	};
	VkDevice dev = VK_NULL_HANDLE;
	r = vkCreateDevice(pd, &dci, NULL, &dev);
	if (r != VK_SUCCESS) {
		printf("listed=%d createDevice=%d\n", listed, (int)r);
		vkDestroyInstance(inst, NULL);
		return 1;
	}
	VkQueue queue = VK_NULL_HANDLE;
	vkGetDeviceQueue(dev, 0, 0, &queue);
	VkResult sr = vkQueueSubmit(queue, 0, NULL, VK_NULL_HANDLE);
	VkResult wr = vkQueueWaitIdle(queue);

	PFN_ql_marker marker = (PFN_ql_marker)vkGetDeviceProcAddr(dev, "vkGetQueueLockMarkerDXR");
	PFN_ql_activation act = (PFN_ql_activation)vkGetDeviceProcAddr(dev, "vkGetQueueLockActivationDXR");
	printf("listed=%d marker=%s activation=%u\n", listed, marker != NULL ? "yes" : "no",
	       act != NULL ? act(dev) : 0u);

	vkDestroyDevice(dev, NULL);
	vkDestroyInstance(inst, NULL);
	return (sr == VK_SUCCESS && wr == VK_SUCCESS) ? 0 : 1;
}
