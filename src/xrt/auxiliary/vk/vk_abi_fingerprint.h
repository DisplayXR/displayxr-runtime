// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ABI fingerprint accessors for struct vk_bundle (#1243).
 *
 * Deliberately free of Vulkan includes so the plug-in loader can call these
 * without pulling Vulkan headers into its translation unit, while both the
 * defining TU (vk_helpers.c, via vk_helpers.h) and the consuming TU
 * (target_plugin_loader.c) see the SAME prototypes — a hand-written duplicate
 * declaration could silently diverge from the definition, which is the exact
 * failure class the fingerprint exists to prevent.
 *
 * @ingroup aux_vk
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! `sizeof(struct vk_bundle)` as compiled into aux_vk. */
uint32_t
vk_bundle_get_abi_size(void);

/*!
 * `offsetof(struct vk_bundle, vkGetInstanceProcAddr)` — where the function
 * table starts. `sizeof` alone cannot catch two header-gated members changing
 * in compensating directions; any shift of the table start is caught here
 * whether or not the total size moved.
 */
uint32_t
vk_bundle_get_fn_table_offset(void);

#ifdef __cplusplus
}
#endif
