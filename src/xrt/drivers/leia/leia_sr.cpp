// Copyright 2024, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Leia SR (Simulated Reality) SDK integration for weaving.
 * @ingroup drv_leia
 */

#include "leia_sr.h"
#include "util/u_misc.h"
#include "util/u_logging.h"
#include "os/os_time.h"

#include <sr/weaver/vkweaver.h>
#include <sr/world/display/display.h>
#include <sr/sense/display/switchablehint.h>
#include <sr/utility/exception.h>

#include <windows.h>
#include <sysinfoapi.h>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <iostream>

/*!
 * Main structure holding SR context and weaver state.
 */
struct leiasr
{
	// Vulkan resources
	VkDevice device = nullptr;
	VkPhysicalDevice physicalDevice = nullptr;
	VkQueue graphicsQueue = nullptr;
	VkCommandPool commandPool = nullptr;
	VkCommandBuffer commandBuffer = nullptr;

	// SR SDK objects
	SR::SRContext *context = nullptr;
	SR::IVulkanWeaver1 *weaver = nullptr;
	SR::SwitchableLensHint *lens_hint = nullptr;

	// Display dimensions in meters (for Kooima FOV calculation)
	float display_width_m = 0.0f;
	float display_height_m = 0.0f;
	bool display_dims_valid = false;

	// Display pixel resolution (for window metrics computation)
	uint32_t display_pixel_width = 0;
	uint32_t display_pixel_height = 0;
	bool display_pixel_dims_valid = false;

	// Display screen position (for diagnostic logging and window metrics)
	int32_t display_screen_left = 0;
	int32_t display_screen_top = 0;
	int32_t display_screen_right = 0;
	int32_t display_screen_bottom = 0;

	// Recommended view texture dimensions from SR display
	uint32_t recommended_view_width = 0;
	uint32_t recommended_view_height = 0;
	bool recommended_dims_valid = false;

	// Window handle (stored for window metrics queries)
	HWND windowHandle = nullptr;

	// Thread-safe eye position storage (for getPredictedEyePositions)
	std::mutex eyeMutex;
};

