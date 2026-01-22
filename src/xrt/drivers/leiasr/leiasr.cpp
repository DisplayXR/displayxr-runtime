// Copyright 2024, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Leia SR (Simulated Reality) SDK integration for eye tracking and weaving.
 * @ingroup drv_leiasr
 */

#include "leiasr.h"
#include "util/u_misc.h"
#include "util/u_logging.h"
#include "os/os_time.h"

#include <sr/weaver/vkweaver.h>
#include <sr/world/display/display.h>
#include <sr/utility/exception.h>

#ifdef XRT_HAVE_LEIA_SR_SENSE
#include <sr/sense/eyetracker/eyetracker.h>
#include <sr/sense/eyetracker/eyepairlistener.h>
#endif

#include <windows.h>
#include <sysinfoapi.h>

#include <mutex>
#include <atomic>
#include <iostream>

// Forward declaration
struct leiasr;

#ifdef XRT_HAVE_LEIA_SR_SENSE
/*!
 * Listener class that receives eye pair updates from the SR SDK.
 * Converts positions from millimeters to meters and stores them thread-safely.
 */
class LeiaEyePairListener : public SR::EyePairListener
{
public:
	LeiaEyePairListener(struct leiasr *owner) : owner_(owner) {}

	void accept(const SR_eyePair &eyePair) override;

private:
	struct leiasr *owner_;
};
#endif

/*!
 * Main structure holding SR context, weaver, and eye tracking state.
 */
struct leiasr
{
	// Vulkan resources (only used when weaver is created)
	VkDevice device = nullptr;
	VkPhysicalDevice physicalDevice = nullptr;
	VkQueue graphicsQueue = nullptr;
	VkCommandPool commandPool = nullptr;
	VkCommandBuffer commandBuffer = nullptr;

	// SR SDK objects
	SR::SRContext *context = nullptr;
	SR::IVulkanWeaver1 *weaver = nullptr;

#ifdef XRT_HAVE_LEIA_SR_SENSE
	// Eye tracking objects
	SR::EyeTracker *eyeTracker = nullptr;
	LeiaEyePairListener *eyeListener = nullptr;
	SR::EyePairStream *eyeStream = nullptr;
#endif

	// Thread-safe eye position storage
	std::mutex eyeMutex;
	leiasr_eye_pair latestEyePair = {};
	std::atomic<bool> eyeTrackingActive{false};

	// True if this instance was created for eye tracking only (no Vulkan)
	bool eyeTrackerOnly = false;

	/*!
	 * Update the latest eye positions (called from listener thread).
	 */
	void
	updateEyePositions(const leiasr_eye_pair &pair)
	{
		std::lock_guard<std::mutex> lock(eyeMutex);
		latestEyePair = pair;
	}

	/*!
	 * Get the latest eye positions (thread-safe).
	 */
	leiasr_eye_pair
	getEyePositions()
	{
		std::lock_guard<std::mutex> lock(eyeMutex);
		return latestEyePair;
	}
};

#ifdef XRT_HAVE_LEIA_SR_SENSE
// Implementation of LeiaEyePairListener::accept
void
LeiaEyePairListener::accept(const SR_eyePair &eyePair)
{
	// Convert from millimeters to meters
	leiasr_eye_pair pair;
	pair.left.x = eyePair.left.x / 1000.0f;
	pair.left.y = eyePair.left.y / 1000.0f;
	pair.left.z = eyePair.left.z / 1000.0f;
	pair.right.x = eyePair.right.x / 1000.0f;
	pair.right.y = eyePair.right.y / 1000.0f;
	pair.right.z = eyePair.right.z / 1000.0f;
	pair.timestamp_ns = os_monotonic_get_ns();
	pair.valid = true;

	owner_->updateEyePositions(pair);
}
#endif

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

