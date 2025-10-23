#include "leiasr.h"
#include "util/u_misc.h"

#include <sr/management/srcontext.h>
#include <sr/world/display/display.h>
#include <sr/utility/exception.h>

#include <windows.h>
#include <sysinfoapi.h>

struct leiasr {
    SR::SRContext* context = nullptr;
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

} // namespace anonymous

extern "C" {

xrt_result_t leiasr_create(struct leiasr** out)
{
    *out = new leiasr;
    CreateSRContext(999.0, **out);
    return XRT_SUCCESS;
}

void leiasr_destroy(struct leiasr* leiasr)
{
    delete leiasr;
}

} // extern "C"
