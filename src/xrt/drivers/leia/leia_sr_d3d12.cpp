// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Leia SR D3D12 weaver wrapper implementation.
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_sr_d3d12.h"
#include "util/u_logging.h"
#include "os/os_time.h"

#include <sr/weaver/dx12weaver.h>
#include <sr/world/display/display.h>
#include <sr/sense/display/switchablehint.h>
#include <sr/utility/exception.h>

#include <d3d12.h>

#include <windows.h>
#include <sysinfoapi.h>

#include <cmath>

/*!
 * D3D12 SR weaver instance.
 */
struct leiasr_d3d12
{
	// SR SDK objects
	SR::SRContext *context = nullptr;
	SR::IDX12Weaver1 *weaver = nullptr;
	SR::SwitchableLensHint *lens_hint = nullptr;

	// D3D12 resources (references, not owned)
	ID3D12Device *device = nullptr;
	ID3D12CommandQueue *command_queue = nullptr;

	// Current input texture info
	ID3D12Resource *input_resource = nullptr;
	uint32_t view_width = 0;
	uint32_t view_height = 0;
	DXGI_FORMAT input_format = DXGI_FORMAT_R8G8B8A8_UNORM;

	// Display dimensions in meters (for Kooima FOV calculation)
	float display_width_m = 0.0f;
	float display_height_m = 0.0f;
	bool display_dims_valid = false;

	// Display pixel resolution and screen position
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
create_sr_context(double max_time, leiasr_d3d12 &sr)
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

		U_LOG_D("Waiting for SR context (D3D12)...");
		Sleep(100);

		double cur_time = (double)GetTickCount64() / 1000.0;
		if ((cur_time - start_time) > max_time) {
			break;
		}
	}

	if (sr.context == nullptr) {
		U_LOG_E("Failed to create SR context within %.1f seconds", max_time);
		return false;
	}

	// Get display manager and wait for display to be ready.
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

				// Cache display dimensions in meters
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

				U_LOG_W("SR D3D12 display: %ldx%ld px, physical %.2fcm x %.2fcm = %.4fm x %.4fm",
				        (long)width, (long)height,
				        raw_width_cm, raw_height_cm,
				        sr.display_width_m, sr.display_height_m);

				break;
			}
		}

		U_LOG_D("Waiting for SR display (D3D12)...");
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

	// Create SwitchableLensHint for 2D/3D mode switching
	try {
		sr.lens_hint = SR::SwitchableLensHint::create(*sr.context);
		U_LOG_W("SR D3D12 SwitchableLensHint created successfully");
	} catch (...) {
		sr.lens_hint = nullptr;
		U_LOG_W("SR D3D12 SwitchableLensHint not available on this display");
	}

	return true;
}

} // namespace