xrt_result_t
leiasr_create_eye_tracker_only(double maxTime, struct leiasr **out)
{
#ifdef XRT_HAVE_LEIA_SR_SENSE
	leiasr *sr = new leiasr;
	sr->eyeTrackerOnly = true;

	if (!CreateSRContext(maxTime, *sr)) {
		U_LOG_E("Failed to create SR context for eye tracking");
		delete sr;
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	sr->context->initialize();

	*out = sr;

	U_LOG_I("Created leiasr instance for eye tracking only");

	return XRT_SUCCESS;
#else
	(void)maxTime;
	*out = nullptr;
	U_LOG_W("Eye tracking not available - SimulatedRealitySense library not found at build time");
	return XRT_ERROR_DEVICE_CREATION_FAILED;
#endif
}

void
leiasr_destroy(struct leiasr *leiasr)
{
	if (leiasr == nullptr) {
		return;
	}

	// Stop eye tracking first
	leiasr_eye_tracker_stop(leiasr);

	// Clean up weaver (if created)
	if (leiasr->weaver != nullptr) {
		// SR SDK handles weaver cleanup internally
		leiasr->weaver = nullptr;
	}

	// Clean up context
	if (leiasr->context != nullptr) {
		delete leiasr->context;
		leiasr->context = nullptr;
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

xrt_result_t
leiasr_eye_tracker_start(struct leiasr *leiasr)
{
#ifdef XRT_HAVE_LEIA_SR_SENSE
	if (leiasr == nullptr) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	if (leiasr->context == nullptr) {
		U_LOG_E("Cannot start eye tracker: SR context is null");
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	if (leiasr->eyeTrackingActive.load()) {
		U_LOG_W("Eye tracker already active");
		return XRT_SUCCESS;
	}

	try {
		// Create eye tracker from context
		leiasr->eyeTracker = SR::EyeTracker::create(*leiasr->context);
		if (leiasr->eyeTracker == nullptr) {
			U_LOG_E("Failed to create SR EyeTracker");
			return XRT_ERROR_DEVICE_CREATION_FAILED;
		}

		// Create listener
		leiasr->eyeListener = new LeiaEyePairListener(leiasr);

		// Open eye pair stream with listener
		leiasr->eyeStream = leiasr->eyeTracker->openEyePairStream(*leiasr->eyeListener);
		if (leiasr->eyeStream == nullptr) {
			U_LOG_E("Failed to open SR eye pair stream");
			delete leiasr->eyeListener;
			leiasr->eyeListener = nullptr;
			delete leiasr->eyeTracker;
			leiasr->eyeTracker = nullptr;
			return XRT_ERROR_DEVICE_CREATION_FAILED;
		}

		leiasr->eyeTrackingActive.store(true);

		U_LOG_I("SR Eye tracking started successfully");

		return XRT_SUCCESS;
	} catch (const std::exception &e) {
		U_LOG_E("Exception starting eye tracker: %s", e.what());
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
#else
	(void)leiasr;
	U_LOG_W("Eye tracking not available - SimulatedRealitySense library not found at build time");
	return XRT_ERROR_DEVICE_CREATION_FAILED;
#endif
}

void
leiasr_eye_tracker_stop(struct leiasr *leiasr)
{
	if (leiasr == nullptr) {
		return;
	}

	if (!leiasr->eyeTrackingActive.load()) {
		return;
	}

	leiasr->eyeTrackingActive.store(false);

#ifdef XRT_HAVE_LEIA_SR_SENSE
	// Close the stream first
	if (leiasr->eyeStream != nullptr) {
		delete leiasr->eyeStream;
		leiasr->eyeStream = nullptr;
	}

	// Delete listener
	if (leiasr->eyeListener != nullptr) {
		delete leiasr->eyeListener;
		leiasr->eyeListener = nullptr;
	}

	// Delete eye tracker
	if (leiasr->eyeTracker != nullptr) {
		delete leiasr->eyeTracker;
		leiasr->eyeTracker = nullptr;
	}
#endif

	// Clear the latest eye pair
	{
		std::lock_guard<std::mutex> lock(leiasr->eyeMutex);
		leiasr->latestEyePair = {};
	}

	U_LOG_I("SR Eye tracking stopped");
}

bool
leiasr_get_eye_positions(struct leiasr *leiasr, struct leiasr_eye_pair *out_eye_pair)
{
	if (leiasr == nullptr || out_eye_pair == nullptr) {
		return false;
	}

	if (!leiasr->eyeTrackingActive.load()) {
		out_eye_pair->valid = false;
		return false;
	}

	*out_eye_pair = leiasr->getEyePositions();
	return out_eye_pair->valid;
}

bool
leiasr_is_eye_tracking_active(struct leiasr *leiasr)
{
	if (leiasr == nullptr) {
		return false;
	}

	return leiasr->eyeTrackingActive.load();
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
