// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Lightweight SR hardware probe — detects SR display presence
 *         and caches display properties for the Leia builder and device.
 *
 * The probe creates a temporary SR context, checks for an active display
 * via IDisplayManager::getPrimaryActiveSRDisplay(), and queries pixel
 * dimensions, refresh rate, physical size, and nominal viewing distance.
 *
 * Results are cached in file-scope statics so that later calls from
 * leia_hmd_create() do not need to re-create an SR context.
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_interface.h"
#include "util/u_logging.h"

#ifdef XRT_HAVE_LEIA_SR_D3D11

#include <sr/world/display/display.h>
#include <sr/utility/exception.h>

#include <windows.h>
#include <sysinfoapi.h>

/*
 * File-scope probe cache.
 */
static struct leiasr_probe_result g_probe = {};
static bool g_probe_done = false;

bool
leiasr_probe_display(double timeout_seconds)
{
	if (g_probe_done) {
		return g_probe.hw_found;
	}

	g_probe_done = true;
	g_probe.hw_found = false;

	const double start_time = (double)GetTickCount64() / 1000.0;

	// Create temporary SR context with retry loop.
	SR::SRContext *context = nullptr;
	while (context == nullptr) {
		try {
			context = SR::SRContext::create();
			break;
		} catch (SR::ServerNotAvailableException &e) {
			(void)e;
		}

		double cur_time = (double)GetTickCount64() / 1000.0;
		if ((cur_time - start_time) > timeout_seconds) {
			break;
		}
		Sleep(100);
	}

	if (context == nullptr) {
		U_LOG_I("SR probe: no SR context within %.1fs — no SR hardware", timeout_seconds);
		return false;
	}

	// Query primary active SR display.
	bool success = false;
	try {
		SR::IDisplayManager *dm = SR::GetDisplayManagerInstance(*context);
		if (dm == nullptr) {
			U_LOG_I("SR probe: no display manager");
			goto cleanup;
		}

		// Wait for display to become ready (within remaining time).
		while (true) {
			SR::IDisplay *display = dm->getPrimaryActiveSRDisplay();
			if (display != nullptr && display->isValid()) {
				SR_recti loc = display->getLocation();
				int64_t native_w = loc.right - loc.left;
				int64_t native_h = loc.bottom - loc.top;

				if (native_w > 0 && native_h > 0) {
					// Pixel dimensions.
					g_probe.pixel_w = static_cast<uint32_t>(native_w);
					g_probe.pixel_h = static_cast<uint32_t>(native_h);

					// Refresh rate via Win32.
					DEVMODEW devmode = {};
					devmode.dmSize = sizeof(devmode);
					if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &devmode) &&
					    devmode.dmDisplayFrequency > 1) {
						g_probe.refresh_hz = (float)devmode.dmDisplayFrequency;
					} else {
						g_probe.refresh_hz = 60.0f;
					}

					// Physical dimensions (SR returns centimeters).
					float width_cm = display->getPhysicalSizeWidth();
					float height_cm = display->getPhysicalSizeHeight();
					if (width_cm > 0.0f && height_cm > 0.0f) {
						g_probe.display_w_m = width_cm / 100.0f;
						g_probe.display_h_m = height_cm / 100.0f;
					} else {
						g_probe.display_w_m = 0.344f;
						g_probe.display_h_m = 0.194f;
					}

					// Nominal viewing distance (SR returns mm).
					float nom_x_mm = 0, nom_y_mm = 0, nom_z_mm = 0;
					try {
						display->getDefaultViewingPosition(nom_x_mm, nom_y_mm, nom_z_mm);
						g_probe.nominal_z_m = nom_z_mm / 1000.0f;
					} catch (...) {
						g_probe.nominal_z_m = 0.65f;
					}
					if (g_probe.nominal_z_m <= 0.0f) {
						g_probe.nominal_z_m = 0.65f;
					}

					g_probe.hw_found = true;
					success = true;

					U_LOG_I("SR probe: found %ux%u @ %.0f Hz, %.3fx%.3f m, Z=%.2f m",
					        g_probe.pixel_w, g_probe.pixel_h, g_probe.refresh_hz,
					        g_probe.display_w_m, g_probe.display_h_m, g_probe.nominal_z_m);
					break;
				}
			}

			double cur_time = (double)GetTickCount64() / 1000.0;
			if ((cur_time - start_time) > timeout_seconds) {
				break;
			}
			Sleep(100);
		}
	} catch (...) {
		U_LOG_I("SR probe: exception during display query");
	}

cleanup:
	if (context != nullptr) {
		SR::SRContext::deleteSRContext(context);
	}

	if (!success) {
		U_LOG_I("SR probe: no active SR display found within %.1fs", timeout_seconds);
	}

	return g_probe.hw_found;
}

bool
leiasr_get_probe_results(struct leiasr_probe_result *out)
{
	if (out == nullptr || !g_probe_done) {
		return false;
	}
	*out = g_probe;
	return g_probe.hw_found;
}

#else // !XRT_HAVE_LEIA_SR_D3D11

// Stub implementations for non-SR builds.

bool
leiasr_probe_display(double timeout_seconds)
{
	(void)timeout_seconds;
	return false;
}

bool
leiasr_get_probe_results(struct leiasr_probe_result *out)
{
	(void)out;
	return false;
}

#endif // XRT_HAVE_LEIA_SR_D3D11
