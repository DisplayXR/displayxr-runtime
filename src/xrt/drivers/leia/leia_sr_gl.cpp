// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Leia SR OpenGL weaver wrapper implementation.
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_sr_gl.h"
#include "util/u_logging.h"
#include "os/os_time.h"

#include <sr/weaver/glweaver.h>
#include <sr/world/display/display.h>
#include <sr/sense/display/switchablehint.h>
#include <sr/utility/exception.h>

#include <windows.h>
#include <sysinfoapi.h>

#include <cmath>

/*!
 * GL SR weaver instance.
 */
struct leiasr_gl
{
	// SR SDK objects
	SR::SRContext *context = nullptr;
	SR::IGLWeaver1 *weaver = nullptr;
	SR::SwitchableLensHint *lens_hint = nullptr;

	// Current input texture info
	uint32_t input_texture = 0;
	uint32_t view_width = 0;
	uint32_t view_height = 0;
	uint32_t input_format = 0; // GL_RGBA8

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
};

namespace {

/*!
 * Create and wait for the SR context to become ready.
 */
bool
create_sr_context(double max_time, leiasr_gl &sr)
{
	const double start_time = (double)GetTickCount64() / 1000.0;

	// Create SR context.
	while (sr.context == nullptr) {
		try {
			sr.context = SR::SRContext::create();
			break;
		} catch (SR::ServerNotAvailableException &e) {
			(void)e;
		}

		U_LOG_D("Waiting for SR context (GL)...");
		Sleep(100);

		double cur_time = (double)GetTickCount64() / 1000.0;
		if ((cur_time - start_time) > max_time) {
			break;
		}
	}

	if (sr.context == nullptr) {
		U_LOG_E("Failed to create SR context (GL) within %.1f seconds", max_time);
		return false;
	}

	// Get display manager and wait for display to be ready.
	SR::IDisplayManager *displayManager = nullptr;
	SR::IDisplay *display = nullptr;
	bool display_ready = false;

	try {
		displayManager = SR::GetDisplayManagerInstance(*sr.context);
		if (displayManager == nullptr) {
			U_LOG_E("Failed to get SR DisplayManager instance (GL)");
			return false;
		}
	} catch (...) {
		U_LOG_E("Exception getting SR DisplayManager (GL) - requires runtime 1.34.8-RC1+");
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

				float raw_width_cm = display->getPhysicalSizeWidth();
				float raw_height_cm = display->getPhysicalSizeHeight();
				sr.display_width_m = raw_width_cm / 100.0f;
				sr.display_height_m = raw_height_cm / 100.0f;
				sr.display_dims_valid = true;

				sr.display_pixel_width = static_cast<uint32_t>(width);
				sr.display_pixel_height = static_cast<uint32_t>(height);
				sr.display_screen_left = static_cast<int32_t>(display_location.left);
				sr.display_screen_top = static_cast<int32_t>(display_location.top);
				sr.display_pixel_dims_valid = true;

				U_LOG_W("SR GL display: %ldx%ld px, physical %.2fcm x %.2fcm = %.4fm x %.4fm",
				        (long)width, (long)height,
				        raw_width_cm, raw_height_cm,
				        sr.display_width_m, sr.display_height_m);

				break;
			}
		}

		U_LOG_D("Waiting for SR display (GL)...");
		Sleep(100);

		double cur_time = (double)GetTickCount64() / 1000.0;
		if ((cur_time - start_time) > max_time) {
			break;
		}
	}

	if (!display_ready) {
		U_LOG_E("SR display not ready (GL) within %.1f seconds", max_time);
		return false;
	}

	// Create SwitchableLensHint for 2D/3D mode switching
	try {
		sr.lens_hint = SR::SwitchableLensHint::create(*sr.context);
		U_LOG_W("SR GL SwitchableLensHint created successfully");
	} catch (...) {
		sr.lens_hint = nullptr;
		U_LOG_W("SR GL SwitchableLensHint not available on this display");
	}

	return true;
}

} // namespace

