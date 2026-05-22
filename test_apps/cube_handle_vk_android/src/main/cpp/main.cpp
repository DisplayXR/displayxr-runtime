// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// cube_handle_vk_android entry point. Wires the Khronos OpenXR loader
// (`xrInitializeLoaderKHR` + `xrCreateInstance`) and logs the runtime
// the loader binds to. No Vulkan / session / cube renderer yet — that
// lands in follow-up commits. The point of this step is to prove the
// loader can find our runtime APK via the
// org.khronos.openxr.OpenXRRuntimeService intent.

#include <android/log.h>
#include <android_native_app_glue.h>

#define XR_USE_PLATFORM_ANDROID
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstring>

#define LOG_TAG "cube_handle_vk_android"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

const char *
xr_result_str(XrResult r)
{
	// Just the codes we expect to see; anything else falls through to a
	// numeric format in the caller.
	switch (r) {
	case XR_SUCCESS:                            return "XR_SUCCESS";
	case XR_ERROR_RUNTIME_FAILURE:              return "XR_ERROR_RUNTIME_FAILURE";
	case XR_ERROR_RUNTIME_UNAVAILABLE:          return "XR_ERROR_RUNTIME_UNAVAILABLE";
	case XR_ERROR_INSTANCE_LOST:                return "XR_ERROR_INSTANCE_LOST";
	case XR_ERROR_INITIALIZATION_FAILED:        return "XR_ERROR_INITIALIZATION_FAILED";
	case XR_ERROR_API_VERSION_UNSUPPORTED:      return "XR_ERROR_API_VERSION_UNSUPPORTED";
	case XR_ERROR_EXTENSION_NOT_PRESENT:        return "XR_ERROR_EXTENSION_NOT_PRESENT";
	case XR_ERROR_API_LAYER_NOT_PRESENT:        return "XR_ERROR_API_LAYER_NOT_PRESENT";
	case XR_ERROR_OUT_OF_MEMORY:                return "XR_ERROR_OUT_OF_MEMORY";
	case XR_ERROR_FUNCTION_UNSUPPORTED:         return "XR_ERROR_FUNCTION_UNSUPPORTED";
	case XR_ERROR_VALIDATION_FAILURE:           return "XR_ERROR_VALIDATION_FAILURE";
	default:                                    return nullptr;
	}
}

void
log_xr_result(const char *what, XrResult r)
{
	const char *name = xr_result_str(r);
	if (name != nullptr) {
		LOGI("%s -> %s", what, name);
	} else {
		LOGI("%s -> XrResult(%d)", what, (int)r);
	}
}

XrInstance g_instance = XR_NULL_HANDLE;

// Wire the Khronos OpenXR loader's Android-specific init. Required on
// Android because the loader needs the JavaVM + Activity context to
// discover the runtime APK via the OpenXRRuntimeService intent.
bool
initialize_loader(struct android_app *app)
{
	PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = nullptr;
	XrResult res = xrGetInstanceProcAddr(
	    XR_NULL_HANDLE, "xrInitializeLoaderKHR",
	    reinterpret_cast<PFN_xrVoidFunction *>(&xrInitializeLoaderKHR));
	if (res != XR_SUCCESS || xrInitializeLoaderKHR == nullptr) {
		LOGE("xrGetInstanceProcAddr(xrInitializeLoaderKHR) failed (%d)", (int)res);
		return false;
	}

	XrLoaderInitInfoAndroidKHR loader_init = {};
	loader_init.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR;
	loader_init.applicationVM = app->activity->vm;
	loader_init.applicationContext = app->activity->clazz;
	res = xrInitializeLoaderKHR(
	    reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR *>(&loader_init));
	log_xr_result("xrInitializeLoaderKHR", res);
	return res == XR_SUCCESS;
}

bool
create_instance(struct android_app *app)
{
	const char *extensions[] = {
	    XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
	};

	XrInstanceCreateInfoAndroidKHR android_info = {};
	android_info.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
	android_info.applicationVM = app->activity->vm;
	android_info.applicationActivity = app->activity->clazz;

	XrInstanceCreateInfo create_info = {};
	create_info.type = XR_TYPE_INSTANCE_CREATE_INFO;
	create_info.next = &android_info;
	std::strncpy(create_info.applicationInfo.applicationName,
	             "cube_handle_vk_android",
	             XR_MAX_APPLICATION_NAME_SIZE - 1);
	create_info.applicationInfo.applicationVersion = 1;
	std::strncpy(create_info.applicationInfo.engineName, "displayxr",
	             XR_MAX_ENGINE_NAME_SIZE - 1);
	create_info.applicationInfo.engineVersion = 1;
	create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
	create_info.enabledExtensionCount = 1;
	create_info.enabledExtensionNames = extensions;

	XrResult res = xrCreateInstance(&create_info, &g_instance);
	log_xr_result("xrCreateInstance", res);
	if (res != XR_SUCCESS) {
		return false;
	}

	XrInstanceProperties props = {};
	props.type = XR_TYPE_INSTANCE_PROPERTIES;
	res = xrGetInstanceProperties(g_instance, &props);
	if (res == XR_SUCCESS) {
		LOGI("Runtime: \"%s\" v%u.%u.%u",
		     props.runtimeName,
		     XR_VERSION_MAJOR(props.runtimeVersion),
		     XR_VERSION_MINOR(props.runtimeVersion),
		     XR_VERSION_PATCH(props.runtimeVersion));
	} else {
		LOGW("xrGetInstanceProperties failed (%d)", (int)res);
	}
	return true;
}

void
destroy_instance()
{
	if (g_instance != XR_NULL_HANDLE) {
		XrResult res = xrDestroyInstance(g_instance);
		log_xr_result("xrDestroyInstance", res);
		g_instance = XR_NULL_HANDLE;
	}
}

void
handle_cmd(struct android_app *app, int32_t cmd)
{
	switch (cmd) {
	case APP_CMD_INIT_WINDOW:
		LOGI("APP_CMD_INIT_WINDOW (window=%p)", app->window);
		// xrCreateInstance is deferred until the window exists because
		// some runtimes inspect the Activity surface state during create.
		// Idempotent — guarded by g_instance.
		if (g_instance == XR_NULL_HANDLE) {
			create_instance(app);
		}
		break;
	case APP_CMD_TERM_WINDOW:
		LOGI("APP_CMD_TERM_WINDOW");
		break;
	case APP_CMD_GAINED_FOCUS:
		LOGI("APP_CMD_GAINED_FOCUS");
		break;
	case APP_CMD_LOST_FOCUS:
		LOGI("APP_CMD_LOST_FOCUS");
		break;
	case APP_CMD_DESTROY:
		LOGI("APP_CMD_DESTROY");
		destroy_instance();
		break;
	default:
		break;
	}
}

} // namespace

extern "C" void
android_main(struct android_app *app)
{
	LOGI("cube_handle_vk_android: android_main entered");
	app->onAppCmd = handle_cmd;

	if (!initialize_loader(app)) {
		LOGE("OpenXR loader init failed; the loop continues but no XR calls will work");
	}

	while (true) {
		int events;
		struct android_poll_source *source;
		while (ALooper_pollAll(0, nullptr, &events, (void **)&source) >= 0) {
			if (source != nullptr) {
				source->process(app, source);
			}
			if (app->destroyRequested != 0) {
				LOGI("destroyRequested — exiting android_main");
				destroy_instance();
				return;
			}
		}
	}
}
