// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// Skeleton entry point for cube_handle_vk_android. Day-8 just opens a
// NativeActivity and logs lifecycle events — proves the test-app APK
// builds + installs alongside the runtime APK without taking on the
// OpenXR loader + Vulkan setup yet. Those land in follow-up commits.

#include <android/log.h>
#include <android_native_app_glue.h>

#define LOG_TAG "cube_handle_vk_android"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

static void
handle_cmd(struct android_app *app, int32_t cmd)
{
	switch (cmd) {
	case APP_CMD_INIT_WINDOW:
		LOGI("APP_CMD_INIT_WINDOW (window=%p)", app->window);
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
		break;
	default:
		break;
	}
}

extern "C" void
android_main(struct android_app *app)
{
	LOGI("cube_handle_vk_android: android_main entered");
	app->onAppCmd = handle_cmd;

	while (true) {
		int events;
		struct android_poll_source *source;
		while (ALooper_pollAll(0, nullptr, &events, (void **)&source) >= 0) {
			if (source != nullptr) {
				source->process(app, source);
			}
			if (app->destroyRequested != 0) {
				LOGI("destroyRequested — exiting android_main");
				return;
			}
		}
	}
}
