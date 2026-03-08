// Copyright 2019-2022, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header for limits of the XRT interfaces.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_compiler.h"


/*!
 * @addtogroup xrt_iface
 * @{
 */
/*
 * Max number of views supported by a compositor, artificial limit.
 *
 * Raised from 2 to 8 to support multiview 3D displays (light field
 * displays with 4-8 views).  Cannot go higher without refactoring
 * shader UBOs (render_layer_ubo_data) and stack-allocated dispatch
 * arrays in the compositor layer renderer.
 */
#define XRT_MAX_VIEWS 8

/*!
 * Maximum number of handles sent in one call.
 */
#define XRT_MAX_IPC_HANDLES 16

/*!
 * Max swapchain images, artificial limit.
 *
 * Must be smaller or the same as XRT_MAX_IPC_HANDLES.
 */
#define XRT_MAX_SWAPCHAIN_IMAGES 8

/*!
 * Max formats supported by a compositor, artificial limit.
 */
#define XRT_MAX_SWAPCHAIN_FORMATS 16

/*!
 * Max number of plane orientations that can be requested at a time.
 */
#define XRT_MAX_PLANE_ORIENTATIONS_EXT 256

/*!
 * Max number of plane semantic types that can be requested at a time.
 */
#define XRT_MAX_PLANE_SEMANTIC_TYPE_EXT 256

/*!
 * Max formats in the swapchain creation info formats list, artificial limit.
 */
#define XRT_MAX_SWAPCHAIN_CREATE_INFO_FORMAT_LIST_COUNT 8

/*!
 * Max number of supported display refresh rates, artificial limit.
 */
#define XRT_MAX_SUPPORTED_REFRESH_RATES 16

/*!
 * Max number of rendering modes a device can expose.
 */
#define XRT_MAX_RENDERING_MODES 8

/*!
 * Max number of layers which can be handled at once.
 */
#define XRT_MAX_LAYERS 128

/*!
 * @}
 */
