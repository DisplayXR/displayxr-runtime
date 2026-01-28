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

#include <mutex>
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

	// Configuration
	bool srgb_read = false;
	bool srgb_write = false;

	// Thread safety
	std::mutex mutex;
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

				U_LOG_W("SR D3D11 display (modern API): %ldx%ld px, physical %.2fcm x %.2fcm = %.4fm x %.4fm",
				        (long)width, (long)height,
				        raw_width_cm, raw_height_cm,
				        sr.display_width_m, sr.display_height_m);

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

	// Create D3D11 weaver
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

	// Initialize the context after creating the weaver
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

	// Destroy weaver first
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

	std::lock_guard<std::mutex> lock(leiasr->mutex);

	leiasr->input_srv = static_cast<ID3D11ShaderResourceView *>(stereo_srv);
	leiasr->view_width = view_width;
	leiasr->view_height = view_height;
	leiasr->input_format = static_cast<DXGI_FORMAT>(format);

	// Configure the weaver with the input texture
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

	std::lock_guard<std::mutex> lock(leiasr->mutex);

	// The weaver writes to the currently bound render target
	// Make sure OMSetRenderTargets and RSSetViewports have been called
	leiasr->weaver->weave();
}

bool
leiasr_d3d11_get_predicted_eye_positions(struct leiasr_d3d11 *leiasr,
                                         float out_left_eye[3],
                                         float out_right_eye[3])
{
	if (leiasr == nullptr || leiasr->weaver == nullptr) {
		return false;
	}

	std::lock_guard<std::mutex> lock(leiasr->mutex);

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

	// Throttled diagnostic logging
	static int eye_log_counter = 0;
	if (++eye_log_counter % 300 == 1) { // Log every ~5 seconds at 60fps
		float ipd_mm = sqrtf(powf(right_mm[0] - left_mm[0], 2) +
		                     powf(right_mm[1] - left_mm[1], 2) +
		                     powf(right_mm[2] - left_mm[2], 2));
		U_LOG_W("SR eye positions (raw mm): L=(%.1f,%.1f,%.1f) R=(%.1f,%.1f,%.1f) IPD=%.1fmm",
		        left_mm[0], left_mm[1], left_mm[2],
		        right_mm[0], right_mm[1], right_mm[2],
		        ipd_mm);
	}

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

	std::lock_guard<std::mutex> lock(leiasr->mutex);

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

	std::lock_guard<std::mutex> lock(leiasr->mutex);

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

	std::lock_guard<std::mutex> lock(leiasr->mutex);

	if (!leiasr->display_dims_valid) {
		out_dims->valid = false;
		return false;
	}

	out_dims->width_m = leiasr->display_width_m;
	out_dims->height_m = leiasr->display_height_m;
	out_dims->valid = true;

	return true;
}

} // extern "C"
