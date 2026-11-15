// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Present-hook re-entry detection, the Windows probe (#2635).
 *
 * The hazard and the check are described in comp_present_hook.h, which holds
 * the portable, unit-tested halves. This is the live half: resolve the module
 * that owns a DXGI swapchain's Present entry, and look for it on the calling
 * thread's stack. Shared by the D3D11 target (the hub and the native D3D11
 * leg) and the D3D12 own-legs target, so the two cannot drift apart on it.
 *
 * Read-only: VirtualQuery before every read of foreign code, module lookups
 * without taking a reference, and nothing is ever written or called.
 *
 * @ingroup comp_util
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Sample the re-entry latch on an APP-thread present of @p dxgi_swapchain (any
 * IDXGISwapChain*; never call it from a runtime-owned thread such as the
 * repaint thread, whose stack proves nothing).
 *
 * Does nothing once *@p io_reentrant is set. An unhooked chain is re-checked at
 * most once a second (*@p io_next_sample_ns); a hooked chain is checked on
 * every call until it latches, because one present handed to another thread
 * while the app sits in the hook is enough to deadlock. Microseconds when it
 * runs.
 *
 * @param out_module_name On latch: the hook module's file name (UTF-8), "?" if
 *                        unknown. May be NULL.
 * @return true exactly when this call latched *@p io_reentrant.
 */
bool
comp_present_hook_win_sample(void *dxgi_swapchain,
                             bool *io_reentrant,
                             uint64_t *io_next_sample_ns,
                             char *out_module_name,
                             size_t module_name_size);

#ifdef __cplusplus
}
#endif
