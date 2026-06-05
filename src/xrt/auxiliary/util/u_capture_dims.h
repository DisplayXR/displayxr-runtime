// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Process-global provider for the in-process compositor's CURRENT
 *         window-scaled capture dimensions.
 *
 * The static @c xrt_system_compositor_info carries the NOMINAL atlas / view
 * dims (display-sized). When an app window differs from the display, the
 * in-process compositor renders — and captures — at window-scaled dims that
 * live only inside the compositor's renderer. xrCaptureAtlasEXT runs in the
 * state tracker (a layer above the compositor) and cannot reach those dims, so
 * it would report the nominal dims, disagreeing with the captured PNG (#431).
 *
 * This tiny registry bridges the gap without a compositor↔state-tracker edge:
 * it lives in aux_util (below both). The active in-process native compositor
 * registers a pull-callback at creation; the state tracker queries it when
 * filling @c XrAtlasCaptureResultEXT. Compositors that don't register (or a
 * not-yet-sized renderer) leave the query returning false, so the caller falls
 * back to the nominal dims.
 *
 * @ingroup aux_util
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Returns the compositor's current window-scaled per-view dims + tile layout —
 * what the next capture will actually write. @p userdata is the value passed to
 * @ref u_capture_dims_set_provider. Returns true and fills the out-params on
 * success; false leaves them untouched.
 */
typedef bool (*u_capture_dims_fn)(void *userdata,
                                  uint32_t *out_view_w,
                                  uint32_t *out_view_h,
                                  uint32_t *out_tile_cols,
                                  uint32_t *out_tile_rows);

/*!
 * Register (@p fn != NULL) or clear the provider. On clear, pass the same
 * @p userdata that was registered — the slot is only released if it still
 * belongs to that caller, so a torn-down compositor never clobbers a provider a
 * newer compositor installed.
 */
void
u_capture_dims_set_provider(u_capture_dims_fn fn, void *userdata);

/*!
 * Query the registered provider. Returns false (out-params untouched) if no
 * provider is installed or the provider declined.
 */
bool
u_capture_dims_query(uint32_t *out_view_w,
                     uint32_t *out_view_h,
                     uint32_t *out_tile_cols,
                     uint32_t *out_tile_rows);

#ifdef __cplusplus
}
#endif
