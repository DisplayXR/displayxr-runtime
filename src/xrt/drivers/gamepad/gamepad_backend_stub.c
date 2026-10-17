// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  No-pad backend for platforms without a reader yet.
 *
 * Desktop Linux lands here today. TODO(linux): an evdev backend —
 * `/dev/input/event*` with `EV_ABS` / `EV_KEY` over the `BTN_GAMEPAD`
 * family (and `udev` for hot-plug) — maps straight onto @ref gamepad_state;
 * the hub, the devices and the mapping table need no change for it. Until
 * then @ref gamepad_backend_available returns false and the provider declines
 * at probe, so a staged Linux manifest costs nothing.
 *
 * @ingroup drv_gamepad
 */

#include "xrt/xrt_config_os.h"

#if !defined(XRT_OS_WINDOWS) && !defined(XRT_OS_MACOS)

#include "gamepad_backend.h"

bool
gamepad_backend_available(void)
{
	return false;
}

bool
gamepad_backend_init(void)
{
	return false;
}

void
gamepad_backend_fini(void)
{}

void
gamepad_backend_poll(struct gamepad_state *out)
{
	out->connected = false;
	out->buttons = 0;
	out->left_x = out->left_y = out->right_x = out->right_y = 0.0f;
	out->left_trigger = out->right_trigger = 0.0f;
}

void
gamepad_backend_rumble(float low_frequency, float high_frequency)
{
	(void)low_frequency;
	(void)high_frequency;
}

const char *
gamepad_backend_name(void)
{
	return "stub (no pad reader on this platform; evdev TODO)";
}

#else
// Keep the translation unit non-empty where this backend is compiled out (-Wempty-translation-unit).
typedef int gamepad_backend_stub_not_built_here_t;
#endif // !XRT_OS_WINDOWS && !XRT_OS_MACOS
