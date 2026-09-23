// Copyright 2026, DisplayXR contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Headless Vulkan instance/physical-device selection and DRM format
 *        modifier enumeration, shared by producer (for --modifiers=auto) and consumer.
 */
#pragma once

#include "common.h"

#include <drm_fourcc.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <vulkan/vulkan.h>
#include <xf86drm.h>

#define VKCHK(expr)                                                                                            \
	do {                                                                                                       \
		VkResult _r = (expr);                                                                                  \
		if (_r != VK_SUCCESS) {                                                                                \
			fprintf(stderr, "%s:%d: %s failed: %s (%d)\n", __FILE__, __LINE__, #expr, spike_vk_result(_r), _r); \
			exit(1);                                                                                           \
		}                                                                                                      \
	} while (0)

static inline const char *
spike_vk_result(VkResult r)
{
	switch (r) {
#define C(x)                                                                                                   \
	case x: return #x;
		C(VK_SUCCESS)
		C(VK_NOT_READY)
		C(VK_TIMEOUT)
		C(VK_INCOMPLETE)
		C(VK_ERROR_OUT_OF_HOST_MEMORY)
		C(VK_ERROR_OUT_OF_DEVICE_MEMORY)
		C(VK_ERROR_INITIALIZATION_FAILED)
		C(VK_ERROR_DEVICE_LOST)
		C(VK_ERROR_EXTENSION_NOT_PRESENT)
		C(VK_ERROR_FEATURE_NOT_PRESENT)
		C(VK_ERROR_FORMAT_NOT_SUPPORTED)
		C(VK_ERROR_INVALID_EXTERNAL_HANDLE)
		C(VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT)
		C(VK_ERROR_UNKNOWN)
#undef C
	default: return "VK_?";
	}
}

/* DRM fourcc -> Vulkan format with identical memory byte order. */
static inline VkFormat
spike_fourcc_to_vk(uint32_t fourcc, int *has_alpha)
{
	switch (fourcc) {
	case DRM_FORMAT_ARGB8888: *has_alpha = 1; return VK_FORMAT_B8G8R8A8_UNORM;
	case DRM_FORMAT_XRGB8888: *has_alpha = 0; return VK_FORMAT_B8G8R8A8_UNORM;
	case DRM_FORMAT_ABGR8888: *has_alpha = 1; return VK_FORMAT_R8G8B8A8_UNORM;
	case DRM_FORMAT_XBGR8888: *has_alpha = 0; return VK_FORMAT_R8G8B8A8_UNORM;
	default: *has_alpha = 0; return VK_FORMAT_UNDEFINED;
	}
}

static inline uint32_t
spike_parse_fourcc(const char *s)
{
	if (!strcasecmp(s, "ARGB8888")) return DRM_FORMAT_ARGB8888;
	if (!strcasecmp(s, "XRGB8888")) return DRM_FORMAT_XRGB8888;
	if (!strcasecmp(s, "ABGR8888")) return DRM_FORMAT_ABGR8888;
	if (!strcasecmp(s, "XBGR8888")) return DRM_FORMAT_XBGR8888;
	fprintf(stderr, "unknown fourcc %s\n", s);
	exit(2);
}

static inline const char *
spike_fourcc_name(uint32_t f)
{
	static char b[8];
	snprintf(b, sizeof(b), "%c%c%c%c", f & 0xff, (f >> 8) & 0xff, (f >> 16) & 0xff, (f >> 24) & 0xff);
	return b;
}

/* Returns a static-ish buffer: "0x... (VENDOR_NAME)". */
static inline const char *
spike_mod_name(uint64_t mod)
{
	static char bufs[4][160];
	static int idx;
	char *b = bufs[idx++ & 3];
	char *name = drmGetFormatModifierName(mod);
	char *vendor = drmGetFormatModifierVendor(mod);
	snprintf(b, 160, "0x%016llx (%s_%s)", (unsigned long long)mod, vendor ? vendor : "?", name ? name : "?");
	free(name);
	free(vendor);
	return b;
}

static inline VkInstance
spike_vk_instance(void)
{
	VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
	                        .pApplicationName = "dxr-dmabuf-spike",
	                        .apiVersion = VK_API_VERSION_1_3};
	VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai};
	VkInstance inst;
	VKCHK(vkCreateInstance(&ici, NULL, &inst));
	return inst;
}