extern "C" {

xrt_result_t
leiasr_gl_create(double max_time,
                  void *hwnd,
                  uint32_t view_width,
                  uint32_t view_height,
                  struct leiasr_gl **out)
{
	leiasr_gl *sr = new leiasr_gl;
	sr->view_width = view_width;
	sr->view_height = view_height;

	// Create SR context
	if (!create_sr_context(max_time, *sr)) {
		delete sr;
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	// Create GL weaver
	WeaverErrorCode result = SR::CreateGLWeaver(*sr->context,
	                                             static_cast<HWND>(hwnd),
	                                             &sr->weaver);
	if (result != WeaverErrorCode::WeaverSuccess) {
		U_LOG_E("Failed to create SR GL weaver: %d", (int)result);
		SR::SRContext::deleteSRContext(sr->context);
		delete sr;
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	// Initialize the context after creating the weaver.
	sr->context->initialize();

	// Set default latency (1 frame)
	sr->weaver->setLatencyInFrames(1);

	*out = sr;

	U_LOG_I("Created GL SR weaver for HWND %p, view size %ux%u", hwnd, view_width, view_height);

	return XRT_SUCCESS;
}

void
leiasr_gl_destroy(struct leiasr_gl **leiasr_ptr)
{
	if (leiasr_ptr == nullptr || *leiasr_ptr == nullptr) {
		return;
	}

	leiasr_gl *sr = *leiasr_ptr;

	// SwitchableLensHint is managed by SRContext — do NOT delete it manually.
	sr->lens_hint = nullptr;

	// Destroy weaver
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

	U_LOG_I("Destroyed GL SR weaver");
}

void
leiasr_gl_set_input_texture(struct leiasr_gl *leiasr,
                             uint32_t stereo_texture,
                             uint32_t view_width,
                             uint32_t view_height,
                             uint32_t format)
{
	if (leiasr == nullptr || leiasr->weaver == nullptr) {
		return;
	}

	static uint32_t last_logged_width = 0, last_logged_height = 0;
	if (view_width != last_logged_width || view_height != last_logged_height) {
		U_LOG_I("SR GL weaver setInputViewTexture: view=%ux%u", view_width, view_height);
		last_logged_width = view_width;
		last_logged_height = view_height;
	}

	leiasr->input_texture = stereo_texture;
	leiasr->view_width = view_width;
	leiasr->view_height = view_height;
	leiasr->input_format = format;

	// Configure the weaver with the input texture
	leiasr->weaver->setInputViewTexture(static_cast<int>(stereo_texture),
	                                     static_cast<int>(view_width),
	                                     static_cast<int>(view_height),
	                                     static_cast<int>(format));
}

void
leiasr_gl_weave(struct leiasr_gl *leiasr)
{
	if (leiasr == nullptr || leiasr->weaver == nullptr) {
		U_LOG_W("leiasr_gl_weave called with null instance or weaver");
		return;
	}

	// The weaver writes to the currently bound framebuffer
	leiasr->weaver->weave();
}

bool
leiasr_gl_get_predicted_eye_positions(struct leiasr_gl *leiasr,
                                       float out_left_eye[3],
                                       float out_right_eye[3])
{
	if (leiasr == nullptr || leiasr->weaver == nullptr) {
		return false;
	}

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

bool
leiasr_gl_get_display_dimensions(struct leiasr_gl *leiasr, struct leiasr_display_dimensions *out_dims)
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
leiasr_gl_get_display_pixel_info(struct leiasr_gl *leiasr,
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
leiasr_gl_request_display_mode(struct leiasr_gl *leiasr, bool enable_3d)
{
	if (leiasr == nullptr || leiasr->lens_hint == nullptr) {
		return false;
	}

	try {
		if (enable_3d) {
			leiasr->lens_hint->enable();
		} else {
			leiasr->lens_hint->disable();
		}
		U_LOG_W("SR GL display mode switched to %s", enable_3d ? "3D" : "2D");
		return true;
	} catch (...) {
		U_LOG_E("Failed to switch SR GL display mode to %s", enable_3d ? "3D" : "2D");
		return false;
	}
}

bool
leiasr_gl_get_hardware_3d_state(struct leiasr_gl *leiasr, bool *out_is_3d)
{
	if (leiasr == nullptr || leiasr->lens_hint == nullptr || out_is_3d == nullptr) {
		return false;
	}
	try {
		*out_is_3d = leiasr->lens_hint->isEnabled();
		return true;
	} catch (...) {
		return false;
	}
}

} // extern "C"
