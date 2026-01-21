// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Service interface for creating per-session render targets.
 *
 * This service pattern breaks the circular dependency between comp_main and
 * comp_multi. comp_main implements this interface and provides it to comp_multi
 * during initialization, allowing comp_multi to request per-session target
 * creation without directly linking against comp_main.
 *
 * @author David Fattal
 * @ingroup comp_util
 */

#pragma once

#include "xrt/xrt_results.h"

#include <stddef.h> // For NULL

#ifdef __cplusplus
extern "C" {
#endif

struct comp_target;
struct vk_bundle;

/*!
 * Service interface for creating per-session render targets.
 *
 * This allows comp_multi to request target creation from comp_main
 * without direct dependency, breaking the circular library dependency.
 *
 * @ingroup comp_util
 */
struct comp_target_service
{
	/*!
	 * Create a render target from an external window handle.
	 *
	 * @param service                  The service instance
	 * @param external_window_handle   Platform window handle (HWND on Windows)
	 * @param out_target               Created target (caller takes ownership)
	 * @return XRT_SUCCESS or error code
	 */
	xrt_result_t (*create_from_window)(struct comp_target_service *service,
	                                   void *external_window_handle,
	                                   struct comp_target **out_target);

	/*!
	 * Destroy a target created by this service.
	 *
	 * @param service The service instance
	 * @param target  Target to destroy (will be set to NULL)
	 */
	void (*destroy_target)(struct comp_target_service *service, struct comp_target **target);

	/*!
	 * Get the Vulkan bundle from the compositor.
	 *
	 * @param service The service instance
	 * @return The vk_bundle pointer
	 */
	struct vk_bundle *(*get_vk)(struct comp_target_service *service);

	//! Opaque context for implementation (typically comp_compositor*)
	void *context;
};

/*!
 * Convenience wrapper for creating a target from an external window.
 *
 * @public @memberof comp_target_service
 * @ingroup comp_util
 */
static inline xrt_result_t
comp_target_service_create(struct comp_target_service *service,
                           void *external_window_handle,
                           struct comp_target **out_target)
{
	if (service == NULL || service->create_from_window == NULL) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	return service->create_from_window(service, external_window_handle, out_target);
}

/*!
 * Convenience wrapper for destroying a target.
 *
 * @public @memberof comp_target_service
 * @ingroup comp_util
 */
static inline void
comp_target_service_destroy(struct comp_target_service *service, struct comp_target **target)
{
	if (service == NULL || service->destroy_target == NULL || target == NULL || *target == NULL) {
		return;
	}
	service->destroy_target(service, target);
}

/*!
 * Convenience wrapper for getting the Vulkan bundle.
 *
 * @public @memberof comp_target_service
 * @ingroup comp_util
 */
static inline struct vk_bundle *
comp_target_service_get_vk(struct comp_target_service *service)
{
	if (service == NULL || service->get_vk == NULL) {
		return NULL;
	}
	return service->get_vk(service);
}


#ifdef __cplusplus
}
#endif
