// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "xrt/xrt_compositor.h"

//! Graphics-specific population may discover a binding that the common create
//! info did not carry (Vulkan Xlib/Wayland). Finalization must preserve that
//! classification, including for legacy camera-profile eligibility.
static inline bool
oxr_session_has_external_binding(bool backend_external, const struct xrt_session_info *info)
{
	return backend_external || info->external_window_handle != NULL || info->readback_callback != NULL ||
	       info->shared_texture_handle != NULL;
}
