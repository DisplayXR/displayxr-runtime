// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  VK_LAYER_DXR_queue_lock handshake names + activation-route policy (#886).
 *
 * The #868 late-weave repaint can share the app's VkQueue on a GPU whose
 * graphics family has a single queue (every Intel iGPU) only when
 * VK_LAYER_DXR_queue_lock serialises that queue (#902). How the layer gets
 * into the app's chain depends on who created the VkInstance:
 *
 *  - vulkan_enable2: the runtime creates it (xrCreateVulkanInstanceKHR) and
 *    INJECTS the layer by name — the app never knows.
 *  - vulkan_enable1: the app creates it. Either the app names the layer in
 *    its own VkInstanceCreateInfo (APP-ENABLED), or the implicit manifest's
 *    enable_environment DXR_VK_QUEUE_LOCK=1 is set (IMPLICIT), or the user
 *    forced it with the loader's own VK_INSTANCE_LAYERS /
 *    VK_LOADER_LAYERS_ENABLE (LOADER-ENV).
 *
 * The layer reports what it saw through a second handshake entry point
 * (U_VK_QL_ACTIVATION_FN), resolved with vkGetDeviceProcAddr exactly like
 * the #902 marker. The runtime combines that with what only it knows (did it
 * inject?) — this header is that combination, kept pure so it is unit-tested
 * (tests/tests_aux_vk_queue_lock_route.cpp) rather than eyeballed in a log.
 *
 * Header-only, no Vulkan types: the layer itself is dependency-free and keeps
 * its own copy of the names/values (src/xrt/targets/vk_layer/
 * queue_lock_layer.c) — keep the two in sync.
 */

#pragma once

#include "xrt/xrt_config_os.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! The layer's name, as an app or the runtime puts it in ppEnabledLayerNames.
#define U_VK_QL_LAYER_NAME "VK_LAYER_DXR_queue_lock"
//! #902 marker: resolves (non-NULL) only when the layer is in the device chain.
#define U_VK_QL_MARKER_FN "vkGetQueueLockMarkerDXR"
//! #886 activation query: `uint32_t fn(VkDevice)` returning U_VK_QL_ACT_*.
#define U_VK_QL_ACTIVATION_FN "vkGetQueueLockActivationDXR"
//! Implicit manifest's enable_environment (value "1") / disable_environment.
#define U_VK_QL_IMPLICIT_ENABLE_ENV "DXR_VK_QUEUE_LOCK"
#define U_VK_QL_IMPLICIT_DISABLE_ENV "DXR_VK_QUEUE_LOCK_DISABLE"

/*!
 * How a vulkan_enable1 app gets the layer, for log lines. The implicit
 * manifest ships on desktop Linux only (the .deb / tarball); elsewhere the
 * app has to name the layer itself.
 */
#ifdef XRT_OS_LINUX_DESKTOP
#define U_VK_QL_ENABLE1_HINT                                                                                           \
	"run it with DXR_VK_QUEUE_LOCK=1 (implicit VK_LAYER_DXR_queue_lock manifest) or have the app name "            \
	"VK_LAYER_DXR_queue_lock in its own VkInstanceCreateInfo"
#else
#define U_VK_QL_ENABLE1_HINT "have the app name VK_LAYER_DXR_queue_lock in its own VkInstanceCreateInfo"
#endif

/*!
 * What the layer saw at vkCreateInstance. Wire values — shared with the layer.
 */
enum u_vk_ql_activation
{
	//! Query absent (a layer older than #886) or device unknown to the layer.
	U_VK_QL_ACT_UNKNOWN = 0,
	//! Named in VkInstanceCreateInfo::ppEnabledLayerNames.
	U_VK_QL_ACT_NAMED = 1,
	//! Not named; the implicit manifest's DXR_VK_QUEUE_LOCK=1 was set.
	U_VK_QL_ACT_IMPLICIT = 2,
	//! Not named, no enable env: VK_INSTANCE_LAYERS / VK_LOADER_LAYERS_ENABLE.
	U_VK_QL_ACT_LOADER_ENV = 3,
};

