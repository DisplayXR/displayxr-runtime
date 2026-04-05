// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Windows.Graphics.Capture wrapper for 2D window capture.
 * @author David Fattal
 * @ingroup comp_d3d11_service
 *
 * Captures a Windows HWND as a D3D11 texture using the
 * Windows.Graphics.Capture API (Win10 2004+). The captured
 * texture is suitable for use as an SRV in the multi-compositor
 * blit pipeline.
 *
 * This header is C-linkage so it can be included from both
 * C and C++ translation units.
 */

#pragma once

#include <stdint.h>

// Forward declarations — avoid pulling in full D3D11/Windows headers.
struct ID3D11Device;
struct ID3D11Texture2D;
typedef struct HWND__ *HWND;

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Opaque capture context. Created by d3d11_capture_start(),
 * destroyed by d3d11_capture_stop().
 */
struct d3d11_capture_context;

/*!
 * Start capturing a window.
 *
 * Creates a WinRT GraphicsCaptureSession for the given HWND using
 * the provided D3D11 device. Frames are delivered asynchronously
 * on a WinRT thread pool and stored internally.
 *
 * @param device  The D3D11 device (must have multithread protection enabled).
 * @param hwnd    The window to capture.
 * @return Capture context, or NULL on failure.
 */
struct d3d11_capture_context *
d3d11_capture_start(struct ID3D11Device *device, HWND hwnd);

/*!
 * Get the latest captured texture.
 *
 * Returns a pointer to the staging texture containing the most recent
 * captured frame. The texture is owned by the capture context and remains
 * valid until the next call to d3d11_capture_get_texture() or
 * d3d11_capture_stop(). The caller must NOT release it.
 *
 * @param ctx        Capture context from d3d11_capture_start().
 * @param out_width  Receives texture width in pixels (may be NULL).
 * @param out_height Receives texture height in pixels (may be NULL).
 * @return Latest texture, or NULL if no frame has been captured yet.
 */
struct ID3D11Texture2D *
d3d11_capture_get_texture(struct d3d11_capture_context *ctx,
                          uint32_t *out_width,
                          uint32_t *out_height);

/*!
 * Stop capturing and destroy the context.
 *
 * Closes the capture session and frame pool, releases all resources.
 * The context pointer is invalid after this call.
 *
 * @param ctx  Capture context (may be NULL, in which case this is a no-op).
 */
void
d3d11_capture_stop(struct d3d11_capture_context *ctx);

#ifdef __cplusplus
}
#endif
