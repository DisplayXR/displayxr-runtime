#include "leiasr.h"
#include "util/u_misc.h"

#include <sr/weaver/vkweaver.h>
#include <sr/world/display/display.h>
#include <sr/utility/exception.h>

#include <windows.h>
#include <sysinfoapi.h>

struct leiasr
{
	VkDevice            device         = nullptr;
	VkPhysicalDevice    physicalDevice = nullptr;
	VkQueue             graphicsQueue  = nullptr;
	VkCommandPool       commandPool    = nullptr;
	VkCommandBuffer     commandBuffer  = nullptr;
	SR::SRContext*      context        = nullptr;
	SR::IVulkanWeaver1* weaver         = nullptr;
};

namespace {

bool CreateSRContext(double maxTime, leiasr& sr)
{
    const double startTime = (double)GetTickCount64() / 1000.0;

    // Create SR context.
    while (sr.context == nullptr)
    {
        try
        {
            sr.context = SR::SRContext::create();
            break;
        }
        catch (SR::ServerNotAvailableException e)
        {
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
    while (sr.context && !displayReady)
    {
        // Attempt to get display.
        SR::Display* display = SR::Display::create(*sr.context);
        if (display != nullptr)
        {
            // Get the display location, and when it is valid, the device is ready.
            SR_recti displayLocation = display->getLocation();
            int64_t width = displayLocation.right - displayLocation.left;
            int64_t height = displayLocation.bottom - displayLocation.top;
            if ((width != 0) && (height != 0))
            {
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

    //! [SR Initialization]

    // Return if we have a valid context and device is ready.
    return (sr.context != nullptr) && displayReady;
}

bool CreateSRWeaver(SR::SRContext* context, VkDevice device, VkPhysicalDevice physicalDevice, VkQueue graphicsQueue, VkCommandPool commandPool, HWND hWnd, leiasr* out)
{
	// Create weaver.
	WeaverErrorCode createWeaverResult = SR::CreateVulkanWeaver(*context, device, physicalDevice, graphicsQueue, commandPool, hWnd, &out->weaver);
	if (createWeaverResult != WeaverErrorCode::WeaverSuccess)
	{
		assert(false);
		return false;
	}

	return true;
}

} // namespace anonymous

extern "C" {

xrt_result_t leiasr_create(double maxTime, VkDevice device, VkPhysicalDevice physicalDevice, VkQueue graphicsQueue, VkCommandPool commandPool, struct leiasr **out)
{
	leiasr* sr = new leiasr;

	if (!CreateSRContext(maxTime, *sr))
	{
		assert(false);
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

	if (!CreateSRWeaver(sr->context, device, physicalDevice, graphicsQueue, commandPool, NULL, sr))
	{
		assert(false);
		return XRT_ERROR_VULKAN;
	}

	sr->context->initialize();

	*out = sr;

	return XRT_SUCCESS;
}

void leiasr_destroy(struct leiasr* leiasr)
{
    delete leiasr;
}

void leiasr_weave(struct leiasr* leiasr, VkCommandBuffer commandBuffer, VkImageView leftImageView, VkImageView rightImageView, VkRect2D viewport, int imageWidth, int imageHeight, VkFormat imageFormat, VkFramebuffer framebuffer, int framebufferWidth, int framebufferHeight, VkFormat framebufferFormat)
{
	RECT rect = {};
	rect.left   = viewport.offset.x;
	rect.top    = viewport.offset.y;
	rect.right  = rect.left + viewport.extent.width;
	rect.bottom = rect.top  + viewport.extent.height;

	leiasr->weaver->setViewport(rect);
	leiasr->weaver->setScissorRect(rect);
	leiasr->weaver->setCommandBuffer(commandBuffer);
	leiasr->weaver->setOutputFrameBuffer(framebuffer, framebufferWidth, framebufferHeight, framebufferFormat);
	leiasr->weaver->setInputViewTexture(leftImageView, rightImageView, imageWidth, imageHeight, imageFormat);
	leiasr->weaver->weave();
}

} // extern "C"
