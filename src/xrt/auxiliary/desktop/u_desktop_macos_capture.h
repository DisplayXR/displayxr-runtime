// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS desktop control: ScreenCaptureKit stills (spec §10). Private to
 *         the macOS backend (u_desktop_macos.m).
 */

#pragma once

#include "u_desktop.h"
#include "u_desktop_macos_logic.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * True when ScreenCaptureKit is present (weak-linked: macOS 12.3+), so the
 * backend may set U_DESKTOP_CAP_CAPTURE. Never prompts.
 */
bool
u_desktop_macos_sck_available(void);

/*!
 * Bring-up / test hook (desktop_backend_smoke): take every still through a
 * one-shot SCStream even where SCScreenshotManager (macOS 14+) exists, so the
 * macOS 12.3–13 path can be exercised on a newer Mac. Process-wide.
 */
void
u_desktop_macos_sck_force_stream(bool on);

/*!
 * Draw the planned @p parts (u_desktop_macos_capture_plan over @p displays) into
 * @p img, which covers desktop rect @p src and is already allocated (withheld).
 * The calling process's own applications are excluded from the content filter.
 * Never prompts: the caller has checked CGPreflightScreenCaptureAccess.
 * Bounded by @p deadline_ns (os_monotonic_get_ns time). Returns OK,
 * NOT_GRANTED (TCC declined), TIMEOUT or FAILED; @p msg names the failure only.
 */
enum u_desktop_result
u_desktop_macos_sck_grab(const struct u_desktop_rect *src,
                         const struct u_desktop_macos_display *displays,
                         const struct u_desktop_macos_capture_part *parts,
                         uint32_t nparts,
                         struct u_desktop_image *img,
                         uint64_t deadline_ns,
                         char *msg);

#ifdef __cplusplus
}
#endif