namespace {

/*!
 * Create and wait for the SR context to become ready.
 */
bool
CreateSRContext(double maxTime, leiasr &sr)
{
	const double startTime = (double)GetTickCount64() / 1000.0;

	// Create SR context.
	while (sr.context == nullptr) {
		try {
			sr.context = SR::SRContext::create();
			break;
		} catch (SR::ServerNotAvailableException e) {
			// Ignore errors because SR may be starting-up.
		}

		std::cout << "Waiting for context" << std::endl;

		// Wait a bit.
		Sleep(100);

		// Abort if we exceed the maximum allowed time.
		double curTime = (double)GetTickCount64() / 1000.0;
		if ((curTime - startTime) > maxTime)
			break;
	}

	// Get display manager (modern API) and wait for display to be ready.
	SR::IDisplayManager *displayManager = nullptr;
	SR::IDisplay *display = nullptr;
	bool displayReady = false;

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

	while (sr.context && !displayReady) {
		display = displayManager->getPrimaryActiveSRDisplay();
		if (display != nullptr && display->isValid()) {
			// Get the display location, and when it is valid, the device is ready.
			SR_recti displayLocation = display->getLocation();
			int64_t width = displayLocation.right - displayLocation.left;
			int64_t height = displayLocation.bottom - displayLocation.top;
			if ((width != 0) && (height != 0)) {
				displayReady = true;

				// Cache display dimensions in meters for Kooima FOV calculation
				// Use SR SDK's physical size API (returns cm, convert to meters)
				float raw_width_cm = display->getPhysicalSizeWidth();
				float raw_height_cm = display->getPhysicalSizeHeight();
				sr.display_width_m = raw_width_cm / 100.0f;
				sr.display_height_m = raw_height_cm / 100.0f;
				sr.display_dims_valid = true;

				// Cache display screen position for diagnostic logging
				sr.display_screen_left = (int32_t)displayLocation.left;
				sr.display_screen_top = (int32_t)displayLocation.top;
				sr.display_screen_right = (int32_t)displayLocation.right;
				sr.display_screen_bottom = (int32_t)displayLocation.bottom;

				// Cache display pixel resolution
				sr.display_pixel_width = static_cast<uint32_t>(width);
				sr.display_pixel_height = static_cast<uint32_t>(height);
				sr.display_pixel_dims_valid = true;

				// Cache recommended view texture dimensions
				sr.recommended_view_width = display->getRecommendedViewsTextureWidth();
				sr.recommended_view_height = display->getRecommendedViewsTextureHeight();
				sr.recommended_dims_valid = (sr.recommended_view_width > 0 && sr.recommended_view_height > 0);

				U_LOG_W("SR display (modern API): %ldx%ld px, physical %.2fcm x %.2fcm = %.4fm x %.4fm",
				        (long)width, (long)height,
				        raw_width_cm, raw_height_cm,
				        sr.display_width_m, sr.display_height_m);
				U_LOG_W("SR display screen position: left=%d top=%d right=%d bottom=%d",
				        sr.display_screen_left, sr.display_screen_top,
				        sr.display_screen_right, sr.display_screen_bottom);
				U_LOG_W("SR recommended view texture: %ux%u per eye",
				        sr.recommended_view_width, sr.recommended_view_height);

				break;
			}
		}

		std::cout << "Waiting for display" << std::endl;

		// Wait a bit.
		Sleep(100);

		// Abort if we exceed the maximum allowed time.
		double curTime = (double)GetTickCount64() / 1000.0;
		if ((curTime - startTime) > maxTime)
			break;
	}

	// Create SwitchableLensHint for 2D/3D mode switching
	if (sr.context != nullptr && displayReady) {
		try {
			sr.lens_hint = SR::SwitchableLensHint::create(*sr.context);
			U_LOG_W("SR SwitchableLensHint created successfully");
		} catch (...) {
			sr.lens_hint = nullptr;
			U_LOG_W("SR SwitchableLensHint not available on this display");
		}
	}

	// Return if we have a valid context and device is ready.
	return (sr.context != nullptr) && displayReady;
}

/*!
 * Create the SR Vulkan weaver.
 */
bool
CreateSRWeaver(SR::SRContext *context,
               VkDevice device,
               VkPhysicalDevice physicalDevice,
               VkQueue graphicsQueue,
               VkCommandPool commandPool,
               HWND hWnd,
               leiasr *out)
{
	// Create weaver.
	WeaverErrorCode createWeaverResult =
	    SR::CreateVulkanWeaver(*context, device, physicalDevice, graphicsQueue, commandPool, hWnd, &out->weaver);
	if (createWeaverResult != WeaverErrorCode::WeaverSuccess) {
		U_LOG_E("Failed to create SR Vulkan weaver: %d", (int)createWeaverResult);
		return false;
	}

	return true;
}

} // namespace anonymous