extern "C" {

xrt_result_t
leiasr_d3d12_create(double max_time,
                    void *d3d12_device,
                    void *d3d12_command_queue,
                    void *hwnd,
                    uint32_t view_width,
                    uint32_t view_height,
                    struct leiasr_d3d12 **out)
{
	if (d3d12_device == nullptr) {
		U_LOG_E("D3D12 device is null");
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	leiasr_d3d12 *sr = new leiasr_d3d12;
	sr->device = static_cast<ID3D12Device *>(d3d12_device);
	sr->command_queue = static_cast<ID3D12CommandQueue *>(d3d12_command_queue);
	sr->view_width = view_width;
	sr->view_height = view_height;

	// Create SR context
	if (!create_sr_context(max_time, *sr)) {
		delete sr;
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	// Create D3D12 weaver — set DPI awareness so the SDK sees physical pixels
	// when it queries the HWND. See LeiaInc/LeiaSR@a8a9fb9 for the pattern.
	DPI_AWARENESS_CONTEXT oldDpiCtx =
	    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	WeaverErrorCode result = SR::CreateDX12Weaver(sr->context,
	                                               sr->device,
	                                               static_cast<HWND>(hwnd),
	                                               &sr->weaver);
	if (oldDpiCtx != NULL) {
		SetThreadDpiAwarenessContext(oldDpiCtx);
	}
	if (result != WeaverErrorCode::WeaverSuccess) {
		U_LOG_E("Failed to create SR D3D12 weaver: %d", (int)result);
		SR::SRContext::deleteSRContext(sr->context);
		delete sr;
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	// Initialize the context after creating the weaver.
	sr->context->initialize();

	// Set default latency (1 frame)
	sr->weaver->setLatencyInFrames(1);

	*out = sr;

	U_LOG_I("Created D3D12 SR weaver for HWND %p, view size %ux%u", hwnd, view_width, view_height);

	return XRT_SUCCESS;
}

void
leiasr_d3d12_destroy(struct leiasr_d3d12 **leiasr_ptr)
{
	if (leiasr_ptr == nullptr || *leiasr_ptr == nullptr) {
		return;
	}

	leiasr_d3d12 *sr = *leiasr_ptr;

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

	U_LOG_I("Destroyed D3D12 SR weaver");
}

void
leiasr_d3d12_set_output_format(struct leiasr_d3d12 *leiasr, uint32_t format)
{
	if (leiasr == nullptr || leiasr->weaver == nullptr) {
		return;
	}

	DXGI_FORMAT dxgi_format = static_cast<DXGI_FORMAT>(format);
	leiasr->weaver->setOutputFormat(dxgi_format);
	U_LOG_W("SR D3D12 weaver output format set to %u", (unsigned)format);
}

void
leiasr_d3d12_set_input_texture(struct leiasr_d3d12 *leiasr,
                               void *stereo_resource,
                               uint32_t view_width,
                               uint32_t view_height,
                               uint32_t format)
{
	if (leiasr == nullptr || leiasr->weaver == nullptr) {
		return;
	}

	// Skip if nothing changed — match reference pattern of calling setInputViewTexture once
	if (leiasr->input_resource == static_cast<ID3D12Resource *>(stereo_resource) &&
	    leiasr->view_width == view_width &&
	    leiasr->view_height == view_height &&
	    leiasr->input_format == static_cast<DXGI_FORMAT>(format)) {
		return;
	}

	U_LOG_I("SR D3D12 weaver setInputViewTexture: view=%ux%u", view_width, view_height);

	leiasr->input_resource = static_cast<ID3D12Resource *>(stereo_resource);
	leiasr->view_width = view_width;
	leiasr->view_height = view_height;
	leiasr->input_format = static_cast<DXGI_FORMAT>(format);

	// Configure the weaver with the input texture
	// SR SDK DX12 weaver takes ID3D12Resource*, width, height, format
	leiasr->weaver->setInputViewTexture(leiasr->input_resource,
	                                     static_cast<int>(view_width),
	                                     static_cast<int>(view_height),
	                                     leiasr->input_format);
}

// Records SR SDK weave commands onto `command_list` constrained to
// (viewport_x, viewport_y, viewport_width, viewport_height) of the bound RTV.
//
// Gotcha: the SR SDK D3D12 weaver's setViewport/setScissorRect APIs are used
// internally for phase calculation only — they do NOT call RSSetViewports or
// RSSetScissorRects on the cmd list. We must set both on the cmd list
// ourselves before weave(), or the woven output lands at the cmd list's
// default viewport (full RT) instead of the canvas sub-rect. The D3D11 path
// gets this for free because the SDK D3D11 weaver reads RSGetViewports from
// the immediate context. Removing the RSSetViewports/RSSetScissorRects calls
// below will reintroduce the canvas-subrect black-screen bug.
void
leiasr_d3d12_weave(struct leiasr_d3d12 *leiasr,
                   void *command_list,
                   int32_t viewport_x,
                   int32_t viewport_y,
                   uint32_t viewport_width,
                   uint32_t viewport_height)
{
	if (leiasr == nullptr || leiasr->weaver == nullptr) {
		U_LOG_W("leiasr_d3d12_weave called with null instance or weaver");
		return;
	}

	ID3D12GraphicsCommandList *cmd_list = static_cast<ID3D12GraphicsCommandList *>(command_list);

	// Diagnostic: log weave parameters periodically
	static uint32_t weave_counter = 0;
	bool weave_log = (weave_counter % 60 == 0);
	weave_counter++;
	if (weave_log) {
		U_LOG_I("SR D3D12 weave: cmd_list=%p, viewport=(%d,%d %ux%u), input=%p (%ux%u fmt=%u)",
		        (void *)cmd_list, viewport_x, viewport_y, viewport_width, viewport_height,
		        (void *)leiasr->input_resource,
		        leiasr->view_width, leiasr->view_height,
		        (unsigned)leiasr->input_format);
	}

	// Set command list for the weaver to record commands onto
	leiasr->weaver->setCommandList(cmd_list);

	// Set viewport — the SR SDK uses TopLeftX/Y in its phase calculation:
	//   xOffset = window_WeavingX + vpX
	D3D12_VIEWPORT viewport = {};
	viewport.TopLeftX = static_cast<float>(viewport_x);
	viewport.TopLeftY = static_cast<float>(viewport_y);
	viewport.Width = static_cast<float>(viewport_width);
	viewport.Height = static_cast<float>(viewport_height);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	leiasr->weaver->setViewport(viewport);

	// Set scissor rect to match viewport sub-rect
	D3D12_RECT scissor = {};
	scissor.left = static_cast<LONG>(viewport_x);
	scissor.top = static_cast<LONG>(viewport_y);
	scissor.right = static_cast<LONG>(viewport_x) + static_cast<LONG>(viewport_width);
	scissor.bottom = static_cast<LONG>(viewport_y) + static_cast<LONG>(viewport_height);
	leiasr->weaver->setScissorRect(scissor);

	// Also set viewport + scissor on the command list itself. The SR SDK
	// weaver records draw commands but does not call RSSetViewports/Scissor
	// on the cmd list — so without this, the canvas sub-rect is ignored
	// and the woven output lands at the cmd list's default viewport
	// (typically full target). D3D11 path gets this for free because
	// the DP sets RSSetViewports on the immediate context before weave().
	cmd_list->RSSetViewports(1, &viewport);
	cmd_list->RSSetScissorRects(1, &scissor);

	// Perform weaving — records draw commands onto the command list.
	// Set DPI awareness so any internal GetClientRect returns physical pixels.
	DPI_AWARENESS_CONTEXT oldDpiCtx =
	    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	leiasr->weaver->weave();
	if (oldDpiCtx != NULL) {
		SetThreadDpiAwarenessContext(oldDpiCtx);
	}
}

bool
leiasr_d3d12_get_predicted_eye_positions(struct leiasr_d3d12 *leiasr,
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

bool
leiasr_d3d12_get_display_dimensions(struct leiasr_d3d12 *leiasr,
                                    struct leiasr_display_dimensions *out_dims)
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
leiasr_d3d12_get_display_pixel_info(struct leiasr_d3d12 *leiasr,
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
leiasr_d3d12_request_display_mode(struct leiasr_d3d12 *leiasr, bool enable_3d)
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
		U_LOG_W("SR D3D12 display mode switched to %s", enable_3d ? "3D" : "2D");
		return true;
	} catch (...) {
		U_LOG_E("Failed to switch SR D3D12 display mode to %s", enable_3d ? "3D" : "2D");
		return false;
	}
}

bool
leiasr_d3d12_get_hardware_3d_state(struct leiasr_d3d12 *leiasr, bool *out_is_3d)
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
