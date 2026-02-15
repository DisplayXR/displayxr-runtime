// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Public interface for the Leia 3D display driver.
 * @author David Fattal
 * @ingroup drv_leia
 */

#pragma once

#include "xrt/xrt_defines.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @defgroup drv_leia Leia 3D Display Driver
 * @ingroup drv
 *
 * @brief Driver for Leia light field displays using SR SDK (Windows)
 *        and CNSDK (Android).
 */

/*!
 * Create a Leia system builder.
 *
 * The builder will detect Leia 3D display hardware and create the
 * appropriate devices and display processors for the platform:
 * - Windows: SR SDK for Vulkan/D3D11 weaving + eye tracking
 * - Android: CNSDK for Vulkan interlacing
 *
 * @return A new xrt_builder, or NULL on failure.
 *
 * @ingroup drv_leia
 */
struct xrt_builder *
t_builder_leia_create(void);

#ifdef __cplusplus
}
#endif
