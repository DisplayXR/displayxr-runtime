// Copyright 2024, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Leia SR (Simulated Reality) SDK integration for weaving.
 * @ingroup drv_leiasr
 */

#include "leiasr.h"
#include "util/u_misc.h"
#include "util/u_logging.h"
#include "os/os_time.h"

#include <sr/weaver/vkweaver.h>
#include <sr/world/display/display.h>
#include <sr/utility/exception.h>

#include <windows.h>
#include <sysinfoapi.h>

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

	// Wait for display to be ready.
	bool displayReady = false;
	while (sr.context && !displayReady) {
		// Attempt to get display.
		SR::Display *display = SR::Display::create(*sr.context);
		if (display != nullptr) {
			// Get the display location, and when it is valid, the device is ready.
			SR_recti displayLocation = display->getLocation();
			int64_t width = displayLocation.right - displayLocation.left;
			int64_t height = displayLocation.bottom - displayLocation.top;
			if ((width != 0) && (height != 0)) {
				displayReady = true;
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

	// Pass windowHandle to CreateSRWeaver: NULL = fullscreen mode, valid HWND = windowed mode
	if (!CreateSRWeaver(sr->context, device, physicalDevice, graphicsQueue, commandPool, (HWND)windowHandle, sr)) {
		U_LOG_E("Failed to create SR weaver");
		delete sr;
		return XRT_ERROR_VULKAN;
	}

	sr->context->initialize();

	*out = sr;

	U_LOG_I("Created leiasr instance with weaver for HWND %p", windowHandle);

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
	leiasr->weaver->setCommandBuffer(commandBuffer);
	leiasr->weaver->setOutputFrameBuffer(framebuffer, framebufferWidth, framebufferHeight, framebufferFormat);
	leiasr->weaver->setInputViewTexture(leftImageView, rightImageView, imageWidth, imageHeight, imageFormat);
	leiasr->weaver->weave();
}

bool
leiasr_get_predicted_eye_positions(struct leiasr *leiasr, struct leiasr_eye_pair *out_eye_pair)
{
	if (leiasr == nullptr || out_eye_pair == nullptr) {
		return false;
	}

	if (leiasr->weaver == nullptr) {
		out_eye_pair->valid = false;
		return false;
	}

	// Get predicted eye positions from weaver's LookaroundFilter
	// The weaver returns positions in millimeters
	float leftEye[3], rightEye[3];
	leiasr->weaver->getPredictedEyePositions(leftEye, rightEye);

	// Convert from millimeters to meters
	out_eye_pair->left.x = leftEye[0] / 1000.0f;
	out_eye_pair->left.y = leftEye[1] / 1000.0f;
	out_eye_pair->left.z = leftEye[2] / 1000.0f;
	out_eye_pair->right.x = rightEye[0] / 1000.0f;
	out_eye_pair->right.y = rightEye[1] / 1000.0f;
	out_eye_pair->right.z = rightEye[2] / 1000.0f;
	out_eye_pair->timestamp_ns = os_monotonic_get_ns();
	out_eye_pair->valid = true;

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

} // extern "C"
