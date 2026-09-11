// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "xrt/xrt_compositor.h"

//! Extra classification for CAMERA PROFILE ELIGIBILITY ONLY. Desktop Linux
//! Vulkan decodes app-owned Xlib/Wayland handles after common create-info parsing.
//! Other backends may report runtime-created handles (Android hosted windows),
//! so their flag must not change this policy or the shared session classification.
static inline bool
oxr_camera_profile_has_external_binding(bool desktop_linux, bool backend_external, const struct xrt_session_info *info)
{
	return (desktop_linux && backend_external) || info->external_window_handle != NULL ||
	       info->readback_callback != NULL || info->shared_texture_handle != NULL;
}