/*!
 * The route the field log names.
 */
enum u_vk_ql_route
{
	U_VK_QL_ROUTE_ABSENT = 0,  //!< marker did not resolve: no layer in the chain
	U_VK_QL_ROUTE_INJECTED,    //!< the runtime added it at xrCreateVulkanInstanceKHR (enable2)
	U_VK_QL_ROUTE_APP_ENABLED, //!< the app named it itself
	U_VK_QL_ROUTE_IMPLICIT,    //!< implicit manifest, DXR_VK_QUEUE_LOCK=1
	U_VK_QL_ROUTE_LOADER_ENV,  //!< VK_INSTANCE_LAYERS / VK_LOADER_LAYERS_ENABLE
	U_VK_QL_ROUTE_UNKNOWN,     //!< live, but the layer predates the activation query
};

/*!
 * Resolve the activation route.
 *
 * @param layer_live        the #902 marker resolved on the device.
 * @param activation        the layer's answer (U_VK_QL_ACT_UNKNOWN when the
 *                          query did not resolve).
 * @param runtime_injected  the runtime itself added the layer to the
 *                          VkInstanceCreateInfo that created this device's
 *                          instance (enable2 only, and only when the app had
 *                          not already named it).
 */
static inline enum u_vk_ql_route
u_vk_ql_route_resolve(bool layer_live, uint32_t activation, bool runtime_injected)
{
	if (!layer_live) {
		return U_VK_QL_ROUTE_ABSENT;
	}
	switch (activation) {
	case U_VK_QL_ACT_NAMED: return runtime_injected ? U_VK_QL_ROUTE_INJECTED : U_VK_QL_ROUTE_APP_ENABLED;
	case U_VK_QL_ACT_IMPLICIT: return U_VK_QL_ROUTE_IMPLICIT;
	case U_VK_QL_ACT_LOADER_ENV: return U_VK_QL_ROUTE_LOADER_ENV;
	default:
		// An older layer cannot tell us — but if the runtime injected it, the
		// name is in the create info, so "injected" is still certain.
		return runtime_injected ? U_VK_QL_ROUTE_INJECTED : U_VK_QL_ROUTE_UNKNOWN;
	}
}

//! Stable log token for @p route (grep-able: "route=<token>").
static inline const char *
u_vk_ql_route_str(enum u_vk_ql_route route)
{
	switch (route) {
	case U_VK_QL_ROUTE_ABSENT: return "absent";
	case U_VK_QL_ROUTE_INJECTED: return "injected";
	case U_VK_QL_ROUTE_APP_ENABLED: return "app-enabled";
	case U_VK_QL_ROUTE_IMPLICIT: return "implicit";
	case U_VK_QL_ROUTE_LOADER_ENV: return "loader-env";
	case U_VK_QL_ROUTE_UNKNOWN: return "unknown";
	}
	return "unknown";
}

/*!
 * #886 / runtime#2039 guard: should the compositor say, once, that the
 * per-call serialisation is in effect for an app that brought its own device?
 *
 * True only for the combination where the layer's cost (if any) lands on an
 * app that never asked the runtime for anything: the app created the device
 * (vulkan_enable1), the layer is live, the repaint shares the APP's queue
 * (single-graphics-queue GPU — no runtime-owned queue), and the app can keep
 * more than one frame of GPU work queued on that queue. LOG ONLY — the
 * runtime#2039 collapse was later attributed to a tracked viewer and a low
 * GPU clock, not to the layer, so nothing here disables anything.
 *
 * @param frames_in_flight  how many app frames can be queued on the shared
 *                          queue; the runtime's proxy is the app swapchain's
 *                          image count minus the one the compositor reads.
 */
static inline bool
u_vk_ql_should_note_serialisation(bool app_created_device,
                                  bool layer_live,
                                  bool shared_queue_tier,
                                  uint32_t frames_in_flight)
{
	return app_created_device && layer_live && shared_queue_tier && frames_in_flight > 1;
}

#ifdef __cplusplus
}
#endif
