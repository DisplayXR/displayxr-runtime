// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "xrt/xrt_compositor.h"

/*!
 * Does this session have an APP-PROVIDED window / surface / texture binding?
 *
 * This is the one authority behind `oxr_session::has_external_window`, and it
 * exists because the evidence arrives from two places:
 *
 *  - @p info — the common `xrt_session_info` filled by the create-info parse.
 *    Carries the win32 HWND, the cocoa NSView, the readback callback and the
 *    shared texture.
 *  - @p backend_external — what the graphics backend concluded. Desktop Linux
 *    Vulkan decodes `XR_DXR_xlib_window_binding` /
 *    `XR_DXR_wayland_surface_binding` LATE, inside `create_impl`, into a
 *    stack-local `comp_vk_native_xlib_handle` / `comp_vk_native_wayland_handle`
 *    that is deliberately never stored in @p info (its consumers dereference
 *    that field as an HWND / NSView, and the struct is a local besides). So on
 *    desktop Linux this flag is the ONLY evidence an app window exists.
 *
 * @p backend_external is honoured ONLY when @p desktop_linux, and that
 * asymmetry is the point: on Android the same backend flag describes a
 * RUNTIME-created hosted SurfaceView (`oxr_session_gfx_vk_native.c` spawns one
 * when no binding was chained), which is the opposite of an app binding and
 * must not be classified as one.
 */
static inline bool
oxr_session_has_external_window_binding(bool desktop_linux, bool backend_external, const struct xrt_session_info *info)
{
	return (desktop_linux && backend_external) || info->external_window_handle != NULL ||
	       info->readback_callback != NULL || info->shared_texture_handle != NULL;
}
