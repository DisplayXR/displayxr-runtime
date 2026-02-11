// Copyright 2024-2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Leia SR D3D11 weaver wrapper implementation.
 * @author David Fattal
 * @ingroup drv_leiasr
 */

#include "leiasr_d3d11.h"
#include "util/u_logging.h"
#include "os/os_time.h"

#include <sr/weaver/dx11weaver.h>
#include <sr/world/display/display.h>
#include <sr/utility/exception.h>

#include <d3d11.h>

#include <windows.h>
#include <sysinfoapi.h>

#include <cmath>

/*!
 * D3D11 SR weaver instance.
 */
struct leiasr_d3d11
{
	// SR SDK objects
	SR::SRContext *context = nullptr;
	SR::IDX11Weaver1 *weaver = nullptr;

	// D3D11 resources (references, not owned)
	ID3D11Device *device = nullptr;
	ID3D11DeviceContext *d3d11_context = nullptr;

	// Current input texture info
	ID3D11ShaderResourceView *input_srv = nullptr;
	uint32_t view_width = 0;
	uint32_t view_height = 0;
	DXGI_FORMAT input_format = DXGI_FORMAT_R8G8B8A8_UNORM;

	// Display dimensions in meters (for Kooima FOV calculation)
	float display_width_m = 0.0f;
	float display_height_m = 0.0f;
	bool display_dims_valid = false;

	// Display pixel resolution and screen position (for window metrics)
	uint32_t display_pixel_width = 0;
	uint32_t display_pixel_height = 0;
	int32_t display_screen_left = 0;
	int32_t display_screen_top = 0;
	bool display_pixel_dims_valid = false;

	// Recommended view texture dimensions from SR display
	uint32_t recommended_view_width = 0;
	uint32_t recommended_view_height = 0;
	bool recommended_dims_valid = false;

	// Configuration
	bool srgb_read = false;
	bool srgb_write = false;
};

namespace {

/*!
 * Create and wait for the SR context to become ready.
 */
bool
create_sr_context(double max_time, leiasr_d3d11 &sr)
{
	const double start_time = (double)GetTickCount64() / 1000.0;

	// Create SR context.
	while (sr.context == nullptr) {
		try {
			sr.context = SR::SRContext::create();
			break;
		} catch (SR::ServerNotAvailableException &e) {
			// Ignore errors because SR may be starting-up.
			(void)e;
		}

		U_LOG_D("Waiting for SR context...");

		// Wait a bit.
		Sleep(100);

		// Abort if we exceed the maximum allowed time.
		double cur_time = (double)GetTickCount64() / 1000.0;
		if ((cur_time - start_time) > max_time) {
			break;
		}
	}

	if (sr.context == nullptr) {
		U_LOG_E("Failed to create SR context within %.1f seconds", max_time);
		return false;
	}

	// Get display manager (modern API) and wait for display to be ready.
	SR::IDisplayManager *displayManager = nullptr;
	SR::IDisplay *display = nullptr;
	bool display_ready = false;

	try {
		displayManager = SR::GetDisplayManagerInstance(*sr.context);
		if (displayManager == nullptr) {
			U_LOG_E("Failed to get SR DisplayManager instance");
			return false;
		}
	} catch (...) {
		U_LOG_E("Exception getting SR DisplayManager - requires runtime version 1.34.8-RC1 or later");
		return false;
	}

	while (!display_ready) {
		display = displayManager->getPrimaryActiveSRDisplay();
		if (display != nullptr && display->isValid()) {
			SR_recti display_location = display->getLocation();
			int64_t width = display_location.right - display_location.left;
			int64_t height = display_location.bottom - display_location.top;
			if ((width != 0) && (height != 0)) {
				display_ready = true;

				// Cache display dimensions in meters for Kooima FOV calculation
				// Use SR SDK's physical size API (returns cm, convert to meters)
				float raw_width_cm = display->getPhysicalSizeWidth();
				float raw_height_cm = display->getPhysicalSizeHeight();
				sr.display_width_m = raw_width_cm / 100.0f;
				sr.display_height_m = raw_height_cm / 100.0f;
				sr.display_dims_valid = true;

				// Cache display pixel resolution and screen position
				sr.display_pixel_width = static_cast<uint32_t>(width);
				sr.display_pixel_height = static_cast<uint32_t>(height);
				sr.display_screen_left = static_cast<int32_t>(display_location.left);
				sr.display_screen_top = static_cast<int32_t>(display_location.top);
				sr.display_pixel_dims_valid = true;

				// Cache recommended view texture dimensions from SR display
				sr.recommended_view_width = display->getRecommendedViewsTextureWidth();
				sr.recommended_view_height = display->getRecommendedViewsTextureHeight();
				sr.recommended_dims_valid = (sr.recommended_view_width > 0 && sr.recommended_view_height > 0);

				U_LOG_W("SR D3D11 display (modern API): %ldx%ld px, physical %.2fcm x %.2fcm = %.4fm x %.4fm",
				        (long)width, (long)height,
				        raw_width_cm, raw_height_cm,
				        sr.display_width_m, sr.display_height_m);
				U_LOG_W("SR recommended view texture: %ux%u per eye",
				        sr.recommended_view_width, sr.recommended_view_height);

				break;
			}
		}

		U_LOG_D("Waiting for SR display...");

		Sleep(100);

		double cur_time = (double)GetTickCount64() / 1000.0;
		if ((cur_time - start_time) > max_time) {
			break;
		}
	}

	if (!display_ready) {
		U_LOG_E("SR display not ready within %.1f seconds", max_time);
		return false;
	}

	return true;
}

} // namespace