extern "C" {

xrt_result_t
leiasr_create(double maxTime,
              VkDevice device,
              VkPhysicalDevice physicalDevice,
              VkQueue graphicsQueue,
              VkCommandPool commandPool,
              void *windowHandle,
              struct leiasr **out)
{
	leiasr *sr = new leiasr;

	if (!CreateSRContext(maxTime, *sr)) {
		U_LOG_E("Failed to create SR context");
		delete sr;
		return XRT_ERROR_VULKAN;
	}

	{
		VkCommandBufferAllocateInfo allocInfo = {};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandPool = commandPool;
		allocInfo.commandBufferCount = 1;
		vkAllocateCommandBuffers(device, &allocInfo, &sr->commandBuffer);
	}

	sr->physicalDevice = physicalDevice;
	sr->commandPool = commandPool;
	sr->graphicsQueue = graphicsQueue;
	sr->device = device;
	sr->windowHandle = (HWND)windowHandle;

	// Pass the real HWND to CreateVulkanWeaver.
	// The weaver does NOT create its own VkSwapchain — the compositor owns
	// the entire presentation pipeline (swapchain, framebuffers, present).
	// The HWND is used only for monitor detection, draw-region calculation,
	// and latency tracking. The weaver receives pre-created graphics objects
	// (command buffer, framebuffer, image views) via setCommandBuffer /
	// setOutputFrameBuffer / setInputViewTexture and records interlacing
	// commands into them.
	HWND weaverHwnd = (HWND)windowHandle;
	if (!CreateSRWeaver(sr->context, device, physicalDevice, graphicsQueue, commandPool, weaverHwnd, sr)) {
		U_LOG_E("Failed to create SR weaver");
		delete sr;
		return XRT_ERROR_VULKAN;
	}

	sr->context->initialize();

	*out = sr;

	U_LOG_W("Created leiasr instance with weaver for HWND %p", windowHandle);

	return XRT_SUCCESS;
}

void
leiasr_destroy(struct leiasr *leiasr)
{
	if (leiasr == nullptr) {
		return;
	}

	U_LOG_I("leiasr_destroy: beginning cleanup");

	// WORKAROUND for SR SDK race condition in WndProcDispatcher:
	// The SR SDK's WeaverBaseImpl has a use-after-free bug where it releases
	// the lock before dereferencing the instance pointer in WndProcDispatcher.
	// This can cause crashes when window messages (especially mouse movement)
	// arrive during weaver destruction.
	//
	// Mitigation 1: Pump all pending window messages before destroying the weaver
	// to reduce the race window. This gives in-flight message handlers time to
	// complete before the weaver is destroyed.
	{
		U_LOG_I("leiasr_destroy: pumping window messages before cleanup");
		MSG msg;
		// Process all pending messages (non-blocking)
		while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		// Small delay to let any in-flight handlers complete
		// The race window is very small, but mouse messages are high-frequency
		Sleep(50);
		// Pump again after the delay
		while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		U_LOG_I("leiasr_destroy: message pump complete");
	}

	// Mitigation 2: Explicitly destroy the weaver before deleting the context.
	// This ensures the window subclass is restored (via restoreOriginalWindowProc)
	// before the object memory is freed, reducing the race window further.
	if (leiasr->weaver != nullptr) {
		U_LOG_I("leiasr_destroy: explicitly destroying weaver");
		leiasr->weaver->destroy();
		leiasr->weaver = nullptr;
		U_LOG_I("leiasr_destroy: weaver destroyed");

		// Pump messages again after weaver destroy, since restoreOriginalWindowProc
		// was just called and there may be in-flight messages that got the old
		// instance pointer before the map was updated
		MSG msg;
		while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		Sleep(10);
		while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}

	// SwitchableLensHint is managed by SRContext — do NOT delete it manually.
	// SRContext::~SRContext() calls deleteAllSenses() which cleans it up.
	// Manually deleting it causes a crash (double-free).
	leiasr->lens_hint = nullptr;

	// Clean up context (this triggers weaver destruction in SR SDK)
	if (leiasr->context != nullptr) {
		U_LOG_I("leiasr_destroy: deleting SR context");
		delete leiasr->context;
		leiasr->context = nullptr;
		U_LOG_I("leiasr_destroy: SR context deleted");
	}

	delete leiasr;

	U_LOG_I("Destroyed leiasr instance");
}

void
leiasr_weave(struct leiasr *leiasr,
             VkCommandBuffer commandBuffer,
             VkImageView leftImageView,
             VkImageView rightImageView,
             VkRect2D viewport,
             int imageWidth,
             int imageHeight,
             VkFormat imageFormat,
             VkFramebuffer framebuffer,
             int framebufferWidth,
             int framebufferHeight,
             VkFormat framebufferFormat)
{
	if (leiasr == nullptr || leiasr->weaver == nullptr) {
		U_LOG_W("leiasr_weave called with null instance or weaver");
		return;
	}

	RECT rect = {};
	rect.left = viewport.offset.x;
	rect.top = viewport.offset.y;
	rect.right = rect.left + viewport.extent.width;
	rect.bottom = rect.top + viewport.extent.height;

	leiasr->weaver->setViewport(rect);
	leiasr->weaver->setScissorRect(rect);
	// If caller provides a command buffer, use it; otherwise use the
	// pre-allocated one from leiasr_create(). The weaver always needs a
	// valid command buffer — passing VK_NULL_HANDLE causes black screen.
	VkCommandBuffer cmd = (commandBuffer != VK_NULL_HANDLE)
	                          ? commandBuffer
	                          : leiasr->commandBuffer;
	leiasr->weaver->setCommandBuffer(cmd);
	leiasr->weaver->setInputViewTexture(leftImageView, rightImageView, imageWidth, imageHeight, imageFormat);
	// Set the output framebuffer for weaving. The weaver renders to the
	// application-provided framebuffer — it does not manage its own swapchain.
	// VK_NULL_HANDLE is only valid if setOutputFrameBuffer was called on a
	// prior frame and the framebuffer hasn't changed.
	if (framebuffer != VK_NULL_HANDLE) {
		leiasr->weaver->setOutputFrameBuffer(framebuffer, framebufferWidth, framebufferHeight, framebufferFormat);
	}
	leiasr->weaver->weave();
}

bool
leiasr_get_predicted_eye_positions(struct leiasr *leiasr, struct leiasr_eye_pair *out_eye_pos)
{
	if (leiasr == nullptr || out_eye_pos == nullptr) {
		return false;
	}

	if (leiasr->weaver == nullptr) {
		out_eye_pos->valid = false;
		return false;
	}

	// Get predicted eye positions from weaver's LookaroundFilter
	// The weaver returns positions in millimeters
	float leftEye[3], rightEye[3];
	leiasr->weaver->getPredictedEyePositions(leftEye, rightEye);

	// Convert from millimeters to meters
	out_eye_pos->eyes[0].x = leftEye[0] / 1000.0f;
	out_eye_pos->eyes[0].y = leftEye[1] / 1000.0f;
	out_eye_pos->eyes[0].z = leftEye[2] / 1000.0f;
	out_eye_pos->eyes[1].x = rightEye[0] / 1000.0f;
	out_eye_pos->eyes[1].y = rightEye[1] / 1000.0f;
	out_eye_pos->eyes[1].z = rightEye[2] / 1000.0f;
	out_eye_pos->count = 2;
	out_eye_pos->timestamp_ns = os_monotonic_get_ns();
	out_eye_pos->valid = true;

	// Heuristic: when SR SDK loses tracking, eye positions collapse to a single point.
	// Detect this by checking if inter-eye distance is near zero.
	float dx = rightEye[0] - leftEye[0];
	float dy = rightEye[1] - leftEye[1];
	float dz = rightEye[2] - leftEye[2];
	float dist_sq = dx * dx + dy * dy + dz * dz;
	out_eye_pos->is_tracking = (dist_sq > 1e-6f);

	return true;
}

bool
leiasr_has_weaver(struct leiasr *leiasr)
{
	if (leiasr == nullptr) {
		return false;
	}

	return leiasr->weaver != nullptr;
}

bool
leiasr_get_display_dimensions(struct leiasr *leiasr, struct leiasr_display_dimensions *out_dims)
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

void
leiasr_log_window_diagnostics(struct leiasr *leiasr, void *windowHandle)
{
	if (leiasr == nullptr || windowHandle == nullptr) {
		return;
	}

	HWND hwnd = (HWND)windowHandle;

	// Get window client area in screen coordinates
	RECT clientRect;
	GetClientRect(hwnd, &clientRect);
	POINT clientTopLeft = {clientRect.left, clientRect.top};
	POINT clientBottomRight = {clientRect.right, clientRect.bottom};
	ClientToScreen(hwnd, &clientTopLeft);
	ClientToScreen(hwnd, &clientBottomRight);

	int clientW = clientBottomRight.x - clientTopLeft.x;
	int clientH = clientBottomRight.y - clientTopLeft.y;

	// Compute window position relative to SR display
	int winOnDisplayX = clientTopLeft.x - leiasr->display_screen_left;
	int winOnDisplayY = clientTopLeft.y - leiasr->display_screen_top;

	// Get window rect (includes non-client area for comparison)
	RECT windowRect;
	GetWindowRect(hwnd, &windowRect);

	// Check DPI awareness
	UINT dpiX = 96, dpiY = 96;
	HDC hdc = GetDC(hwnd);
	if (hdc) {
		dpiX = GetDeviceCaps(hdc, LOGPIXELSX);
		dpiY = GetDeviceCaps(hdc, LOGPIXELSY);
		ReleaseDC(hwnd, hdc);
	}

	U_LOG_W("[diag] Window HWND=%p", windowHandle);
	U_LOG_W("[diag] Client area screen pos: (%d, %d) to (%d, %d) = %dx%d",
	        (int)clientTopLeft.x, (int)clientTopLeft.y,
	        (int)clientBottomRight.x, (int)clientBottomRight.y,
	        clientW, clientH);
	U_LOG_W("[diag] Window rect (incl borders): (%d, %d) to (%d, %d) = %dx%d",
	        (int)windowRect.left, (int)windowRect.top,
	        (int)windowRect.right, (int)windowRect.bottom,
	        (int)(windowRect.right - windowRect.left),
	        (int)(windowRect.bottom - windowRect.top));
	U_LOG_W("[diag] SR display rect: (%d, %d) to (%d, %d)",
	        leiasr->display_screen_left, leiasr->display_screen_top,
	        leiasr->display_screen_right, leiasr->display_screen_bottom);
	U_LOG_W("[diag] Window on SR display: (%d, %d), DPI: %ux%u (scale=%.1f%%)",
	        winOnDisplayX, winOnDisplayY, dpiX, dpiY, dpiX * 100.0f / 96.0f);
}

bool
leiasr_get_display_pixel_info(struct leiasr *leiasr,
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
leiasr_get_recommended_view_dimensions(struct leiasr *leiasr,
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
leiasr_get_window_metrics(struct leiasr *leiasr,
                           struct leiasr_window_metrics *out_metrics)
{
	if (leiasr == nullptr || out_metrics == nullptr) {
		if (out_metrics != nullptr) {
			out_metrics->valid = false;
		}
		return false;
	}

	memset(out_metrics, 0, sizeof(*out_metrics));

	if (leiasr->windowHandle == nullptr ||
	    !leiasr->display_pixel_dims_valid ||
	    !leiasr->display_dims_valid) {
		return false;
	}

	uint32_t disp_px_w = leiasr->display_pixel_width;
	uint32_t disp_px_h = leiasr->display_pixel_height;
	int32_t disp_left = leiasr->display_screen_left;
	int32_t disp_top = leiasr->display_screen_top;
	float disp_w_m = leiasr->display_width_m;
	float disp_h_m = leiasr->display_height_m;

	if (disp_px_w == 0 || disp_px_h == 0) {
		return false;
	}

	// Get window client rect
	RECT rect;
	if (!GetClientRect(leiasr->windowHandle, &rect)) {
		return false;
	}
	uint32_t win_px_w = static_cast<uint32_t>(rect.right - rect.left);
	uint32_t win_px_h = static_cast<uint32_t>(rect.bottom - rect.top);
	if (win_px_w == 0 || win_px_h == 0) {
		return false;
	}

	// Get window screen position
	POINT client_origin = {0, 0};
	ClientToScreen(leiasr->windowHandle, &client_origin);

	// Compute pixel size (meters per pixel)
	float pixel_size_x = disp_w_m / (float)disp_px_w;
	float pixel_size_y = disp_h_m / (float)disp_px_h;

	// Window physical size
	float win_w_m = (float)win_px_w * pixel_size_x;
	float win_h_m = (float)win_px_h * pixel_size_y;

	// Window center in pixels (relative to display origin in screen coords)
	float win_center_px_x = (float)(client_origin.x - disp_left) + (float)win_px_w / 2.0f;
	float win_center_px_y = (float)(client_origin.y - disp_top) + (float)win_px_h / 2.0f;

	// Display center in pixels
	float disp_center_px_x = (float)disp_px_w / 2.0f;
	float disp_center_px_y = (float)disp_px_h / 2.0f;

	// Window center offset in meters
	// X: +right (screen coords and eye coords both +right)
	// Y: negated because screen coords Y-down, eye coords Y-up
	float offset_x_m = (win_center_px_x - disp_center_px_x) * pixel_size_x;
	float offset_y_m = -((win_center_px_y - disp_center_px_y) * pixel_size_y);

	// Fill output
	out_metrics->display_width_m = disp_w_m;
	out_metrics->display_height_m = disp_h_m;
	out_metrics->display_pixel_width = disp_px_w;
	out_metrics->display_pixel_height = disp_px_h;
	out_metrics->display_screen_left = disp_left;
	out_metrics->display_screen_top = disp_top;

	out_metrics->window_pixel_width = win_px_w;
	out_metrics->window_pixel_height = win_px_h;
	out_metrics->window_screen_left = static_cast<int32_t>(client_origin.x);
	out_metrics->window_screen_top = static_cast<int32_t>(client_origin.y);

	out_metrics->window_width_m = win_w_m;
	out_metrics->window_height_m = win_h_m;
	out_metrics->window_center_offset_x_m = offset_x_m;
	out_metrics->window_center_offset_y_m = offset_y_m;

	out_metrics->valid = true;

	return true;
}

bool
leiasr_request_display_mode(struct leiasr *leiasr, bool enable_3d)
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
		U_LOG_W("SR display mode switched to %s", enable_3d ? "3D" : "2D");
		return true;
	} catch (...) {
		U_LOG_E("Failed to switch SR display mode to %s", enable_3d ? "3D" : "2D");
		return false;
	}
}

bool
leiasr_supports_display_mode_switch(struct leiasr *leiasr)
{
	if (leiasr == nullptr) {
		return false;
	}

	return leiasr->lens_hint != nullptr;
}

} // extern "C"