static inline int
spike_has_dev_ext(VkPhysicalDevice pd, const char *name)
{
	uint32_t n = 0;
	vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
	VkExtensionProperties *p = calloc(n, sizeof(*p));
	vkEnumerateDeviceExtensionProperties(pd, NULL, &n, p);
	int found = 0;
	for (uint32_t i = 0; i < n; i++)
		if (!strcmp(p[i].extensionName, name))
			found = 1;
	free(p);
	return found;
}

/*
 * Pick the physical device whose VK_EXT_physical_device_drm render node matches
 * render_node (e.g. /dev/dri/renderD128). With want_cpu, pick the CPU device instead.
 */
static inline VkPhysicalDevice
spike_vk_pick(VkInstance inst, const char *render_node, int want_cpu)
{
	struct stat st;
	if (stat(render_node, &st) != 0) {
		perror(render_node);
		exit(1);
	}
	uint32_t n = 0;
	vkEnumeratePhysicalDevices(inst, &n, NULL);
	VkPhysicalDevice *pds = calloc(n, sizeof(*pds));
	vkEnumeratePhysicalDevices(inst, &n, pds);
	VkPhysicalDevice pick = VK_NULL_HANDLE;
	for (uint32_t i = 0; i < n; i++) {
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties(pds[i], &props);
		if (want_cpu) {
			if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
				pick = pds[i];
			continue;
		}
		if (!spike_has_dev_ext(pds[i], VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME))
			continue;
		VkPhysicalDeviceDrmPropertiesEXT drm = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
		VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drm};
		vkGetPhysicalDeviceProperties2(pds[i], &p2);
		if (drm.hasRender && (dev_t)makedev(drm.renderMajor, drm.renderMinor) == st.st_rdev)
			pick = pds[i];
	}
	free(pds);
	if (pick == VK_NULL_HANDLE) {
		fprintf(stderr, "no Vulkan physical device matches %s%s\n", render_node, want_cpu ? " (cpu)" : "");
		exit(1);
	}
	return pick;
}

struct spike_mod_info
{
	uint64_t modifier;
	uint32_t planes;
	VkFormatFeatureFlags2 features;
};

/* Enumerate modifiers for a format. Returns count; *out is malloc'd. */
static inline uint32_t
spike_vk_modifiers(VkPhysicalDevice pd, VkFormat fmt, struct spike_mod_info **out)
{
	VkDrmFormatModifierPropertiesList2EXT l2 = {.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT};
	VkFormatProperties2 fp = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &l2};
	vkGetPhysicalDeviceFormatProperties2(pd, fmt, &fp);
	l2.pDrmFormatModifierProperties = calloc(l2.drmFormatModifierCount, sizeof(VkDrmFormatModifierProperties2EXT));
	vkGetPhysicalDeviceFormatProperties2(pd, fmt, &fp);
	*out = calloc(l2.drmFormatModifierCount ? l2.drmFormatModifierCount : 1, sizeof(**out));
	for (uint32_t i = 0; i < l2.drmFormatModifierCount; i++) {
		(*out)[i].modifier = l2.pDrmFormatModifierProperties[i].drmFormatModifier;
		(*out)[i].planes = l2.pDrmFormatModifierProperties[i].drmFormatModifierPlaneCount;
		(*out)[i].features = l2.pDrmFormatModifierProperties[i].drmFormatModifierTilingFeatures;
	}
	uint32_t n = l2.drmFormatModifierCount;
	free((void *)l2.pDrmFormatModifierProperties);
	return n;
}

/*
 * Would the driver actually accept an image of this format/modifier with the
 * given usage and dma-buf external handle? (Format features alone are not the
 * full answer; this is what an importer must ask.)
 */
static inline VkResult
spike_vk_mod_image_ok(VkPhysicalDevice pd, VkFormat fmt, uint64_t mod, VkImageUsageFlags usage,
                      VkExternalMemoryFeatureFlags *ext_feat)
{
	VkPhysicalDeviceImageDrmFormatModifierInfoEXT mi = {
	    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
	    .drmFormatModifier = mod,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
	VkPhysicalDeviceExternalImageFormatInfo ei = {
	    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
	    .pNext = &mi,
	    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
	VkPhysicalDeviceImageFormatInfo2 fi = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
	                                       .pNext = &ei,
	                                       .format = fmt,
	                                       .type = VK_IMAGE_TYPE_2D,
	                                       .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
	                                       .usage = usage};
	VkExternalImageFormatProperties ep = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
	VkImageFormatProperties2 ip = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &ep};
	VkResult r = vkGetPhysicalDeviceImageFormatProperties2(pd, &fi, &ip);
	if (ext_feat)
		*ext_feat = ep.externalMemoryProperties.externalMemoryFeatures;
	return r;
}