extern "C" {

xrt_result_t
leiasr_d3d11_create(double max_time,
                    void *d3d11_device,
                    void *d3d11_context,
                    void *hwnd,
                    uint32_t view_width,
                    uint32_t view_height,
                    struct leiasr_d3d11 **out)
{
	if (d3d11_device == nullptr || d3d11_context == nullptr) {
		U_LOG_E("D3D11 device or context is null");
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	leiasr_d3d11 *sr = new leiasr_d3d11;
	sr->device = static_cast<ID3D11Device *>(d3d11_device);
	sr->d3d11_context = static_cast<ID3D11DeviceContext *>(d3d11_context);
	sr->view_width = view_width;
	sr->view_height = view_height;

	// Create SR context
	if (!create_sr_context(max_time, *sr)) {
		delete sr;
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	// Create D3D11 weaver (SR SDK installs its WndProc via SetWindowLongPtr)
	WeaverErrorCode result = SR::CreateDX11Weaver(sr->context,
	                                               sr->d3d11_context,
	                                               static_cast<HWND>(hwnd),
	                                               &sr->weaver);
	if (result != WeaverErrorCode::WeaverSuccess) {
		U_LOG_E("Failed to create SR D3D11 weaver: %d", (int)result);
		delete sr->context;
		delete sr;
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	// Initialize the context after creating the weaver.
	sr->context->initialize();

	// Set default latency (1 frame)
	sr->weaver->setLatencyInFrames(1);

	*out = sr;

	U_LOG_I("Created D3D11 SR weaver for HWND %p, view size %ux%u", hwnd, view_width, view_height);

	return XRT_SUCCESS;
}

void
leiasr_d3d11_destroy(struct leiasr_d3d11 **leiasr_ptr)
{
	if (leiasr_ptr == nullptr || *leiasr_ptr == nullptr) {
		return;
	}

	leiasr_d3d11 *sr = *leiasr_ptr;

	// Destroy weaver (SR SDK restores the app's original WndProc)
	if (sr->weaver != nullptr) {
		sr->weaver->destroy();
		sr->weaver = nullptr;
	}

	// Destroy context
	if (sr->context != nullptr) {
		SR::SRContext::deleteSRContext(sr->context);
		sr->context = nullptr;
	}

	delete sr;
	*leiasr_ptr = nullptr;

	U_LOG_I("Destroyed D3D11 SR weaver");
}

void
leiasr_d3d11_set_input_texture(struct leiasr_d3d11 *leiasr,
                               void *stereo_srv,
                               uint32_t view_width,
                               uint32_t view_height,
                               uint32_t format)
{
	if (leiasr == nullptr || leiasr->weaver == nullptr) {
		return;
	}

	// Log dimension changes (first time or when dimensions change)
	static uint32_t last_logged_width = 0, last_logged_height = 0;
	if (view_width != last_logged_width || view_height != last_logged_height) {
		U_LOG_I("SR weaver setInputViewTexture: view=%ux%u (expects side-by-side stereo SRV)",
		        view_width, view_height);
		last_logged_width = view_width;
		last_logged_height = view_height;
	}

	leiasr->input_srv = static_cast<ID3D11ShaderResourceView *>(stereo_srv);
	leiasr->view_width = view_width;
	leiasr->view_height = view_height;
	leiasr->input_format = static_cast<DXGI_FORMAT>(format);

	// Configure the weaver with the input texture
	// NOTE: view_width is single-eye width; SR SDK handles the side-by-side stereo layout internally
	leiasr->weaver->setInputViewTexture(leiasr->input_srv,
	                                     static_cast<int>(view_width),
	                                     static_cast<int>(view_height),
	                                     leiasr->input_format);
}

void
leiasr_d3d11_weave(struct leiasr_d3d11 *leiasr)
{
	if (leiasr == nullptr || leiasr->weaver == nullptr) {
		U_LOG_W("leiasr_d3d11_weave called with null instance or weaver");
		return;
	}

	// The weaver writes to the currently bound render target
	// Make sure OMSetRenderTargets and RSSetViewports have been called
	leiasr->weaver->weave();
}

/*!
 * Check if weaver's HWND is still valid (for debugging "window handle is invalid" errors).
 */
bool
leiasr_d3d11_check_window_valid(struct leiasr_d3d11 *leiasr, void *hwnd)
{
	if (leiasr == nullptr) {
		return false;
	}

	HWND h = static_cast<HWND>(hwnd);
	if (h == nullptr) {
		U_LOG_W("leiasr_d3d11: HWND is null");
		return false;
	}

	if (!IsWindow(h)) {
		U_LOG_W("leiasr_d3d11: HWND %p is not a valid window", h);
		return false;
	}

	// Check if window is visible
	if (!IsWindowVisible(h)) {
		static bool warned_invisible = false;
		if (!warned_invisible) {
			U_LOG_W("leiasr_d3d11: Window %p is not visible", h);
			warned_invisible = true;
		}
	}

	// Get window position to check if it's on a valid monitor
	RECT rect;
	if (GetWindowRect(h, &rect)) {
		HMONITOR monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONULL);
		if (monitor == nullptr) {
			U_LOG_W("leiasr_d3d11: Window %p is not on any monitor (rect: %ld,%ld-%ld,%ld)",
			        h, rect.left, rect.top, rect.right, rect.bottom);
			return false;
		}
	}

	return true;
}

bool
leiasr_d3d11_get_predicted_eye_positions(struct leiasr_d3d11 *leiasr,
                                         float out_left_eye[3],
                                         float out_right_eye[3])
{
	if (leiasr == nullptr || leiasr->weaver == nullptr) {
		return false;
	}

	// Get positions in millimeters from weaver
	float left_mm[3], right_mm[3];
	leiasr->weaver->getPredictedEyePositions(left_mm, right_mm);

	// Convert to meters
	out_left_eye[0] = left_mm[0] / 1000.0f;
	out_left_eye[1] = left_mm[1] / 1000.0f;
	out_left_eye[2] = left_mm[2] / 1000.0f;
	out_right_eye[0] = right_mm[0] / 1000.0f;
	out_right_eye[1] = right_mm[1] / 1000.0f;
	out_right_eye[2] = right_mm[2] / 1000.0f;

	return true;
}

void
leiasr_d3d11_set_srgb_conversion(struct leiasr_d3d11 *leiasr,
                                 bool read_srgb,
                                 bool write_srgb)
{
	if (leiasr == nullptr || leiasr->weaver == nullptr) {
		return;
	}

	leiasr->srgb_read = read_srgb;
	leiasr->srgb_write = write_srgb;
	leiasr->weaver->setShaderSRGBConversion(read_srgb, write_srgb);
}

void
leiasr_d3d11_set_latency_in_frames(struct leiasr_d3d11 *leiasr,
                                   uint64_t latency_frames)
{
	if (leiasr == nullptr || leiasr->weaver == nullptr) {
		return;
	}

	leiasr->weaver->setLatencyInFrames(latency_frames);
}

bool
leiasr_d3d11_is_ready(struct leiasr_d3d11 *leiasr)
{
	if (leiasr == nullptr) {
		return false;
	}

	return leiasr->weaver != nullptr && leiasr->context != nullptr;
}

bool
leiasr_d3d11_get_display_dimensions(struct leiasr_d3d11 *leiasr, struct leiasr_display_dimensions *out_dims)
{
	if (leiasr == nullptr || out_dims == nullptr) {
		if (out_dims != nullptr) {
			out_dims->valid = false;
		}
		return false;
	}

	if (!leiasr->display_dims_valid) {
		out_dims->valid = false;
		return false;
	}

	out_dims->width_m = leiasr->display_width_m;
	out_dims->height_m = leiasr->display_height_m;
	out_dims->valid = true;

	return true;
}

bool
leiasr_d3d11_get_display_pixel_info(struct leiasr_d3d11 *leiasr,
                                     uint32_t *out_display_pixel_width,
                                     uint32_t *out_display_pixel_height,
                                     int32_t *out_display_screen_left,
                                     int32_t *out_display_screen_top,
                                     float *out_display_width_m,
                                     float *out_display_height_m)
{
	if (leiasr == nullptr || out_display_pixel_width == nullptr ||
	    out_display_pixel_height == nullptr || out_display_screen_left == nullptr ||
	    out_display_screen_top == nullptr || out_display_width_m == nullptr ||
	    out_display_height_m == nullptr) {
		return false;
	}

	if (!leiasr->display_pixel_dims_valid || !leiasr->display_dims_valid) {
		return false;
	}

	*out_display_pixel_width = leiasr->display_pixel_width;
	*out_display_pixel_height = leiasr->display_pixel_height;
	*out_display_screen_left = leiasr->display_screen_left;
	*out_display_screen_top = leiasr->display_screen_top;
	*out_display_width_m = leiasr->display_width_m;
	*out_display_height_m = leiasr->display_height_m;

	return true;
}

bool
leiasr_d3d11_get_recommended_view_dimensions(struct leiasr_d3d11 *leiasr,
                                              uint32_t *out_width,
                                              uint32_t *out_height)
{
	if (leiasr == nullptr || out_width == nullptr || out_height == nullptr) {
		return false;
	}

	if (!leiasr->recommended_dims_valid) {
		return false;
	}

	*out_width = leiasr->recommended_view_width;
	*out_height = leiasr->recommended_view_height;

	return true;
}

bool
leiasr_query_recommended_view_dimensions(double max_time,
                                          uint32_t *out_width,
                                          uint32_t *out_height,
                                          float *out_refresh_rate_hz,
                                          uint32_t *out_native_width,
                                          uint32_t *out_native_height)
{
	if (out_width == nullptr || out_height == nullptr) {
		return false;
	}

	const double start_time = (double)GetTickCount64() / 1000.0;

	// Create temporary SR context
	SR::SRContext *context = nullptr;
	while (context == nullptr) {
		try {
			context = SR::SRContext::create();
			break;
		} catch (SR::ServerNotAvailableException &e) {
			(void)e;
		}

		U_LOG_D("Waiting for SR context (dimension query)...");
		Sleep(100);

		double cur_time = (double)GetTickCount64() / 1000.0;
		if ((cur_time - start_time) > max_time) {
			break;
		}
	}

	if (context == nullptr) {
		U_LOG_E("Failed to create SR context for dimension query within %.1f seconds", max_time);
		return false;
	}

	// Get display manager and query dimensions
	bool success = false;
	try {
		SR::IDisplayManager *displayManager = SR::GetDisplayManagerInstance(*context);
		if (displayManager != nullptr) {
			// Wait for display to be ready
			while (!success) {
				SR::IDisplay *display = displayManager->getPrimaryActiveSRDisplay();
				if (display != nullptr && display->isValid()) {
					SR_recti display_location = display->getLocation();
					int64_t native_width = display_location.right - display_location.left;
					int64_t native_height = display_location.bottom - display_location.top;
					if ((native_width != 0) && (native_height != 0)) {
						*out_width = display->getRecommendedViewsTextureWidth();
						*out_height = display->getRecommendedViewsTextureHeight();
						success = (*out_width > 0 && *out_height > 0);
						if (success) {
							U_LOG_I("SR query: recommended view dimensions %ux%u per eye",
							        *out_width, *out_height);

							// Return native display dimensions if requested
							if (out_native_width != nullptr) {
								*out_native_width = static_cast<uint32_t>(native_width);
							}
							if (out_native_height != nullptr) {
								*out_native_height = static_cast<uint32_t>(native_height);
							}
							U_LOG_I("SR query: native display dimensions %ux%u",
							        (uint32_t)native_width, (uint32_t)native_height);

							// Query monitor refresh rate via Win32
							if (out_refresh_rate_hz != nullptr) {
								DEVMODEW dm = {};
								dm.dmSize = sizeof(dm);
								if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) &&
								    dm.dmDisplayFrequency > 1) {
									*out_refresh_rate_hz = (float)dm.dmDisplayFrequency;
									U_LOG_I("SR query: display refresh rate %.0f Hz",
									        *out_refresh_rate_hz);
								} else {
									*out_refresh_rate_hz = 60.0f;
									U_LOG_W("Could not query display refresh rate, defaulting to 60 Hz");
								}
							}
						}
						break;
					}
				}

				Sleep(100);

				double cur_time = (double)GetTickCount64() / 1000.0;
				if ((cur_time - start_time) > max_time) {
					break;
				}
			}
		}
	} catch (...) {
		U_LOG_E("Exception querying SR display dimensions");
	}

	// Clean up temporary context
	SR::SRContext::deleteSRContext(context);

	if (!success) {
		U_LOG_E("Failed to query SR recommended dimensions within %.1f seconds", max_time);
	}

	return success;
}

// Cached display dimensions for static queries
static float g_cached_display_width_m = 0.0f;
static float g_cached_display_height_m = 0.0f;
static bool g_display_dims_cached = false;

bool
leiasr_static_get_predicted_eye_positions(float out_left_eye[3],
                                          float out_right_eye[3])
{
	if (out_left_eye == nullptr || out_right_eye == nullptr) {
		return false;
	}

	// Eye position prediction requires an active weaver instance because:
	// 1. The weaver owns the LookaroundFilter that provides prediction
	// 2. Prediction is tuned to each application's update rate
	// Without a weaver, we cannot provide accurate predicted eye positions.
	// Callers should use leiasr_d3d11_get_predicted_eye_positions() with
	// their per-session weaver instance instead.

	// Return default center position for graceful fallback
	// This allows apps to start rendering before eye tracking is ready
	out_left_eye[0] = -0.032f;  // -32mm left of center
	out_left_eye[1] = 0.0f;
	out_left_eye[2] = 0.6f;     // 600mm from display

	out_right_eye[0] = 0.032f;  // +32mm right of center
	out_right_eye[1] = 0.0f;
	out_right_eye[2] = 0.6f;    // 600mm from display

	// Return false to indicate these are fallback values, not tracked
	return false;
}

bool
leiasr_static_get_display_dimensions(struct leiasr_display_dimensions *out_dims)
{
	if (out_dims == nullptr) {
		return false;
	}

	// Return cached values if available
	if (g_display_dims_cached) {
		out_dims->width_m = g_cached_display_width_m;
		out_dims->height_m = g_cached_display_height_m;
		out_dims->valid = true;
		return true;
	}

	// Need to query from SR SDK
	// Create temporary context
	SR::SRContext *context = nullptr;

	try {
		context = SR::SRContext::create();
	} catch (...) {
		return false;
	}

	if (context == nullptr) {
		return false;
	}

	bool success = false;
	try {
		SR::IDisplayManager *displayManager = SR::GetDisplayManagerInstance(*context);
		if (displayManager != nullptr) {
			SR::IDisplay *display = displayManager->getPrimaryActiveSRDisplay();
			if (display != nullptr && display->isValid()) {
				// Get physical dimensions using SR SDK's physical size API
				// Returns centimeters, convert to meters
				float raw_width_cm = display->getPhysicalSizeWidth();
				float raw_height_cm = display->getPhysicalSizeHeight();

				if (raw_width_cm > 0.0f && raw_height_cm > 0.0f) {
					g_cached_display_width_m = raw_width_cm / 100.0f;
					g_cached_display_height_m = raw_height_cm / 100.0f;
					g_display_dims_cached = true;

					out_dims->width_m = g_cached_display_width_m;
					out_dims->height_m = g_cached_display_height_m;
					out_dims->valid = true;
					success = true;

					U_LOG_W("Static display dimensions: %.2fcm x %.2fcm = %.4fm x %.4fm",
					        raw_width_cm, raw_height_cm,
					        g_cached_display_width_m, g_cached_display_height_m);
				}
			}
		}
	} catch (...) {
		U_LOG_E("Exception querying static display dimensions");
	}

	// Clean up temporary context
	if (context != nullptr) {
		SR::SRContext::deleteSRContext(context);
	}

	return success;
}

} // extern "C"
